#pragma once

#include <optional>
#include <vector>

#include "implot.h"

#include <pacer/session/session.hpp>

namespace pacer {

/// The UI over one Comparison: the speed trace, the delta plot and the map,
/// all reading the comparison's laps resampled against its adopted track.
///
/// As with the source map, the app plots the satellite tiles itself (the
/// map-tiles library is desktop-only):
///   BeginPlot -> SetupComparisonMap -> [PlotSatelliteTiles]
///     -> PlotComparisonMap -> EndPlot
struct ComparisonView {
  explicit ComparisonView(Comparison *comparison);

  Comparison *comparison = nullptr;

  /// The frame the laps and the map are plotted in: the adopted track's own,
  /// so everything the comparison draws shares one set of local meters.
  CoordinateSystem cs;

  bool show_satellite = true;
  bool show_reference_track = true;

  /// Whether the telemetry subplots include the track-position trace: where
  /// each lap sits across the track's width, against the track edges.
  bool show_track_position = true;

  /// Colour of the lap in slot `index` of the comparison. Keyed by position
  /// rather than by lap id, so the first lap dropped in is always the first
  /// colour however the laps were numbered.
  static ImVec4 LapColor(int index);

  /// Re-adopts the comparison's track and refits, if it changed. Call once
  /// per frame before drawing any of the panels.
  void Update();

  /// Notes that the comparison's laps changed, so the resampling and the
  /// axis fits are redone.
  void Invalidate();

  //-------------------------------- PANELS ---------------------------------//

  /// The lap chips and the speed/delta plots. Accepts lap drops anywhere in
  /// the window.
  void Display(Session &session);

  /// Contents of a "Laps" menu: every source's laps offered for adding
  /// (with the reason a refused one cannot join), and the laps already
  /// held, for removing. Drag and drop is the quick path; this is the one
  /// that works without both windows being on screen at once.
  /// Call between BeginMenu/EndMenu.
  void DrawLapsMenu(Session &session);

  /// Fits the plot axes to the reference track once per track change.
  /// Call right after ImPlot::BeginPlot.
  void SetupComparisonMap();

  /// Plots the reference track outline and the laps' trajectories (in
  /// `cs` local meters), plus the hover markers. Hovering inside the track
  /// projects the mouse onto the track middle line and shares the resulting
  /// distance with the speed/delta plots.
  void PlotComparisonMap(const Session &session);

  //------------------------------- HOVER ----------------------------------//

  /// Publishes a hovered distance along the best lap for the current frame;
  /// every view then draws its own cursor/annotations from it.
  void SetHoverDistance(double distance);

  /// Distance published this frame or the previous one (views are drawn in
  /// separate windows, so a consumer may run before this frame's producer).
  std::optional<double> HoverDistance() const;

private:
  /// Re-resamples the comparison's laps and picks the best one, at most once
  /// per frame; every panel calls it, so each works on current data even
  /// when the others are not drawn.
  void RefreshResampled(const Session &session);

  /// Draws the row of lap chips, and returns the lap the user asked to drop
  /// from the comparison (or an invalid LapRef).
  LapRef DrawLapChips(const Session &session);

  /// Rebuilds gate_frames_ from the adopted track. Called on track change.
  void RebuildGateFrames();

  /// Recomputes lateral_ and the track edges from the current resampling.
  void RefreshLateral();

  /// Laps resampled against the comparison's track, in the same order, so a
  /// lap's slot indexes both `comparison->laps` and this.
  std::vector<Lap> resampled_;
  int resample_frame_ = -1;

  /// One per densified gate, in `cs` local meters: where the gate's middle
  /// is, the unit vector pointing left of the direction of travel, and half
  /// the annotated track width there (the gate minus its TimingLine
  /// extension). Resample() puts a lap's point k on gate k, so this turns
  /// that point into a signed offset across the track.
  struct GateFrame {
    Point mid;
    Point left;
    double half_width = 0;
  };
  std::vector<GateFrame> gate_frames_;

  /// Signed distance from the track's middle line, in meters, positive to
  /// the left of the direction of travel. lateral_[slot][k] goes with
  /// resampled_[slot].points[k]; it stops at the last gate, so it can be
  /// shorter than the lap.
  std::vector<std::vector<double>> lateral_;

  /// The track edges sampled along the best lap's distance axis, so the
  /// track-position plot shows the offsets against the boundaries they are
  /// offsets from. Rebuilt with the resampling.
  std::vector<double> edge_distance_, edge_left_, edge_right_;

  /// Slot of the quickest lap; -1 when there is none. Its cum_distances
  /// define the delta plot's x-axis / hover distance domain.
  int best_slot_ = -1;

  /// Number of laps and track the resampling was last valid for.
  size_t resampled_lap_count_ = 0;
  std::string adopted_track_path_;

  double hover_distance_ = 0;
  int hover_frame_ = -1;
  bool map_needs_fit_ = true;
  /// The delta subplots' x-axes are linked (LinkAllX), and ImPlot never
  /// initial-fits a linked axis — without an explicit fit request they'd
  /// stay at the default [0,1] range. Set on any lap/track change.
  bool plots_need_fit_ = true;
};

} // namespace pacer
