#include "laps-display.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_map>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
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

void pacer::LapsDisplay::DragTimingLine(Segment *s, const char *name,
                                        int drag_id) {
  auto get_point = [](int index, void *data) -> ImPlotPoint {
    auto &s = *reinterpret_cast<Segment *>(data);
    const Point &p = index ? s.second : s.first;
    return {p.x, p.y};
  };

  ImPlot::PlotLineG(name, get_point, s, 2, 0);
  ImPlot::PlotScatterG(name, get_point, s, 2, 0);

  ImPlot::DragPoint(2 * drag_id + 1, &s->first.x, &s->first.y,
                    ImPlot::GetLastItemColor());
  ImPlot::DragPoint(2 * drag_id + 2, &s->second.x, &s->second.y,
                    ImPlot::GetLastItemColor());
}

void pacer::LapsDisplay::DisplayMap() {
  if (bounds.first.x >= bounds.second.x) {
    if (laps->PointCount() == 0)
      goto plot_data;

    bounds = laps->MinMax();
    cs = CoordinateSystem(GPSSample{
        .lat = (bounds.first.y + bounds.second.y) / 2,
        .lon = (bounds.first.x + bounds.second.x) / 2,
        .altitude = 0,
    });
    laps->SetCoordinateSystem(cs);
    laps->sectors.start_line = laps->PickRandomStart();
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

plot_data:

  ImPlot::PlotLineG(
      "trace",
      [](int index, void *data) {
        auto &ld = *reinterpret_cast<LapsDisplay *>(data);
        return ld.ToImPlotPoint(ld.laps->GetPoint(index).point);
      },
      reinterpret_cast<void *>(this), (int)laps->PointCount());

  DragTimingLine(&laps->sectors.start_line, "Start", 0);
  for (int i = 0; i < laps->SectorCount(); ++i) {
    auto &s = laps->sectors.sector_lines[i];
    std::stringstream ss;
    ss << "Sector " << i + 1;
    DragTimingLine(&s, ss.str().c_str(), i + 1);
  }
}

void pacer::LapsDisplay::DisplayLapTelemetry() const {
  if (selected_lap != -1 && ImPlot::BeginPlot("Lap", ImVec2(-1, -1))) {
    ImPlot::PlotLineG(
        "speed trace",
        [](int index, void *data) {
          auto &ld = *reinterpret_cast<LapsDisplay *>(data);
          auto [gps, time] = ld.laps->At(ld.selected_lap, index);

          return ImPlotPoint{
              (double)index, // ld.laps->Distance(ld.selected_lap, index),
              ld.laps->Speed(ld.selected_lap, index) * 3.6};
        },
        (void *)this, (int)laps->SampleCount(selected_lap));
    ImPlot::PlotScatterG(
        "speed trace",
        [](int index, void *data) {
          auto &ld = *reinterpret_cast<LapsDisplay *>(data);
          auto [gps, time] = ld.laps->At(ld.selected_lap, index);

          return ImPlotPoint{
              (double)index, // ld.laps->Distance(ld.selected_lap, index),
              ld.laps->Speed(ld.selected_lap, index) * 3.6};
        },
        (void *)this, (int)laps->SampleCount(selected_lap));

    ImPlot::EndPlot();
  }
}
bool pacer::LapsDisplay::DisplayTable() {
  if (ImGui::Button("Add sector")) {
    laps->sectors.sector_lines.push_back(laps->PickRandomStart());
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset sectors")) {
    laps->ClearSectors();
  }

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

void pacer::DeltaLapsComparision::DrawReferenceTrackLoader(Laps &laps) {
  ImGui::SetNextItemWidth(200.0f);
  ImGui::InputText("Reference track file", &reference_track_filename);
  ImGui::SameLine();
  if (ImGui::Button("Load reference track")) {
    try {
      reference_track = ReferenceTrack::FromFile(reference_track_filename);
      reference_track_status = "Loaded " +
                               std::to_string(reference_track.segments.size()) +
                               " segments";
      if (!reference_track.segments.empty()) {
        laps.sectors = reference_track.BuildSectors(cs);
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
  for (const auto &seg : reference_track.segments) {
    auto a = reference_track.cs.Global(Vec3f{seg.first.x, seg.first.y, 0});
    auto b = reference_track.cs.Global(Vec3f{seg.second.x, seg.second.y, 0});
    Vec3f line[2] = {cs.Local(a), cs.Local(b)};
    ImPlot::PlotLineG("", Vec3fToPoint, line, 2);
  }
}

// std::optional<float>
void pacer::DeltaLapsComparision::Display(const Laps &laps) {
  if (reference_track.segments.empty()) {
    return;
  }

  std::unordered_map<int, Lap> resampled_laps;

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
    }
  }

  if (ImPlot::BeginSubplots("", 2, 1, ImVec2(-1, -1),
                            ImPlotSubplotFlags_LinkAllX)) {

    if (ImPlot::BeginPlot("Telemetry", ImVec2())) {
      ImPlot::SetupAxis(ImAxis_X1, "", ImPlotAxisFlags_NoTickLabels);

      for (auto lap_id : selected_laps) {
        auto lap = reference_track.Resample(laps.GetLap(lap_id));
        resampled_laps[lap_id] = lap;
        auto data = std::pair{lap, lap_id};

        ImPlot::PlotLineG(
            std::format("lap {}", lap_id).c_str(),
            [](int index, void *data) -> ImPlotPoint {
              auto &[lap, lap_id] =
                  *reinterpret_cast<std::pair<pacer::Lap, int> *>(data);
              auto [gps, time] = lap.points[index];

              return ImPlotPoint{lap.cum_distances[index],
                                 lap.points[index].point.full_speed * 3.6};
            },
            (void *)&data, (int)lap.Count());
      }
      ImPlot::EndPlot();
    }
    if (ImPlot::BeginPlot("Delta", ImVec2(), ImPlotFlags_NoTitle)) {
      if (!selected_laps.empty()) {
        int best_lap_id = *std::min_element(
            selected_laps.begin(), selected_laps.end(),
            [&](int i, int j) { return laps.LapTime(i) < laps.LapTime(j); });

        auto &best_lap = resampled_laps[best_lap_id];
        int plot_count = static_cast<int>(best_lap.points.size());

        for (int lap_id : selected_laps) {
          auto &lap = resampled_laps[lap_id];
          int plot_count = static_cast<int>(
              std::min(lap.points.size(), best_lap.points.size()));
          if (plot_count <= 0)
            continue;

          std::tuple<Lap &, Lap &> data{lap, best_lap};
          ImPlot::PlotLineG(
              std::format("lap {}", lap_id).c_str(),
              [](int index, void *data) -> ImPlotPoint {
                auto [lap, best_lap] = *(std::tuple<Lap &, Lap &> *)data;
                auto lap_time = lap.points[index].time - lap.points[0].time;
                auto best_time =
                    best_lap.points[index].time - best_lap.points[0].time;
                assert(best_time < 1000 && best_time >= 0);
                assert(lap_time < 1000 && lap_time >= 0);

                return ImPlotPoint{best_lap.cum_distances[index],
                                   lap_time - best_time};
              },
              &data, plot_count);
        }
      }
      ImPlot::EndPlot();
    }
  }

  ImPlot::EndSubplots();
  if (dnd) {
    ImGui::EndDragDropTarget();
  }
}
