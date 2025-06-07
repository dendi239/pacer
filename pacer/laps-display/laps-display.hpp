#pragma once

#include <unordered_set>

#include "implot.h"

#include <pacer/laps/laps.hpp>

namespace pacer {

struct LapsDisplay {
  Laps &laps;
  int selected_lap = -1;

  CoordinateSystem cs;

  ImPlotPoint ToImPlotPoint(GPSSample s) const;

  std::pair<Point, Point> bounds = {{1, 1}, {0, 0}};

  void DragTimingLine(Segment *s, const char *name, int drag_id);

  void DisplayMap();

  void DisplayLapTelemetry() const;

  bool DisplayTable();
};

struct DeltaLapsComparision {
  Lap reference_lap;
  CoordinateSystem cs;

  void PlotSticks();
  void DrawSlider();

  std::unordered_set<int> selected_laps = {}; //{19, 24, 28, 35, 36};

  void Display(const Laps &laps);
};

} // namespace pacer
