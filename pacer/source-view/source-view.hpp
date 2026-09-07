#pragma once

#include <string>
#include <vector>

#include <pacer/laps-display/laps-display.hpp>
#include <pacer/session/session.hpp>
#include <pacer/ui/track-picker.hpp>

namespace pacer {

/// ImGui drag-and-drop payload type for a lap: the payload itself is a
/// LapRef. Dragged from a source's lap chart or lap table, dropped on a
/// comparison.
inline constexpr const char *kLapDragPayload = "PACER_LAP";

/// Formats a lap time the way a timing screen does: "1:07.104". Sector
/// times are short enough to read as plain seconds, so they don't use this.
std::string FormatLapTime(double seconds);

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

  /// The source's samples laid out on its own clock: speed and fix accuracy
  /// over one shared time axis, the files that make it up as labelled bands,
  /// and draggable handles for each file's trim window. This is where a
  /// stale head, a dropout, or a clip boundary landing mid-lap is visible.
  void DrawSamplesPanel();

  /// Adopts the source's track: its coordinate system becomes the map frame.
  /// The constructor does this already; call it again after loading a track
  /// into the source from outside this view (e.g. from the command line).
  void AdoptTrack();

private:
  /// The per-file head/tail handles on the samples plot. `origin_s` is the
  /// source's first sample time, i.e. what the plot's x axis counts from.
  void DrawTrimHandles(double origin_s);

  /// Makes the item just submitted a drag source carrying `lap`.
  void LapDragSource(int lap);

  /// Order rows are drawn in, per the table's current sort. Rebuilt every
  /// frame: a session is a few hundred laps, and caching it would need
  /// invalidating on every retrim.
  std::vector<int> lap_order_;

  TrackFilePicker track_picker_;
  std::string track_status_;

  std::string files_status_;
  std::string pending_path_;

  /// Slowest lap the chart plots, as a percentage of the session best.
  float lap_cutoff_pct_ = 107;
};

} // namespace pacer
