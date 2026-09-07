#include "source-view.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <limits>
#include <vector>

#include "imgui.h"
#include "imgui_stdlib.h"
#include "implot.h"

namespace pacer {

SourceView::SourceView(Source *source) : source{source} {
  display.laps = &source->laps;
  track_picker_.path = source->track_path;
  if (source->HasTrack()) {
    AdoptTrack();
  }
}

void SourceView::Update() {
  if (source->Update()) {
    // The rebuild replaced every point, so any lap index the views held is
    // stale and the axes are fitted to data that is gone.
    display.bounds = {{1, 1}, {0, 0}};
    display.selected_lap = -1;
  }
}

void SourceView::AdoptTrack() {
  const ReferenceTrack &track = source->track;
  track_picker_.path = source->track_path;
  if (track.segments.empty()) {
    track_status_ = "Track has no segments.";
    return;
  }
  // The reference track supplies the map frame, so it outlives any
  // particular set of loaded files.
  display.SetMapFrame(track.cs);
  track_status_ =
      std::format("Loaded {} segments ({}).", track.segments.size(),
                  track.sector_indices.empty()
                      ? std::string("no sectors marked")
                      : std::format("{} sectors",
                                    track.sector_indices.size()));
}

//------------------------------ TRACK PANEL --------------------------------//

void SourceView::DrawTrackPanel() {
  bool load = track_picker_.Draw("reference_track");
  if (ImGui::Button("Load reference track") && !track_picker_.path.empty()) {
    load = true;
  }

  ImGui::SetNextItemWidth(120);
  float extension = static_cast<float>(source->track.gate_extension_m);
  if (ImGui::SliderFloat("Gate extension (m)", &extension, 0.0f, 10.0f,
                         "%.1f")) {
    source->track.gate_extension_m = extension;
    source->TrackChanged();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
    ImGui::SetTooltip("How far each timing gate extends past the annotated "
                      "track edges,\nso laps running slightly wide still "
                      "cross it. Affects the delta\nand lap/sector splits.");
  }

  if (load) {
    std::string error;
    if (source->LoadTrack(track_picker_.path, &error)) {
      AdoptTrack();
    } else {
      track_status_ = "Error: " + error;
    }
  }

  if (!track_status_.empty()) {
    ImGui::TextWrapped("%s", track_status_.c_str());
  }
  if (source->laps.PointCount() > 0 && !source->HasTrack()) {
    ImGui::TextWrapped(
        "Load a reference track to split the data into laps and sectors.");
  }
}

//------------------------------ FILES PANEL --------------------------------//

namespace {

// "12:34.567" for a duration, so a file's span reads at a glance.
std::string FormatDuration(int64_t ms) {
  if (ms < 0)
    ms = 0;
  int64_t total_s = ms / 1000;
  return std::format("{}:{:02}.{:03}", total_s / 60, total_s % 60, ms % 1000);
}

} // namespace

void SourceView::DrawFilesPanel() {
  ImGui::SetNextItemWidth(-90);
  bool submitted = ImGui::InputTextWithHint(
      "##new_file", "path to a .MP4 or .dat recording", &pending_path_,
      ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  if ((ImGui::Button("Add file") || submitted) && !pending_path_.empty()) {
    std::string error;
    if (source->AddFile(pending_path_, &error)) {
      files_status_ = std::format("Loaded {} samples from {}.",
                                  source->files.back().samples.size(),
                                  pending_path_);
      pending_path_.clear();
    } else {
      files_status_ = error;
    }
  }

  if (source->files.empty()) {
    ImGui::TextWrapped("No files yet. GoPro splits long runs into several "
                       "clips; add them in order and they are treated as one "
                       "continuous recording.");
    return;
  }

  int remove_index = -1;
  int move_index = -1, move_delta = 0;

  for (int i = 0; i < (int)source->files.size(); ++i) {
    SourceFile &file = source->files[i];
    ImGui::PushID(i);

    if (ImGui::Checkbox("##enabled", &file.enabled)) {
      source->MarkDirty();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
      ImGui::SetTooltip("Include this file in the source");
    }
    ImGui::SameLine();

    bool open = ImGui::TreeNodeEx(
        "##file", ImGuiTreeNodeFlags_SpanAvailWidth,
        "%s  (%zu samples)", file.path.c_str(), file.samples.size());

    if (!file.error.empty()) {
      ImGui::TextWrapped("%s", file.error.c_str());
    }

    if (open) {
      if (file.samples.empty()) {
        ImGui::TextDisabled("Nothing loaded.");
      } else {
        // Trim as sample counts off each end. The first fixes after a
        // receiver wakes up are often stale or wildly inaccurate, and the
        // tail can carry the drive back to the paddock.
        int begin = (int)file.trim_begin;
        int end = (int)file.trim_end;
        int max_trim = (int)file.samples.size();
        ImGui::SetNextItemWidth(120);
        bool changed = ImGui::DragInt("Trim head", &begin, 1, 0, max_trim);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        changed |= ImGui::DragInt("Trim tail", &end, 1, 0, max_trim);
        if (changed) {
          file.trim_begin = (size_t)std::clamp(begin, 0, max_trim);
          file.trim_end = (size_t)std::clamp(end, 0, max_trim);
          source->MarkDirty();
        }

        ImGui::Text("%zu samples kept, %s", file.UsedCount(),
                    FormatDuration(file.LastTimestampMs() -
                                   file.FirstTimestampMs())
                        .c_str());

        if (ImGui::SmallButton("Auto-trim")) {
          size_t trimmed = source->AutoTrim(i);
          files_status_ =
              trimmed ? std::format("Auto-trim dropped {} samples from {}.",
                                    trimmed,
                                    std::filesystem::path(file.path)
                                        .filename()
                                        .string())
                      : "Auto-trim found nothing to drop.";
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
          ImGui::SetTooltip(
              "Drops samples off each end that the receiver hadn't settled "
              "on:\nfixes worse than 5 m, ones with no reported accuracy, "
              "and\nrepeats of a single stale position.");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset trim")) {
          file.trim_begin = 0;
          file.trim_end = 0;
          source->MarkDirty();
        }
        ImGui::SameLine();
      }

      if (ImGui::SmallButton("Reload")) {
        std::string error;
        if (!source->ReloadFile(i, &error))
          files_status_ = error;
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Up")) {
        move_index = i;
        move_delta = -1;
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Down")) {
        move_index = i;
        move_delta = 1;
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Remove")) {
        remove_index = i;
      }
      ImGui::TreePop();
    }

    ImGui::PopID();
  }

  if (move_index >= 0)
    source->MoveFile(move_index, move_delta);
  if (remove_index >= 0)
    source->RemoveFile(remove_index);

  ImGui::Separator();
  ImGui::Text("%zu samples in %zu laps", source->UsedSampleCount(),
              source->LapsCount());
  if (!files_status_.empty()) {
    ImGui::TextWrapped("%s", files_status_.c_str());
  }
}

//---------------------------- LAP CHART PANEL ------------------------------//

void SourceView::DrawLapChartPanel() {
  Laps &laps = source->laps;

  ImGui::SetNextItemWidth(200);
  ImGui::SliderFloat("Cutoff (% of best)", &lap_cutoff_pct_, 100, 150, "%.0f");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
    ImGui::SetTooltip("Laps slower than this share of the session best are "
                      "left off the chart,\nso an in-lap or a full course "
                      "yellow doesn't flatten the scale.");
  }

  if (!ImPlot::BeginPlot("Lap time chart", ImVec2(-1, -1),
                         ImPlotFlags_NoTitle)) {
    return;
  }

  double best_lap = std::numeric_limits<double>::max();
  for (size_t i = 0; i < laps.LapsCount(); ++i) {
    double t = laps.LapTime(i);
    if (t > 1.0 && t < best_lap)
      best_lap = t;
  }
  if (best_lap == std::numeric_limits<double>::max())
    best_lap = 0;

  struct ChartData {
    Laps &laps;
    double best;
    double cutoff;
  } data{laps, best_lap, best_lap * lap_cutoff_pct_ / 100};

  auto getter = [](int index, void *raw) {
    auto &d = *reinterpret_cast<ChartData *>(raw);
    double time = d.laps.LapTime(index);
    if (time > d.cutoff || time < d.best)
      time = NAN;
    return ImPlotPoint((double)index, time);
  };

  ImPlot::PlotLineG("Lap time", getter, &data, (int)laps.LapsCount());
  ImPlot::PlotScatterG("Lap time", getter, &data, (int)laps.LapsCount());
  ImPlot::EndPlot();
}

//---------------------------- LAP TABLE PANEL ------------------------------//

void SourceView::DrawLapTablePanel() { display.DisplayTable(); }

//----------------------------- SAMPLES PANEL -------------------------------//

namespace {

// Points to plot for one file, in seconds since the source's start.
struct Trace {
  std::vector<double> t;
  std::vector<double> value;

