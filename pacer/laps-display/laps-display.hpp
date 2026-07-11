#pragma once

#include <string>
#include <unordered_set>

#include "implot.h"

#include <pacer/laps/laps.hpp>
#include <pacer/reference-track/reference-track.hpp>
#include <pacer/ui/track-picker.hpp>

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

  /// True once SetupMap has derived a coordinate system from loaded points,
  /// i.e. cs maps plot coordinates to real lat/lon.
  bool HasMapFrame() const;

  /// Initializes cs/bounds from the loaded points and fits the plot axes.
  /// Call right after ImPlot::BeginPlot, before plotting any item.
  void SetupMap();

  /// Plots the GPS trace plus the start/sector timing lines (read-only;
  /// edit the geometry in track_annotator).
  void PlotMapItems();

  void DisplayLapTelemetry() const;

  bool DisplayTable();
};

struct DeltaLapsComparision {
  ReferenceTrack reference_track;
  CoordinateSystem cs;

  TrackFilePicker reference_track_picker;
  std::string reference_track_status;

  void PlotSticks();
  void DrawReferenceTrackLoader(Laps &laps);

  std::unordered_set<int> selected_laps = {};

  void Display(const Laps &laps);
};

} // namespace pacer
