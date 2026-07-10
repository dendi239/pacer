#pragma once

#include <string>
#include <unordered_set>

#include "implot.h"

#include <pacer/laps/laps.hpp>
#include <pacer/reference-track/reference-track.hpp>

namespace pacer {

/// ImPlot getter over a raw GPSSample array (lon/lat). Lived in
/// pacer::geometry before; moved here so the core geometry library has no
/// implot dependency and can be compiled for embedded targets.
ImPlotPoint ToImPlotPoint(int index, void *data);

struct LapsDisplay {
  Laps *laps;
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
  ReferenceTrack reference_track;
  CoordinateSystem cs;

  std::string reference_track_filename = "track_annotation.json";
  std::string reference_track_status;

  void PlotSticks();
  void DrawReferenceTrackLoader(Laps &laps);

  std::unordered_set<int> selected_laps = {}; //{19, 24, 28, 35, 36};

  void Display(const Laps &laps);
};

} // namespace pacer