  void Clear() {
    t.clear();
    value.clear();
  }
  void Add(double time, double v) {
    t.push_back(time);
    value.push_back(v);
  }
  int Count() const { return (int)t.size(); }
};

// Fills `out` with `samples[begin, end)` reduced to at most 2*`buckets`
// points: per bucket, its lowest and highest value, in the order they occur.
// Striding would drop a one-sample accuracy spike, which is exactly what
// this view exists to show; a min/max envelope keeps it.
void BuildEnvelope(const std::vector<pacer::GPSSample> &samples, size_t begin,
                   size_t end, bool speed, double offset_s, int buckets,
                   Trace *out) {
  out->Clear();
  if (begin >= end)
    return;

  auto value_of = [&](const pacer::GPSSample &sample) {
    return speed ? sample.full_speed * 3.6 : sample.h_acc;
  };
  auto time_of = [&](const pacer::GPSSample &sample) {
    return sample.timestamp_ms / 1000.0 + offset_s;
  };

  const size_t count = end - begin;
  if (count <= (size_t)buckets * 2) {
    out->t.reserve(count);
    out->value.reserve(count);
    for (size_t i = begin; i < end; ++i) {
      out->Add(time_of(samples[i]), value_of(samples[i]));
    }
    return;
  }

  out->t.reserve(buckets * 2);
  out->value.reserve(buckets * 2);
  for (int b = 0; b < buckets; ++b) {
    size_t from = begin + count * b / buckets;
    size_t to = begin + count * (b + 1) / buckets;
    if (from >= to)
      continue;
    size_t lo = from, hi = from;
    for (size_t i = from + 1; i < to; ++i) {
      if (value_of(samples[i]) < value_of(samples[lo]))
        lo = i;
      if (value_of(samples[i]) > value_of(samples[hi]))
        hi = i;
    }
    size_t first = std::min(lo, hi), second = std::max(lo, hi);
    out->Add(time_of(samples[first]), value_of(samples[first]));
    if (second != first)
      out->Add(time_of(samples[second]), value_of(samples[second]));
  }
}

} // namespace

void SourceView::DrawSamplesPanel() {
  auto [span_begin_ms, span_end_ms] = source->FullTimestampSpanMs();
  if (span_end_ms <= span_begin_ms) {
    ImGui::TextWrapped("Add a file to see its samples.");
    return;
  }
  const double origin_s = span_begin_ms / 1000.0;
  const double span_s = (span_end_ms - span_begin_ms) / 1000.0;

  // Seconds on the plot's axis for a timestamp in `file_index`'s own clock.
  auto file_offset_s = [&](size_t file_index) {
    return source->FileOffsetMs(file_index) / 1000.0 - origin_s;
  };

  ImGui::TextDisabled("Drag a handle to trim that file. Trimmed samples stay "
                      "on the plot, greyed out.");

  // Speed and fix accuracy are different quantities on different scales, so
  // they get a subplot each over one shared time axis -- never two y-scales
  // on one plot.
  constexpr int kRows = 2;
  if (!ImPlot::BeginSubplots("##samples", kRows, 1, ImVec2(-1, -1),
                             ImPlotSubplotFlags_LinkAllX)) {
    return;
  }

  Trace kept, dropped;
  for (int row = 0; row < kRows; ++row) {
    const bool speed_row = row == 0;
    if (!ImPlot::BeginPlot("##samples_row", ImVec2(-1, -1),
                           ImPlotFlags_NoTitle | ImPlotFlags_NoLegend)) {
      continue;
    }
    ImPlot::SetupAxes(speed_row ? nullptr : "Time (s)",
                      speed_row ? "Speed (km/h)" : "Fix accuracy (m)", 0,
                      ImPlotAxisFlags_AutoFit);
    ImPlot::SetupAxisLimits(ImAxis_X1, 0, span_s, ImPlotCond_Once);
    ImPlot::SetupFinish();

    const ImPlotRect limits = ImPlot::GetPlotLimits();
    // One bucket per pixel column: finer than that is invisible, coarser
    // throws away detail the user zoomed in to see.
    const int buckets =
        std::clamp((int)ImPlot::GetPlotSize().x, 64, 4096);

    // File bands are background context, so they go straight to the draw
    // list: neutral tints that name their file and stay out of the axis fit.
    ImPlot::PushPlotClipRect();
    ImDrawList *draw = ImPlot::GetPlotDrawList();
    const ImVec4 muted = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    for (size_t i = 0; i < source->files.size(); ++i) {
      const SourceFile &file = source->files[i];
      if (file.samples.empty())
        continue;
      double from = file.samples.front().timestamp_ms / 1000.0 +
                    file_offset_s(i);
      double to =
          file.samples.back().timestamp_ms / 1000.0 + file_offset_s(i);
      ImVec2 top_left = ImPlot::PlotToPixels(from, limits.Y.Max);
      ImVec2 bottom_right = ImPlot::PlotToPixels(to, limits.Y.Min);
      draw->AddRectFilled(top_left, bottom_right,
                          ImGui::GetColorU32(ImVec4(muted.x, muted.y, muted.z,
                                                    (i % 2) ? 0.12f : 0.05f)));
      if (speed_row) {
        std::string name = std::filesystem::path(file.path).filename().string();
        if (!file.enabled)
          name += " (off)";
        draw->AddText(ImVec2(top_left.x + 4, top_left.y + 4),
                      ImGui::GetColorU32(ImGuiCol_TextDisabled), name.c_str());
      }
    }
    ImPlot::PopPlotClipRect();

    for (size_t i = 0; i < source->files.size(); ++i) {
      const SourceFile &file = source->files[i];
      if (file.samples.empty() || !file.enabled)
        continue;
      const double offset_s = file_offset_s(i);
      // Only envelope what is on screen, so zooming in resolves more of it.
      const int64_t from_ms =
          (int64_t)((limits.X.Min - offset_s) * 1000);
      const int64_t to_ms = (int64_t)((limits.X.Max - offset_s) * 1000);
      const size_t view_begin = file.IndexAtTimestamp(from_ms);
      const size_t view_end =
          std::min(file.IndexAtTimestamp(to_ms) + 1, file.samples.size());

      // The kept window and the trimmed ends are drawn separately, so what
      // is included reads as the solid trace and what is not stays behind it.
      const size_t keep_begin = std::max(view_begin, file.BeginIndex());
      const size_t keep_end = std::min(view_end, file.EndIndex());

      ImPlot::PushStyleColor(ImPlotCol_Line, ImVec4(muted.x, muted.y, muted.z,
                                                    0.55f));
      ImPlot::PushStyleVar(ImPlotStyleVar_LineWeight, 1.0f);
      BuildEnvelope(file.samples, view_begin,
                    std::min(view_end, file.BeginIndex()), speed_row, offset_s,
                    buckets, &dropped);
      ImPlot::PlotLine("Trimmed", dropped.t.data(), dropped.value.data(),
                       dropped.Count());
      BuildEnvelope(file.samples, std::max(view_begin, file.EndIndex()),
                    view_end, speed_row, offset_s, buckets, &dropped);
      ImPlot::PlotLine("Trimmed", dropped.t.data(), dropped.value.data(),
                       dropped.Count());
      ImPlot::PopStyleVar();
      ImPlot::PopStyleColor();

      ImPlot::PushStyleVar(ImPlotStyleVar_LineWeight, 2.0f);
      BuildEnvelope(file.samples, keep_begin, keep_end, speed_row, offset_s,
                    buckets, &kept);
      ImPlot::PlotLine("Samples", kept.t.data(), kept.value.data(),
                       kept.Count());
      ImPlot::PopStyleVar();
    }

    // Lap starts, so a clip boundary or a dropout landing mid-lap is visible
    // against the trace. Vertical infinite lines take no part in the y fit.
    // A full session is a hundred-odd laps, which at that zoom reads as
    // hatching rather than as marks, so they appear once zoomed in enough to
    // tell them apart.
    constexpr int kMaxVisibleLapLines = 60;
    const Laps &laps = source->laps;
    std::vector<double> lap_starts;
    for (size_t lap = 0; lap < laps.LapsCount(); ++lap) {
      double at = laps.StartTimestamp(lap) - origin_s;
      if (at >= limits.X.Min && at <= limits.X.Max) {
        lap_starts.push_back(at);
        if ((int)lap_starts.size() > kMaxVisibleLapLines)
          break;
      }
    }
    if (!lap_starts.empty() &&
        (int)lap_starts.size() <= kMaxVisibleLapLines) {
      ImPlot::PushStyleVar(ImPlotStyleVar_LineWeight, 1.0f);
      ImPlot::PushStyleColor(ImPlotCol_Line,
                             ImVec4(muted.x, muted.y, muted.z, 0.35f));
      ImPlot::PlotInfLines("Lap start", lap_starts.data(),
                           (int)lap_starts.size());
      ImPlot::PopStyleColor();
      ImPlot::PopStyleVar();
    }

    // The trim handles live on the speed row only: two per file is already
    // enough furniture without repeating them underneath.
    if (speed_row) {
      DrawTrimHandles(origin_s);
    }

    ImPlot::EndPlot();
  }

  ImPlot::EndSubplots();
}

void SourceView::DrawTrimHandles(double origin_s) {
  const ImVec4 handle_color = ImGui::GetStyleColorVec4(ImGuiCol_SliderGrab);

  for (size_t i = 0; i < source->files.size(); ++i) {
    SourceFile &file = source->files[i];
    if (file.samples.empty() || !file.enabled)
      continue;
    const double offset_s = source->FileOffsetMs(i) / 1000.0 - origin_s;
    auto to_index = [&](double plot_s) {
      int64_t target = (int64_t)((plot_s - offset_s) * 1000);
      return std::min(file.IndexAtTimestamp(target), file.samples.size() - 1);
    };

    double head = file.samples[file.BeginIndex()].timestamp_ms / 1000.0 +
                  offset_s;
    double tail = file.samples[file.EndIndex() - 1].timestamp_ms / 1000.0 +
                  offset_s;

    // Two ids per file, distinct from every other file's.
    if (ImPlot::DragLineX((int)i * 2, &head, handle_color, 2.0f)) {
      size_t index = to_index(head);
      file.trim_begin = index;
      // Keep at least one sample: the tail handle must stay ahead of this.
      file.trim_end =
          std::min(file.trim_end, file.samples.size() - index - 1);
      source->MarkDirty();
    }
    if (ImPlot::DragLineX((int)i * 2 + 1, &tail, handle_color, 2.0f)) {
      size_t index = to_index(tail);
      file.trim_end = file.samples.size() - index - 1;
      file.trim_begin = std::min(file.trim_begin, index);
      source->MarkDirty();
    }
  }
}

} // namespace pacer
