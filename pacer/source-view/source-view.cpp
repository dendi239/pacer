#include "source-view.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

#include "imgui.h"
#include "imgui_stdlib.h"
#include "implot.h"

namespace pacer {

SourceView::SourceView(Source *source) : source{source} {
  display.laps = &source->laps;
  track_picker_.path = source->track_path;
  if (source->HasTrack()) {
    ApplyTrack();
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

void SourceView::ApplyTrack() {
  const ReferenceTrack &track = source->track;
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
      ApplyTrack();
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

} // namespace pacer
