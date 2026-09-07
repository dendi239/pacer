#pragma once

#include <string>

#include <pacer/laps-display/laps-display.hpp>
#include <pacer/session/session.hpp>
#include <pacer/ui/track-picker.hpp>

namespace pacer {

/// The UI over one Source: the panels the user sets a recording up with.
/// Each panel draws its own contents only -- the app owns the windows they
/// live in, so they can be docked, closed and reopened independently.
///
/// The map is the one exception: it needs the app's TileStore (map-tiles is
/// desktop-only), so the app drives it through `display` directly, as
///   BeginPlot -> display.SetupMap -> [PlotSatelliteTiles]
///     -> display.PlotMapItems -> EndPlot
struct SourceView {
  explicit SourceView(Source *source);

  Source *source = nullptr;
  LapsDisplay display;

  /// Rebuilds the source if needed and refits the views when it changed.
  /// Call once per frame, before drawing any panel.
  void Update();

  //-------------------------------- PANELS ---------------------------------//

  /// Track selector: which reference track splits this source into laps.
  void DrawTrackPanel();

  /// The recordings that make up this source, in order, with per-file
  /// trimming.
  void DrawFilesPanel();

  /// Lap time per lap, with a cutoff that hides laps slower than a share of
  /// the session best (a full course yellow shouldn't set the y-scale).
  void DrawLapChartPanel();

  /// Lap and sector times.
  void DrawLapTablePanel();

private:
  /// Adopts the track's coordinate system as the map frame and notes the
  /// track's shape in `track_status_`.
  void ApplyTrack();

  TrackFilePicker track_picker_;
  std::string track_status_;

  std::string files_status_;
  std::string pending_path_;

  /// Slowest lap the chart plots, as a percentage of the session best.
  float lap_cutoff_pct_ = 107;
};

} // namespace pacer
