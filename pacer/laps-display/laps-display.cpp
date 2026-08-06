#include "laps-display.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "implot.h"
#include "implot_internal.h"

#include <pacer/datatypes/datatypes.hpp>

ImPlotPoint pacer::ToImPlotPoint(int index, void *data) {
  GPSSample *data_ = reinterpret_cast<GPSSample *>(data);
  return ImPlotPoint(data_[index].lon, data_[index].lat);
}

ImPlotPoint pacer::LapsDisplay::ToImPlotPoint(GPSSample s) const {
  auto p = cs.Local(s);
  return {p[0], p[1]};
}

// Plots a timing line as a plain (non-draggable) segment; the geometry is
// owned by the reference track, so it's edited in track_annotator, not here.
static void PlotTimingLine(const char *name, const pacer::Segment &s) {
  double xs[2] = {s.first.x, s.second.x};
  double ys[2] = {s.first.y, s.second.y};
  ImPlot::PushStyleVar(ImPlotStyleVar_LineWeight, 3.0f);
  ImPlot::PlotLine(name, xs, ys, 2);
  ImPlot::PopStyleVar();
  ImPlot::PlotScatter(name, xs, ys, 2);
}

bool pacer::LapsDisplay::HasMapFrame() const {
  if (has_supplied_frame)
    return true;
  return laps && laps->PointCount() > 0 && bounds.first.x < bounds.second.x;
}

void pacer::LapsDisplay::SetMapFrame(const CoordinateSystem &frame) {
  cs = frame;
  has_supplied_frame = true;
  // Invalidate bounds so the next SetupMap refits the axes in the new frame.
  bounds = {{1, 1}, {0, 0}};
  if (laps) {
    laps->SetCoordinateSystem(cs);
  }
}

void pacer::LapsDisplay::SetupMap() {
  if (bounds.first.x >= bounds.second.x) {
    if (laps->PointCount() == 0)
      return;

    bounds = laps->MinMax();
    if (!has_supplied_frame) {
      cs = CoordinateSystem(GPSSample{
          .lat = (bounds.first.y + bounds.second.y) / 2,
          .lon = (bounds.first.x + bounds.second.x) / 2,
          .altitude = 0,
      });
    }
    laps->SetCoordinateSystem(cs);
    auto min_ = cs.Local(GPSSample{
        .lat = bounds.first.y,
        .lon = bounds.first.x,
    });
    auto max_ = cs.Local(GPSSample{
        .lat = bounds.second.y,
        .lon = bounds.second.x,
    });
    bounds = {{min_[0], min_[1]}, {max_[0], max_[1]}};

    auto gp = ImPlot::GetCurrentContext();

    if (!gp || gp->CurrentPlot == nullptr) {
      return;
    }

    auto plot_size = gp->CurrentPlot->PlotRect.GetSize();

    // Guard against zero-sized plot rect which may lead to division by zero
    if (plot_size.x <= 0.0 || plot_size.y <= 0.0) {
      return;
    }

    // Guard against NaN bounds that can propagate into axis limits
    if (std::isnan(bounds.first.x) || std::isnan(bounds.first.y) ||
        std::isnan(bounds.second.x) || std::isnan(bounds.second.y)) {
      return;
    }

    auto x_width = std::max(bounds.second.x - bounds.first.x,
                            (bounds.second.y - bounds.first.y) * plot_size.x /
                                plot_size.y);
    auto y_width = std::max(bounds.second.y - bounds.first.y,

                            (bounds.second.x - bounds.first.x) * plot_size.y /
                                plot_size.x);

    ImPlot::SetupAxisLimits(
        ImAxis_X1, (bounds.first.x + bounds.second.x) / 2 - x_width / 2,
        (bounds.first.x + bounds.second.x) / 2 + x_width / 2,
        ImPlotCond_Always);

    ImPlot::SetupAxisLimits(
        ImAxis_Y1, (bounds.first.y + bounds.second.y) / 2 - y_width / 2,
        (bounds.first.y + bounds.second.y) / 2 + y_width / 2,
        ImPlotCond_Always);
  }
}

