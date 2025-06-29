#include "laps-display.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_map>

#include "imgui.h"
#include "implot.h"
#include "implot_internal.h"

#include <pacer/datatypes/datatypes.hpp>

ImPlotPoint pacer::LapsDisplay::ToImPlotPoint(GPSSample s) const {
  auto p = cs.Local(s);
  return {p[0], p[1]};
}

void pacer::LapsDisplay::DragTimingLine(Segment *s, const char *name,
                                        int drag_id) {
  auto get_point = [](int index, void *data) -> ImPlotPoint {
    auto &s = *reinterpret_cast<Segment *>(data);
    return index ? s.second : s.first;
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
    bounds = laps->MinMax();
    cs = CoordinateSystem(GPSSample{
        .lat = (bounds.first.y + bounds.second.y) / 2,
        .lon = (bounds.first.x + bounds.second.x) / 2,
        .altitude = 0,
    });
    laps->SetCoordinateSystem(cs);
    laps->sectors.start_line = laps->PickRandomStart();
    auto min_ =
        cs.Local(GPSSample{.lon = bounds.first.x, .lat = bounds.first.y});
    auto max_ =
        cs.Local(GPSSample{.lon = bounds.second.x, .lat = bounds.second.y});
    bounds = {{min_[0], min_[1]}, {max_[0], max_[1]}};

    // ImPlot::SetupAxisLimits(ImAxis_X1, min_[0], max_[0]);
    // ImPlot::SetupAxisLimits(ImAxis_Y1, min_[1], max_[1]);

    auto gp = ImPlot::GetCurrentContext();

    auto plot_size = gp->CurrentPlot->PlotRect.GetSize();

    auto x_width = std::max(bounds.second.x - bounds.first.x,
                            (bounds.second.y - bounds.first.y) * plot_size.x /
                                plot_size.y);
    auto y_width = std::max(bounds.second.y - bounds.first.y,

                            (bounds.second.x - bounds.first.x) * plot_size.y /
                                plot_size.x);

    ImPlot::SetupAxisLimits(
        ImAxis_X1, (bounds.first.x + bounds.second.x) / 2 - x_width / 2,
        (bounds.first.x + bounds.second.x) / 2 + x_width / 2, ImPlotCond_Once);

    ImPlot::SetupAxisLimits(
        ImAxis_Y1, (bounds.first.y + bounds.second.y) / 2 - y_width / 2,
        (bounds.first.y + bounds.second.y) / 2 + y_width / 2, ImPlotCond_Once);
  }

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
ImPlotPoint SampleToPoint(int index, void *data) {
  auto &s = *reinterpret_cast<pacer::DeltaLapsComparision *>(data);
  auto p = s.cs.Local(s.reference_lap.points[index].point);
  return {p[0], p[1]};
}
ImPlotPoint Vec3fToPoint(int index, void *data) {
  auto s = reinterpret_cast<pacer::Vec3f *>(data)[index];
  return {s[0], s[1]};
}

void pacer::DeltaLapsComparision::DrawSlider() {
  ImGui::SliderFloat("Width", &reference_lap.width, 0, 10);
}

void pacer::DeltaLapsComparision::PlotSticks() {
  for (size_t i = 1; i + 1 < reference_lap.Count(); ++i) {
    Vec3f prev = cs.Local(reference_lap.points[i - 1].point),
          curr = cs.Local(reference_lap.points[i].point),
          next = cs.Local(reference_lap.points[i + 1].point);

    Vec3f dir = (next - prev);
    dir /= std::sqrt(dir.Norm());
    Vec3f norm = Vec3f{dir[1], -dir[0], 0};
    Vec3f line[2] = {curr - norm * reference_lap.width,
                     curr + norm * reference_lap.width};
    ImPlot::PlotLineG("", Vec3fToPoint, line, 2);
  }
}

// std::optional<float>
void pacer::DeltaLapsComparision::Display(const Laps &laps) {
  if (reference_lap.points.size() < 1) {
    return;
  }

  std::unordered_map<int, Lap> resampled_laps;
  if (ImPlot::BeginSubplots("", 2, 1, ImVec2(-1, -1),
                            ImPlotSubplotFlags_LinkAllX)) {
    if (ImPlot::BeginPlot("Telemetry", ImVec2())) {
      ImPlot::SetupAxis(ImAxis_X1, "", ImPlotAxisFlags_NoTickLabels);
      if (ImPlot::BeginDragDropTargetPlot()) {
        if (const ImGuiPayload *payload =
                ImGui::AcceptDragDropPayload("MY_DND")) {
          int i = *(int *)payload->Data;
          if (selected_laps.contains(i)) {
            selected_laps.erase(i);
          } else {
            selected_laps.insert(i);
          }
        }
        ImPlot::EndDragDropTarget();
      }

      for (auto lap_id : selected_laps) {
        auto lap = reference_lap.Resample(laps.GetLap(lap_id), cs);
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

        for (int lap_id : selected_laps) {
          auto &lap = resampled_laps[lap_id];
          std::tuple<Lap &, Lap &> data{lap, best_lap};
          ImPlot::PlotLineG(
              std::format("lap {}", lap_id).c_str(),
              [](int index, void *data) -> ImPlotPoint {
                auto [lap, best_lap] = *(std::tuple<Lap &, Lap &> *)data;
                if (lap.points.size() <= index ||
                    best_lap.points.size() <= index) {
                  return {best_lap.cum_distances[index],
                          lap.LapTime() - best_lap.LapTime()};
                }

                auto lap_time = lap.points[index].time - lap.points[0].time;
                auto best_time =
                    best_lap.points[index].time - best_lap.points[0].time;
                assert(best_time < 1000 && best_time >= 0);
                assert(lap_time < 1000 && lap_time >= 0);

                return ImPlotPoint{best_lap.cum_distances[index],
                                   lap_time - best_time};
              },
              &data, reference_lap.points.size());
        }
      }
      ImPlot::EndPlot();
    }
  }
  ImPlot::EndSubplots();
}
