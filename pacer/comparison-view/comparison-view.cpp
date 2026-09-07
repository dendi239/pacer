#include "comparison-view.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <tuple>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "implot.h"

namespace {

ImPlotPoint Vec3fToPoint(int index, void *data) {
  auto s = reinterpret_cast<pacer::Vec3f *>(data)[index];
  return {s[0], s[1]};
}

// Interpolated sample at distance `d` along the lap's own cum_distances;
// nullopt when `d` falls outside the lap.
std::optional<pacer::GPSSample> SampleAtDistance(const pacer::Lap &lap,
                                                 double d) {
  const auto &dist = lap.cum_distances;
  if (dist.size() < 2 || dist.size() != lap.points.size() ||
      d < dist.front() || d > dist.back()) {
    return std::nullopt;
  }
  size_t i = std::lower_bound(dist.begin(), dist.end(), d) - dist.begin();
  if (i == 0) {
    return lap.points.front();
  }
  double span = dist[i] - dist[i - 1];
  double t = span > 0 ? (d - dist[i - 1]) / span : 0.0;
  return pacer::Interpolate(lap.points[i - 1], lap.points[i], t);
}

// Time lost/gained by `lap` versus `best` at distance `d` along the best
// lap, interpolated the same way the delta plot renders it (both laps are
// resampled against the same gates, so equal indices are comparable).
std::optional<double> DeltaAtDistance(const pacer::Lap &lap,
                                      const pacer::Lap &best, double d) {
  size_t count = std::min(lap.points.size(), best.points.size());
  if (count < 2 || count > best.cum_distances.size() ||
      d < best.cum_distances.front() || d > best.cum_distances[count - 1]) {
    return std::nullopt;
  }
  auto begin = best.cum_distances.begin();
  size_t i = std::lower_bound(begin, begin + count, d) - begin;
  i = std::max<size_t>(i, 1);
  auto delta_at = [&](size_t j) {
    double lap_time =
        (lap.points[j].timestamp_ms - lap.points[0].timestamp_ms) / 1000.0;
    double best_time =
        (best.points[j].timestamp_ms - best.points[0].timestamp_ms) / 1000.0;
    return lap_time - best_time;
  };
  double span = best.cum_distances[i] - best.cum_distances[i - 1];
  double t = span > 0 ? (d - best.cum_distances[i - 1]) / span : 0.0;
  return delta_at(i - 1) * (1 - t) + delta_at(i) * t;
}

// Nearest point of the gates' middle line to `p`, as a fractional gate
// index (k + t between gates k and k+1). Nullopt when `p` is farther from
// the middle line than the local gate half-width, i.e. outside the track.
std::optional<double>
ProjectOntoMidline(const std::vector<pacer::Segment> &gates, pacer::Point p) {
  if (gates.size() < 2) {
    return std::nullopt;
  }
  auto mid = [](const pacer::Segment &g) { return (g.first + g.second) / 2.0; };
  auto half_width = [](const pacer::Segment &g) {
    return std::sqrt((g.second - g.first).Norm()) / 2.0;
  };

  double best_dist2 = std::numeric_limits<double>::infinity();
  double best_pos = 0.0, best_half_width = 0.0;
  for (size_t k = 0; k + 1 < gates.size(); ++k) {
    pacer::Point m0 = mid(gates[k]), m1 = mid(gates[k + 1]);
    pacer::Point dir = m1 - m0;
    double len2 = dir.Norm();
    double t =
        len2 > 1e-12 ? std::clamp((p - m0).Scalar(dir) / len2, 0.0, 1.0) : 0.0;
    double dist2 = (p - pacer::Interpolate(m0, m1, t)).Norm();
    if (dist2 < best_dist2) {
      best_dist2 = dist2;
      best_pos = static_cast<double>(k) + t;
      best_half_width =
          half_width(gates[k]) * (1 - t) + half_width(gates[k + 1]) * t;
    }
  }
  if (best_dist2 > best_half_width * best_half_width) {
    return std::nullopt;
  }
  return best_pos;
}

// Plots the reference track as its outline: one polyline per annotated
// track edge (the gates' first/second endpoints keep consistent sides, see
// track_annotator's overlay), joined back to the start when the annotation
// wraps around into a closed circuit.
void PlotTrackOutline(const pacer::ReferenceTrack &track,
                      const pacer::CoordinateSystem &cs) {
  size_t n = track.segments.size();
  if (n < 2) {
    return;
  }

  auto to_local = [&](const pacer::Point &ref_local) {
    pacer::Vec3f p =
        cs.Local(track.cs.Global(pacer::Vec3f{ref_local.x, ref_local.y, 0}));
    return pacer::Point{p[0], p[1]};
  };

  std::vector<pacer::Point> left, right, mids;
  left.reserve(n + 1);
  right.reserve(n + 1);
  mids.reserve(n);
  for (const pacer::Segment &seg : track.segments) {
    left.push_back(to_local(seg.first));
    right.push_back(to_local(seg.second));
    mids.push_back((left.back() + right.back()) / 2.0);
  }

  // Closed-ness is not stored in the track file; treat the track as a
  // closed circuit when the wraparound gap is within 1.5x the largest
  // annotated gate spacing (Norm() is squared, hence the squared factor).
  double max_step_sq = 0;
  for (size_t i = 0; i + 1 < n; ++i) {
    max_step_sq = std::max(max_step_sq, (mids[i + 1] - mids[i]).Norm());
  }
  if ((mids.front() - mids.back()).Norm() <= max_step_sq * (1.5 * 1.5)) {
    left.push_back(left.front());
    right.push_back(right.front());
  }

  auto plot_edge = [](const std::vector<pacer::Point> &edge) {
    ImPlot::SetNextLineStyle(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), 2.0f);
    ImPlot::PlotLineG(
        "track",
        [](int index, void *data) -> ImPlotPoint {
          const auto &pts = *reinterpret_cast<std::vector<pacer::Point> *>(data);
          return ImPlotPoint{pts[index].x, pts[index].y};
        },
        (void *)&edge, (int)edge.size());
  };
  plot_edge(left);
  plot_edge(right);
}

} // namespace

