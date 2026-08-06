#pragma once

#include <optional>
#include <string>
#include <unordered_map>
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

  /// Stable per-lap color used by the speed trace, delta plot and the
  /// comparison map, so a lap is recognizable across all three views.
  static ImVec4 LapColor(int lap_id);

  void Display(const Laps &laps);

  //--------------------------- COMPARISON MAP -----------------------------//
  // The map itself is plotted by the app (it owns the TileStore; the
  // map-tiles library is desktop-only) as:
  //   BeginPlot -> SetupComparisonMap -> [PlotSatelliteTiles]
  //     -> PlotComparisonMap -> EndPlot

  bool show_satellite = true;
  bool show_reference_track = true;

  /// Fits the plot axes to the reference track once per track load.
  /// Call right after ImPlot::BeginPlot.
  void SetupComparisonMap();

  /// Plots the reference track gates and the selected laps' trajectories
  /// (in this->cs local meters), plus the hover markers. Hovering the plot
  /// inside the track bounds projects the mouse onto the track middle line
  /// and shares the resulting distance with the speed/delta plots.
  void PlotComparisonMap(const Laps &laps);

  //------------------------------- HOVER ----------------------------------//

  /// Publishes a hovered distance along the (best) lap for the current
  /// frame; every view then draws its own cursor/annotations from it.
  void SetHoverDistance(double distance);

  /// Distance published this frame or the previous one (views are drawn in
  /// separate windows, so a consumer may run before this frame's producer).
  std::optional<double> HoverDistance() const;

private:
  /// Re-resamples the selected laps and picks the best lap, at most once per
  /// frame; both Display and PlotComparisonMap call it so each window works
  /// on current data even when the other one is not drawn.
  void RefreshResampled(const Laps &laps);

  /// Laps resampled against the reference track, refreshed once per frame
  /// and shared between the delta plots and the comparison map.
  std::unordered_map<int, Lap> resampled_laps_;
  int resample_frame_ = -1;
  /// Lap (among selected) with the smallest lap time; -1 when none. Its
  /// cum_distances define the delta plot's x-axis / hover distance domain.
  int best_lap_id_ = -1;

  double hover_distance_ = 0;
  int hover_frame_ = -1;
  bool map_needs_fit_ = true;
  /// The delta subplots' x-axes are linked (LinkAllX), and ImPlot never
  /// initial-fits a linked axis — without an explicit fit request they'd
  /// stay at the default [0,1] range. Set on any selection/track change.
  bool plots_need_fit_ = true;
};

} // namespace pacer