void pacer::LapsDisplay::PlotMapItems() {
  ImPlot::PlotLineG(
      "trace",
      [](int index, void *data) {
        auto &ld = *reinterpret_cast<LapsDisplay *>(data);
        return ld.ToImPlotPoint(ld.laps->GetPoint(index));
      },
      reinterpret_cast<void *>(this), (int)laps->PointCount());

  const Segment &start = laps->sectors.start_line;
  bool has_start = start.first.x != start.second.x ||
                   start.first.y != start.second.y;
  if (has_start) {
    PlotTimingLine("Start", start);
  }
  for (int i = 0; i < laps->SectorCount(); ++i) {
    std::stringstream ss;
    ss << "Sector " << i + 1;
    PlotTimingLine(ss.str().c_str(), laps->sectors.sector_lines[i]);
  }
}

void pacer::LapsDisplay::DisplayLapTelemetry() const {
  if (selected_lap != -1 && ImPlot::BeginPlot("Lap", ImVec2(-1, -1))) {
    ImPlot::PlotLineG(
        "speed trace",
        [](int index, void *data) {
          auto &ld = *reinterpret_cast<LapsDisplay *>(data);

          return ImPlotPoint{
              (double)index, // ld.laps->Distance(ld.selected_lap, index),
              ld.laps->Speed(ld.selected_lap, index) * 3.6};
        },
        (void *)this, (int)laps->SampleCount(selected_lap));
    ImPlot::PlotScatterG(
        "speed trace",
        [](int index, void *data) {
          auto &ld = *reinterpret_cast<LapsDisplay *>(data);

          return ImPlotPoint{
              (double)index, // ld.laps->Distance(ld.selected_lap, index),
              ld.laps->Speed(ld.selected_lap, index) * 3.6};
        },
        (void *)this, (int)laps->SampleCount(selected_lap));

    ImPlot::EndPlot();
  }
}
bool pacer::LapsDisplay::DisplayTable() {
  size_t sector_count = 1 + laps->SectorCount();
  if (!ImGui::BeginTable("Laps", 4 + 2 * (int)sector_count,
                         ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_BordersInnerV)) {
    return false;
  }

  ImGui::TableSetupColumn("start");
  ImGui::TableSetupColumn("points");
  ImGui::TableSetupColumn("distance");
  ImGui::TableSetupColumn("laptime");
  for (size_t i = 0; i < sector_count; ++i) {
    std::stringstream ss;
    ss << "S" << i + 1;
    ImGui::TableSetupColumn("");
    ImGui::TableSetupColumn(ss.str().c_str());
  }
  ImGui::TableHeadersRow();

  for (int row = 0, i_sector = 0; row < laps->LapsCount(); ++row) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);

    ImGui::Selectable(std::format("{}", row).c_str(), false, 0, ImVec2(100, 0));

    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
      ImGui::SetDragDropPayload("MY_DND", &row, sizeof(int));
      ImGui::Text("%.3f", laps->StartTimestamp(row));
      ImGui::EndDragDropSource();
    }

    ImGui::TableSetColumnIndex(1);
    ImGui::Text("%zu", laps->SampleCount(row));

    ImGui::TableSetColumnIndex(2);
    ImGui::Text("%.2f", laps->GetLapDistance(row, cs));

    ImGui::TableSetColumnIndex(3);
    if (ImGui::Button(std::format("{:.3f}", laps->LapTime(row)).c_str())) {
      selected_lap = row == selected_lap ? -1 : row;
    }

    for (int i = 0; i < sector_count; ++i, ++i_sector) {
      ImGui::TableSetColumnIndex(4 + 2 * i);
      if (i_sector < laps->RecordedSectors()) {

        ImGui::Text("%.3fkph", laps->SectorEntrySpeed(i_sector) * 3.6);
      }
      ImGui::TableSetColumnIndex(5 + 2 * i);

      if (i_sector < laps->RecordedSectors()) {
        ImGui::Text("%.3fs", laps->SectorTime(i_sector));
      }
    }
  }

  ImGui::EndTable();
  return true;
}
ImPlotPoint Vec3fToPoint(int index, void *data) {
  auto s = reinterpret_cast<pacer::Vec3f *>(data)[index];
  return {s[0], s[1]};
}

namespace {

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

} // namespace