namespace pacer {

ComparisonView::ComparisonView(Comparison *comparison)
    : comparison{comparison} {
  Update();
}

ImVec4 ComparisonView::LapColor(int index) {
  // GetColormapColor wraps, so a long comparison reuses colours rather than
  // running out -- at which point the chips' labels are what tell laps apart.
  return ImPlot::GetColormapColor(index);
}

void ComparisonView::Invalidate() {
  resample_frame_ = -1;
  plots_need_fit_ = true;
}

void ComparisonView::Update() {
  if (comparison->track_path == adopted_track_path_)
    return;
  adopted_track_path_ = comparison->track_path;
  cs = comparison->track.cs;
  map_needs_fit_ = true;
  Invalidate();
}

void ComparisonView::SetHoverDistance(double distance) {
  hover_distance_ = distance;
  hover_frame_ = ImGui::GetFrameCount();
}

std::optional<double> ComparisonView::HoverDistance() const {
  // Accept the previous frame too: the producing window may be drawn after
  // the consuming one within a frame.
  if (hover_frame_ < ImGui::GetFrameCount() - 1) {
    return std::nullopt;
  }
  return hover_distance_;
}

void ComparisonView::RefreshResampled(const Session &session) {
  if (resample_frame_ == ImGui::GetFrameCount() &&
      resampled_lap_count_ == comparison->laps.size()) {
    return;
  }
  resample_frame_ = ImGui::GetFrameCount();
  resampled_lap_count_ = comparison->laps.size();

  resampled_.assign(comparison->laps.size(), Lap{});
  best_slot_ = -1;
  double best_time = 0;
  for (size_t slot = 0; slot < comparison->laps.size(); ++slot) {
    std::optional<Lap> lap = session.ResolveLap(comparison->laps[slot]);
    if (!lap)
      continue;
    double time = lap->LapTime();
    resampled_[slot] = comparison->track.Resample(*lap);
    if (time > 0 && (best_slot_ < 0 || time < best_time)) {
      best_slot_ = (int)slot;
      best_time = time;
    }
  }
}

//--------------------------------- CHIPS -----------------------------------//

LapRef ComparisonView::DrawLapChips(const Session &session) {
  LapRef dropped;
  for (size_t slot = 0; slot < comparison->laps.size(); ++slot) {
    const LapRef ref = comparison->laps[slot];
    if (slot > 0)
      ImGui::SameLine();
    ImGui::PushID((int)slot);

    // The colour swatch is what ties the chip to its trace on the plots;
    // the label carries the identity, so colour is never doing it alone.
    ImGui::ColorButton("##color", LapColor((int)slot),
                       ImGuiColorEditFlags_NoTooltip |
                           ImGuiColorEditFlags_NoDragDrop,
                       ImVec2(ImGui::GetTextLineHeight(),
                              ImGui::GetTextLineHeight()));
    ImGui::SameLine(0, 4);

    std::string label = ref.Label();
    if (std::optional<Lap> lap = session.ResolveLap(ref)) {
      label += "  " + FormatLapTime(lap->LapTime());
    } else {
      label += "  (gone)";
    }
    // ASCII only: the default font has no star, and a missing glyph reads
    // as a stray box rather than as emphasis.
    if ((int)slot == best_slot_)
      label += "  (best)";
    ImGui::TextUnformatted(label.c_str());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
      const Source *source = session.Find(ref.source_id);
      ImGui::SetTooltip("%s, lap %d%s\nRight-click to remove",
                        source ? source->name.c_str() : "(removed source)",
                        ref.lap_index,
                        (int)slot == best_slot_ ? "\nQuickest here" : "");
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      dropped = ref;
    }

    ImGui::PopID();
  }
  return dropped;
}

