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

  /// True once a frame was supplied via SetMapFrame (e.g. by a reference
  /// track); SetupMap then keeps that frame instead of deriving one from
  /// the loaded points.
  bool has_supplied_frame = false;

  ImPlotPoint ToImPlotPoint(GPSSample s) const;

  std::pair<Point, Point> bounds = {{1, 1}, {0, 0}};

  /// True once cs maps plot coordinates to real lat/lon — either supplied
  /// via SetMapFrame or derived from loaded points by SetupMap.
  bool HasMapFrame() const;

  /// Adopts `frame` as the map coordinate system (typically the reference
  /// track's cs, so sectors/delta/display all share one frame that outlives
  /// any particular set of loaded data files). Propagates it to the laps and
  /// schedules an axis refit on the next SetupMap.
  void SetMapFrame(const CoordinateSystem &frame);

  /// Initializes cs/bounds from the loaded points and fits the plot axes.
  /// If a frame was supplied via SetMapFrame, only the bounds/axes are
  /// refit; the supplied frame is kept.
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

  /// UI-owned copy of ReferenceTrack::gate_extension_m; survives reloads
  /// (FromFile resets the track to the default) and is re-applied to the
  /// loaded track every frame by DrawReferenceTrackLoader.
  float gate_extension_m = 2.0f;

  void PlotSticks();

  /// Draws the picker/load UI. On a successful load the reference track's
  /// own coordinate system becomes the map frame: it is pushed into
  /// `display` (and from there into `laps`), adopted as this->cs, and the
  /// sectors are built directly in it.
  void DrawReferenceTrackLoader(Laps &laps, LapsDisplay &display);

  std::unordered_set<int> selected_laps = {};

  void Display(const Laps &laps);
};

} // namespace pacer
