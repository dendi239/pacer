#include "laps-display.hpp"

#include <sstream>

#include "implot_internal.h"

ImPlotPoint pacer::LapsDisplay::ToImPlotPoint(GPSSample s) const {
  auto p = cs.Local(s);
  return {p[0], p[1]};
}

ImPlotPoint StartGetter(int index, void *data) {
  pacer::Laps *laps = reinterpret_cast<pacer::Laps *>(data);
  return index ? laps->start_line.second : laps->start_line.first;
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
    bounds = laps.MinMax();
    cs = CoordinateSystem(GPSSample{
        .lat = (bounds.first.y + bounds.second.y) / 2,
        .lon = (bounds.first.x + bounds.second.x) / 2,
        .altitude = 0,
    });
    laps.SetCoordinateSystem(cs);
    laps.start_line = laps.PickRandomStart();
    auto min_ =
        cs.Local(GPSSample{.lon = bounds.first.x, .lat = bounds.first.y});
    auto max_ =
        cs.Local(GPSSample{.lon = bounds.second.x, .lat = bounds.second.y});
    bounds = {{min_[0], min_[1]}, {max_[0], max_[1]}};

    // ImPlot::SetupAxisLimits(ImAxis_X1, min_[0], max_[0]);
    // ImPlot::SetupAxisLimits(ImAxis_Y1, min_[1], max_[1]);
  }
  auto gp = ImPlot::GetCurrentContext();

  auto plot_size = gp->CurrentPlot->PlotRect.GetSize();

  auto x_width =
      std::max(bounds.second.x - bounds.first.x,
               (bounds.second.y - bounds.first.y) * plot_size.x / plot_size.y);
  auto y_width =
      std::max(bounds.second.y - bounds.first.y,

               (bounds.second.x - bounds.first.x) * plot_size.y / plot_size.x);

  ImPlot::SetupAxisLimits(
      ImAxis_X1, (bounds.first.x + bounds.second.x) / 2 - x_width / 2,
      (bounds.first.x + bounds.second.x) / 2 + x_width / 2, ImPlotCond_Always);

  ImPlot::SetupAxisLimits(
      ImAxis_Y1, (bounds.first.y + bounds.second.y) / 2 - y_width / 2,
      (bounds.first.y + bounds.second.y) / 2 + y_width / 2, ImPlotCond_Always);

  ImPlot::PlotLineG(
      "trace",
      [](int index, void *data) {
        auto &ld = *reinterpret_cast<LapsDisplay *>(data);
        return ld.ToImPlotPoint(ld.laps.GetPoint(index).point);
      },
      reinterpret_cast<void *>(this), (int)laps.PointCount());

  DragTimingLine(&laps.start_line, "Start", 0);
  for (int i = 0; i < laps.SectorCount(); ++i) {
    auto &s = laps.sector_lines[i];
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
          auto [gps, time] = ld.laps.At(ld.selected_lap, index);

          return ImPlotPoint{ld.laps.Distance(ld.selected_lap, index),
                             ld.laps.Speed(ld.selected_lap, index) * 3.6};
        },
        (void *)this, (int)laps.SampleCount(selected_lap));

    ImPlot::EndPlot();
  }
}
bool pacer::LapsDisplay::DisplayTable() {
  if (ImGui::Button("Add sector")) {
    laps.sector_lines.push_back(laps.PickRandomStart());
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset sectors")) {
    laps.ClearSectors();
  }

  size_t sector_count = 1 + laps.SectorCount();
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

  for (int row = 0, i_sector = 0; row < laps.LapsCount(); ++row) {
    ImGui::TableNextRow();

    ImGui::TableSetColumnIndex(0);
    ImGui::Text("%.3f", laps.StartTimestamp(row));

    ImGui::TableSetColumnIndex(1);
    ImGui::Text("%zu", laps.SampleCount(row));

    ImGui::TableSetColumnIndex(2);
    ImGui::Text("%.2f", laps.GetLapDistance(row, cs));

    ImGui::TableSetColumnIndex(3);
    if (ImGui::Button(std::format("{:.3f}", laps.LapTime(row)).c_str())) {
      selected_lap = row == selected_lap ? -1 : row;
    }

    for (int i = 0; i < sector_count; ++i, ++i_sector) {
      ImGui::TableSetColumnIndex(4 + 2 * i);
      if (i_sector < laps.RecordedSectors()) {

        ImGui::Text("%.3fkph", laps.SectorEntrySpeed(i_sector) * 3.6);
      }
      ImGui::TableSetColumnIndex(5 + 2 * i);

      if (i_sector < laps.RecordedSectors()) {
        ImGui::Text("%.3fs", laps.SectorTime(i_sector));
      }
    }
  }

  ImGui::EndTable();
  return true;
}