//------------------------------ SPEED / DELTA ------------------------------//

void ComparisonView::Display(Session &session) {
  // The whole window is a drop target, so a lap can be let go anywhere over
  // the comparison rather than onto a particular strip of it.
  bool dnd = ImGui::BeginDragDropTargetCustom(
      ImGui::GetCurrentWindow()->Rect(), ImGui::GetID("##comparison_drop"));
  if (dnd) {
    if (const ImGuiPayload *payload =
            ImGui::AcceptDragDropPayload(kLapDragPayload)) {
      LapRef ref = *reinterpret_cast<const LapRef *>(payload->Data);
      if (session.AddLap(comparison, ref)) {
        Invalidate();
      }
    }
  }

  if (LapRef remove = DrawLapChips(session); remove.Valid()) {
    session.RemoveLap(comparison, remove);
    Invalidate();
  }

  if (comparison->laps.empty()) {
    ImGui::TextWrapped(
        "Drag laps here from a source's lap chart or lap table. The first "
        "one sets the track this comparison is on.");
    if (dnd)
      ImGui::EndDragDropTarget();
    return;
  }

  RefreshResampled(session);

  // Derived from the theme's text color so the cursor line and its tag stay
  // readable against both a dark and a light plot background.
  ImVec4 cursor_color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
  cursor_color.w = 0.8f;

  // Draws the shared hover cursor: a vertical line at the hovered distance
  // plus a distance tag on the x-axis. Call inside a plot.
  auto plot_cursor = [&](double d) {
    double x = d;
    ImPlot::SetNextLineStyle(cursor_color, 1.0f);
    ImPlot::PlotInfLines("##hover", &x, 1);
    ImPlot::TagX(x, cursor_color, "%.0fm", x);
  };

  // Hover annotations would overlay each other when laps have close values
  // at the hovered distance, so alternate them around the cursor: laps in
  // even slots go left, odd slots go right. The side is per lap, so it
  // matches between the speed and delta plots.
  auto annotation_offset = [](size_t slot) {
    return ImVec2(slot % 2 == 0 ? -8.0f : 8.0f, -8.0f);
  };

  const int slots = (int)resampled_.size();

  if (ImPlot::BeginSubplots("", 2, 1, ImVec2(-1, -1),
                            ImPlotSubplotFlags_LinkAllX)) {
    if (plots_need_fit_) {
      ImPlot::SetNextAxesToFit();
    }
    if (ImPlot::BeginPlot("Telemetry", ImVec2())) {
      ImPlot::SetupAxis(ImAxis_X1, "", ImPlotAxisFlags_NoTickLabels);

      for (int slot = 0; slot < slots; ++slot) {
        const Lap &lap = resampled_[slot];
        if (lap.Count() == 0)
          continue;
        ImPlot::SetNextLineStyle(LapColor(slot));
        ImPlot::PlotLineG(
            comparison->laps[slot].Label().c_str(),
            [](int index, void *data) -> ImPlotPoint {
              const auto &lap = *reinterpret_cast<const pacer::Lap *>(data);
              return ImPlotPoint{lap.cum_distances[index],
                                 lap.points[index].full_speed * 3.6};
            },
            (void *)&lap, (int)lap.Count());
      }

      if (auto d = HoverDistance()) {
        plot_cursor(*d);
        for (int slot = 0; slot < slots; ++slot) {
          if (auto s = SampleAtDistance(resampled_[slot], *d)) {
            ImPlot::Annotation(*d, s->full_speed * 3.6, LapColor(slot),
                               annotation_offset(slot), true, "%.1f km/h",
                               s->full_speed * 3.6);
          }
        }
      }
      if (ImPlot::IsPlotHovered()) {
        SetHoverDistance(ImPlot::GetPlotMousePos().x);
      }
      ImPlot::EndPlot();
    }

    if (plots_need_fit_) {
      ImPlot::SetNextAxesToFit();
    }
    if (ImPlot::BeginPlot("Delta", ImVec2(), ImPlotFlags_NoTitle)) {
      if (best_slot_ != -1) {
        const Lap &best_lap = resampled_[best_slot_];

        for (int slot = 0; slot < slots; ++slot) {
          const Lap &lap = resampled_[slot];
          int plot_count = static_cast<int>(
              std::min(lap.points.size(), best_lap.points.size()));
          if (plot_count <= 0)
            continue;

          std::tuple<const Lap &, const Lap &> data{lap, best_lap};
          ImPlot::SetNextLineStyle(LapColor(slot));
          ImPlot::PlotLineG(
              comparison->laps[slot].Label().c_str(),
              [](int index, void *data) -> ImPlotPoint {
                auto [lap, best_lap] =
                    *(std::tuple<const Lap &, const Lap &> *)data;
                auto lap_time = (lap.points[index].timestamp_ms -
                                 lap.points[0].timestamp_ms) /
                                1000.0;
                auto best_time = (best_lap.points[index].timestamp_ms -
                                  best_lap.points[0].timestamp_ms) /
                                 1000.0;
                return ImPlotPoint{best_lap.cum_distances[index],
                                   lap_time - best_time};
              },
              &data, plot_count);
        }

        if (auto d = HoverDistance()) {
          plot_cursor(*d);
          for (int slot = 0; slot < slots; ++slot) {
            if (auto delta = DeltaAtDistance(resampled_[slot], best_lap, *d)) {
              ImPlot::Annotation(*d, *delta, LapColor(slot),
                                 annotation_offset(slot), true, "%+.2fs",
                                 *delta);
            }
          }
        }
      }
      if (ImPlot::IsPlotHovered()) {
        SetHoverDistance(ImPlot::GetPlotMousePos().x);
      }
      ImPlot::EndPlot();
    }
    plots_need_fit_ = false;
    ImPlot::EndSubplots();
  }

  if (dnd) {
    ImGui::EndDragDropTarget();
  }
}

