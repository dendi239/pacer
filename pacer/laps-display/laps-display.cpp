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