ImVec4 pacer::DeltaLapsComparision::LapColor(int lap_id) {
  // Keyed by lap id (not selection order), so a lap keeps its color when
  // other laps are added or removed. GetColormapColor wraps the index.
  return ImPlot::GetColormapColor(lap_id);
}

void pacer::DeltaLapsComparision::SetHoverDistance(double distance) {
  hover_distance_ = distance;
  hover_frame_ = ImGui::GetFrameCount();
}

std::optional<double> pacer::DeltaLapsComparision::HoverDistance() const {
  // Accept the previous frame too: the producing window may be drawn after
  // the consuming one within a frame.
  if (hover_frame_ < ImGui::GetFrameCount() - 1) {
    return std::nullopt;
  }
  return hover_distance_;
}

void pacer::DeltaLapsComparision::RefreshResampled(const Laps &laps) {
  if (resample_frame_ == ImGui::GetFrameCount()) {
    return;
  }
  resample_frame_ = ImGui::GetFrameCount();

  resampled_laps_.clear();
  best_lap_id_ = -1;
  for (int lap_id : selected_laps) {
    resampled_laps_[lap_id] = reference_track.Resample(laps.GetLap(lap_id));
    if (best_lap_id_ == -1 || laps.LapTime(lap_id) < laps.LapTime(best_lap_id_)) {
      best_lap_id_ = lap_id;
    }
  }
}

void pacer::DeltaLapsComparision::DrawReferenceTrackLoader(
    Laps &laps, LapsDisplay &display) {
  bool load = reference_track_picker.Draw("reference_track");
  if (ImGui::Button("Load reference track") &&
      !reference_track_picker.path.empty()) {
    load = true;
  }
  ImGui::SetNextItemWidth(120);
  bool extension_changed = ImGui::SliderFloat(
      "Gate extension (m)", &gate_extension_m, 0.0f, 10.0f, "%.1f");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
    ImGui::SetTooltip("How far each timing gate extends past the annotated "
                      "track edges,\nso laps running slightly wide still "
                      "cross it. Affects the delta\nand lap/sector splits.");
  }
  reference_track.gate_extension_m = gate_extension_m;
  if (extension_changed && !reference_track.segments.empty()) {
    laps.sectors = reference_track.BuildSectors(reference_track.cs);
  }
  if (load) {
    try {
      reference_track = ReferenceTrack::FromFile(reference_track_picker.path);
      reference_track_status = "Loaded " +
                               std::to_string(reference_track.segments.size()) +
                               " segments";
      if (!reference_track.segments.empty()) {
        // The reference track supplies the map frame; sectors, delta sticks
        // and the display all share it from here on.
        display.SetMapFrame(reference_track.cs);
        cs = reference_track.cs;
        map_needs_fit_ = true;
        plots_need_fit_ = true;
        laps.sectors = reference_track.BuildSectors(reference_track.cs);
        reference_track_status +=
            reference_track.sector_indices.empty()
                ? " (no sectors marked)."
                : " (" + std::to_string(reference_track.sector_indices.size()) +
                      " sectors).";
      } else {
        reference_track_status += ".";
      }
    } catch (const std::exception &e) {
      reference_track_status = std::string("Error: ") + e.what();
    }
  }
  if (!reference_track_status.empty()) {
    ImGui::TextWrapped("%s", reference_track_status.c_str());
  }
}

void pacer::DeltaLapsComparision::PlotSticks() {
  ImPlot::PushStyleVar(ImPlotStyleVar_LineWeight, 3.0f);
  for (size_t i = 0; i < reference_track.TimingLinesCount(); ++i) {
    Segment seg = reference_track.TimingLine(i);
    auto a = reference_track.cs.Global(Vec3f{seg.first.x, seg.first.y, 0});
    auto b = reference_track.cs.Global(Vec3f{seg.second.x, seg.second.y, 0});
    Vec3f line[2] = {cs.Local(a), cs.Local(b)};
    ImPlot::PlotLineG("", Vec3fToPoint, line, 2);
  }
  ImPlot::PopStyleVar();
}