//--------------------------------- THE MAP ---------------------------------//

void ComparisonView::SetupComparisonMap() {
  if (!map_needs_fit_ || !comparison->HasTrack()) {
    return;
  }
  map_needs_fit_ = false;

  // Bounds of the reference track in this->cs local meters (the frame the
  // map plots in; equals the track's own frame once a track is adopted).
  auto to_local = [&](const Point &ref_local) {
    Vec3f p = cs.Local(
        comparison->track.cs.Global(Vec3f{ref_local.x, ref_local.y, 0}));
    return Point{p[0], p[1]};
  };

  double min_x = std::numeric_limits<double>::infinity(), min_y = min_x;
  double max_x = -min_x, max_y = -min_y;
  for (const Segment &seg : comparison->track.segments) {
    for (Point p : {to_local(seg.first), to_local(seg.second)}) {
      min_x = std::min(min_x, p.x);
      max_x = std::max(max_x, p.x);
      min_y = std::min(min_y, p.y);
      max_y = std::max(max_y, p.y);
    }
  }

  double margin = std::max(20.0, 0.05 * std::max(max_x - min_x, max_y - min_y));
  ImPlot::SetupAxisLimits(ImAxis_X1, min_x - margin, max_x + margin,
                          ImPlotCond_Always);
  ImPlot::SetupAxisLimits(ImAxis_Y1, min_y - margin, max_y + margin,
                          ImPlotCond_Always);
}

void ComparisonView::PlotComparisonMap(const Session &session) {
  if (!comparison->HasTrack()) {
    return;
  }
  RefreshResampled(session);

  if (show_reference_track) {
    PlotTrackOutline(comparison->track, cs);
  }

  for (size_t slot = 0; slot < resampled_.size(); ++slot) {
    const Lap &lap = resampled_[slot];
    if (lap.Count() == 0)
      continue;
    auto data = std::pair{this, &lap};
    ImPlot::SetNextLineStyle(LapColor((int)slot), 2.0f);
    ImPlot::PlotLineG(
        comparison->laps[slot].Label().c_str(),
        [](int index, void *data) -> ImPlotPoint {
          auto &[self, lap] =
              *reinterpret_cast<std::pair<ComparisonView *, const Lap *> *>(
                  data);
          Vec3f p = self->cs.Local(lap->points[index]);
          return ImPlotPoint{p[0], p[1]};
        },
        &data, (int)lap.Count());
  }

  // Hovering inside the track picks the distance for every view: project
  // the mouse onto the middle line and translate the gate position into a
  // distance along the best lap (the delta plot's x-axis). Without a best
  // lap, fall back to the middle line's own arc length.
  if (ImPlot::IsPlotHovered()) {
    auto mouse = ImPlot::GetPlotMousePos();
    auto to_local = [&](const Point &ref_local) {
      Vec3f p = cs.Local(
          comparison->track.cs.Global(Vec3f{ref_local.x, ref_local.y, 0}));
      return Point{p[0], p[1]};
    };
    std::vector<Segment> gates = comparison->track.DensifiedGates();
    for (Segment &gate : gates) {
      gate = Segment{to_local(gate.first), to_local(gate.second)};
    }
    if (auto pos = ProjectOntoMidline(gates, Point{mouse.x, mouse.y})) {
      size_t k = static_cast<size_t>(*pos);
      double t = *pos - static_cast<double>(k);

      const Lap *best = best_slot_ != -1 ? &resampled_[best_slot_] : nullptr;
      // Resample() emits the crossing of gate k at point index k.
      if (best && k + 1 < best->cum_distances.size()) {
        SetHoverDistance(best->cum_distances[k] * (1 - t) +
                         best->cum_distances[k + 1] * t);
      } else if (best && k < best->cum_distances.size()) {
        SetHoverDistance(best->cum_distances[k]);
      } else {
        double d = 0;
        auto mid = [](const Segment &g) { return (g.first + g.second) / 2.0; };
        for (size_t i = 0; i + 1 < gates.size() && i <= k; ++i) {
          double len = std::sqrt((mid(gates[i + 1]) - mid(gates[i])).Norm());
          d += (i == k) ? len * t : len;
        }
        SetHoverDistance(d);
      }
    }
  }

  // A dot per lap at the hovered distance, matching the vertical cursor in
  // the speed/delta plots.
  if (auto d = HoverDistance()) {
    for (size_t slot = 0; slot < resampled_.size(); ++slot) {
      if (auto s = SampleAtDistance(resampled_[slot], *d)) {
        Vec3f p = cs.Local(*s);
        double x = p[0], y = p[1];
        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 6, LapColor((int)slot),
                                   IMPLOT_AUTO, LapColor((int)slot));
        ImPlot::PlotScatter(std::format("##hover{}", slot).c_str(), &x, &y, 1);
      }
    }
  }
}

} // namespace pacer