// std::optional<float>
void pacer::DeltaLapsComparision::Display(const Laps &laps) {
  if (reference_track.segments.empty()) {
    return;
  }

  bool dnd;
  if ((dnd = ImGui::BeginDragDropTargetCustom(ImGui::GetCurrentWindow()->Rect(),
                                              239))) {
    ImGui::Text("Drop laps here to select them");
    if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("MY_DND")) {
      int i = *(int *)payload->Data;
      if (selected_laps.contains(i)) {
        selected_laps.erase(i);
      } else {
        selected_laps.insert(i);
      }
      // The selection changed after this frame's resample; redo it, and
      // refit the plots to the new set of laps.
      resample_frame_ = -1;
      plots_need_fit_ = true;
    }
  }

  RefreshResampled(laps);

  std::vector<int> lap_ids(selected_laps.begin(), selected_laps.end());
  std::sort(lap_ids.begin(), lap_ids.end());

  const ImVec4 cursor_color{0.7f, 0.7f, 0.7f, 0.8f};

  // Draws the shared hover cursor: a vertical line at the hovered distance
  // plus a distance tag on the x-axis. Call inside a plot.
  auto plot_cursor = [&](double d) {
    double x = d;
    ImPlot::SetNextLineStyle(cursor_color, 1.0f);
    ImPlot::PlotInfLines("##hover", &x, 1);
    ImPlot::TagX(x, cursor_color, "%.0fm", x);
  };

  // Hover annotations would overlay each other when laps have close values
  // at the hovered distance, so alternate them around the cursor: laps at
  // even positions in lap_ids go left, odd positions go right. The side is
  // per lap, so it matches between the speed and delta plots.
  auto annotation_offset = [](size_t position) {
    return ImVec2(position % 2 == 0 ? -8.0f : 8.0f, -8.0f);
  };

  if (ImPlot::BeginSubplots("", 2, 1, ImVec2(-1, -1),
                            ImPlotSubplotFlags_LinkAllX)) {

    if (plots_need_fit_) {
      ImPlot::SetNextAxesToFit();
    }
    if (ImPlot::BeginPlot("Telemetry", ImVec2())) {
      ImPlot::SetupAxis(ImAxis_X1, "", ImPlotAxisFlags_NoTickLabels);

      for (int lap_id : lap_ids) {
        const Lap &lap = resampled_laps_[lap_id];
        ImPlot::SetNextLineStyle(LapColor(lap_id));
        ImPlot::PlotLineG(
            std::format("lap {}", lap_id).c_str(),
            [](int index, void *data) -> ImPlotPoint {
              const auto &lap = *reinterpret_cast<const pacer::Lap *>(data);
              return ImPlotPoint{lap.cum_distances[index],
                                 lap.points[index].full_speed * 3.6};
            },
            (void *)&lap, (int)lap.Count());
      }

      if (auto d = HoverDistance()) {
        plot_cursor(*d);
        for (size_t i = 0; i < lap_ids.size(); ++i) {
          if (auto s = SampleAtDistance(resampled_laps_[lap_ids[i]], *d)) {
            ImPlot::Annotation(*d, s->full_speed * 3.6, LapColor(lap_ids[i]),
                               annotation_offset(i), true, "%.1f km/h",
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
      if (best_lap_id_ != -1) {
        auto &best_lap = resampled_laps_[best_lap_id_];

        for (int lap_id : lap_ids) {
          auto &lap = resampled_laps_[lap_id];
          int plot_count = static_cast<int>(
              std::min(lap.points.size(), best_lap.points.size()));
          if (plot_count <= 0)
            continue;

          std::tuple<Lap &, Lap &> data{lap, best_lap};
          ImPlot::SetNextLineStyle(LapColor(lap_id));
          ImPlot::PlotLineG(
              std::format("lap {}", lap_id).c_str(),
              [](int index, void *data) -> ImPlotPoint {
                auto [lap, best_lap] = *(std::tuple<Lap &, Lap &> *)data;
                auto lap_time = (lap.points[index].timestamp_ms -
                                 lap.points[0].timestamp_ms) /
                                1000.0;
                auto best_time = (best_lap.points[index].timestamp_ms -
                                  best_lap.points[0].timestamp_ms) /
                                 1000.0;
                assert(best_time < 1000 && best_time >= 0);
                assert(lap_time < 1000 && lap_time >= 0);

                return ImPlotPoint{best_lap.cum_distances[index],
                                   lap_time - best_time};
              },
              &data, plot_count);
        }

        if (auto d = HoverDistance()) {
          plot_cursor(*d);
          for (size_t i = 0; i < lap_ids.size(); ++i) {
            if (auto delta = DeltaAtDistance(resampled_laps_[lap_ids[i]],
                                             best_lap, *d)) {
              ImPlot::Annotation(*d, *delta, LapColor(lap_ids[i]),
                                 annotation_offset(i), true, "%+.2fs", *delta);
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
  }

  ImPlot::EndSubplots();
  if (dnd) {
    ImGui::EndDragDropTarget();
  }
}

void pacer::DeltaLapsComparision::SetupComparisonMap() {
  if (!map_needs_fit_ || reference_track.segments.empty()) {
    return;
  }
  map_needs_fit_ = false;

  // Bounds of the reference track in this->cs local meters (the frame the
  // map plots in; equals the track's own frame once a track is loaded).
  auto to_local = [&](const Point &ref_local) {
    Vec3f p = cs.Local(
        reference_track.cs.Global(Vec3f{ref_local.x, ref_local.y, 0}));
    return Point{p[0], p[1]};
  };

  double min_x = std::numeric_limits<double>::infinity(), min_y = min_x;
  double max_x = -min_x, max_y = -min_y;
  for (const Segment &seg : reference_track.segments) {
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

// Plots the reference track as its outline: one polyline per annotated
// track edge (the gates' first/second endpoints keep consistent sides, see
// track_annotator's overlay), joined back to the start when the annotation
// wraps around into a closed circuit.
static void PlotTrackOutline(const pacer::ReferenceTrack &track,
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

void pacer::DeltaLapsComparision::PlotComparisonMap(const Laps &laps) {
  if (reference_track.segments.empty()) {
    return;
  }
  RefreshResampled(laps);

  if (show_reference_track) {
    PlotTrackOutline(reference_track, cs);
  }

  std::vector<int> lap_ids(selected_laps.begin(), selected_laps.end());
  std::sort(lap_ids.begin(), lap_ids.end());

  for (int lap_id : lap_ids) {
    const Lap &lap = resampled_laps_[lap_id];
    auto data = std::pair{this, &lap};
    ImPlot::SetNextLineStyle(LapColor(lap_id), 2.0f);
    ImPlot::PlotLineG(
        std::format("lap {}", lap_id).c_str(),
        [](int index, void *data) -> ImPlotPoint {
          auto &[self, lap] =
              *reinterpret_cast<std::pair<DeltaLapsComparision *, const Lap *>
                                    *>(data);
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
          reference_track.cs.Global(Vec3f{ref_local.x, ref_local.y, 0}));
      return Point{p[0], p[1]};
    };
    std::vector<Segment> gates = reference_track.DensifiedGates();
    for (Segment &gate : gates) {
      gate = Segment{to_local(gate.first), to_local(gate.second)};
    }
    if (auto pos = ProjectOntoMidline(gates, Point{mouse.x, mouse.y})) {
      size_t k = static_cast<size_t>(*pos);
      double t = *pos - static_cast<double>(k);

      const Lap *best =
          best_lap_id_ != -1 ? &resampled_laps_[best_lap_id_] : nullptr;
      // Resample() emits the crossing of gate k at point index k + 1.
      if (best && k + 2 < best->cum_distances.size()) {
        SetHoverDistance(best->cum_distances[k + 1] * (1 - t) +
                         best->cum_distances[k + 2] * t);
      } else if (best && k + 1 < best->cum_distances.size()) {
        SetHoverDistance(best->cum_distances[k + 1]);
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
    for (int lap_id : lap_ids) {
      if (auto s = SampleAtDistance(resampled_laps_[lap_id], *d)) {
        Vec3f p = cs.Local(*s);
        double x = p[0], y = p[1];
        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 6, LapColor(lap_id),
                                   IMPLOT_AUTO, LapColor(lap_id));
        ImPlot::PlotScatter(std::format("##hover{}", lap_id).c_str(), &x, &y,
                            1);
      }
    }
  }
}
