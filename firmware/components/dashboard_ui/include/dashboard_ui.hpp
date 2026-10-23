#pragma once

// The driver-facing screen: NV3041A QSPI panel + GT911 touch + LVGL.
// Layout (480x272 landscape):
//
//   LAP 9                        8:12   <- lap number / session elapsed
//                           L4   48.640 <- recent laps, oldest on top,
//                           L5   49.104    dimmed (see lap_table.hpp)
//       -0.42               L6   48.877
//       48.715              L7   49.015
//                           LAST 48.912
//                           BEST 48.299
//    ^ delta to the reference lap (session best, or the best lap of a
//      recording picked in the debug menu), huge, green negative / red
//      positive; under it the lap time if the rest of the lap matches it
//   [status: sats / track / logging]

#include <cstddef>
#include <string>
#include <vector>

#include "esp_err.h"

#include <pacer/geometry/geometry.hpp>
#include <pacer/live-timing/live-timing.hpp>

esp_err_t dashboard_ui_start();

/// Repaints the timing fields; safe to call from any task (locks LVGL).
void dashboard_ui_update(const pacer::LiveSnapshot &snap);

/// One-line status strip at the bottom (GPS/sd/track info).
void dashboard_ui_set_status(const char *text);

/// Debug readout in the bottom-right corner (raw lat/lon/speed).
void dashboard_ui_set_debug(const char *text);

/// True exactly once after the user picks an entry on the debug menu's
/// "Reload track" page (long press -> menu -> Reload track); the main loop
/// polls this. `path_out` gets the chosen track file, or an empty string for
/// "Nearest (auto)", i.e. the usual scan of /sdcard/tracks.
bool dashboard_ui_consume_track_reload(std::string *path_out = nullptr);

/// One entry of the debug menu's track picker.
struct DashboardTrack {
  /// Track file; handed back by dashboard_ui_consume_track_reload. Only the
  /// file name (minus the .json) is shown.
  std::string path;
  /// Closed outline for the tile's thumbnail, in any local metric frame (the
  /// UI fits it to the thumbnail on its own). Keep it short — every point is
  /// held for as long as the entry is listed. Empty draws no thumbnail,
  /// which is what a file that wouldn't parse gets.
  std::vector<pacer::Point> outline;
};

/// Fills the full-screen "Reload track" page with one tile per track, after
/// the always-present "Nearest (auto)" tile. `active_path` is drawn as the
/// loaded one (green outline and border); pass an empty string for none.
/// Safe to call from any task (locks LVGL).
void dashboard_ui_set_track_list(const std::vector<DashboardTrack> &tracks,
                                 const std::string &active_path = {});

/// One entry of the debug menu's "Reference lap" picker: a session log to
/// take the delta reference from.
struct DashboardRecording {
  /// Log file; handed back by dashboard_ui_consume_reference_pick. Only the
  /// file name (minus the .dat) is shown.
  std::string path;
  double duration_s = 0;
  /// The log this boot is writing: listed as "now", since its length is
  /// out of date as soon as the list is.
  bool live = false;
  /// Track file the recording was most likely driven on (nearest start
  /// line); only the name is shown. Empty: no track on the card is close.
  std::string track_path;
  /// Whether track_path is the track loaded now. Rows that aren't are
  /// dimmed — picking one will find no lap on the loaded gates.
  bool on_loaded_track = false;
};

/// Fills the "Reference lap" page (long press -> menu -> Reference lap) with
/// one row per recording, after the always-present "Session best" row (no
/// recorded reference). `active_path` is drawn as the one in use, with
/// `active_lap_s` — the reference lap it gave — next to it; pass an empty
/// path for none. Safe to call from any task (locks LVGL).
void dashboard_ui_set_recording_list(
    const std::vector<DashboardRecording> &recordings,
    const std::string &active_path = {}, double active_lap_s = 0);

/// True exactly once after the user picks a row on the "Reference lap"
/// page; the main loop polls this. `path_out` gets the chosen log, or an
/// empty string for "Session best", i.e. no recorded reference.
bool dashboard_ui_consume_reference_pick(std::string *path_out);

/// The "Demo" page (long press -> menu -> Demo) lists the same recordings as
/// "Reference lap", after a "Live GPS" row (no demo), with a speed button in
/// its header. Marks `active_path` as the one playing; pass an empty string
/// once the demo stops. Safe to call from any task (locks LVGL).
void dashboard_ui_set_demo_recording(const std::string &active_path);

/// True exactly once after the user picks a row on the "Demo" page; the
/// main loop polls this. `path_out` gets the log to play, or an empty string
/// for "Live GPS", i.e. stop the demo.
bool dashboard_ui_consume_demo_pick(std::string *path_out);

/// The demo page's playback speed multiplier (1, 2, 4, 8 or 16; starts at
/// 1). Can change while a demo plays; the main loop polls it.
int dashboard_ui_demo_speed();

/// Whether a track is loaded. Without one the debug menu's "Reference lap"
/// tile is disabled: a recorded lap needs gates to be timed on. Safe to call
/// from any task (locks LVGL).
void dashboard_ui_set_track_loaded(bool loaded);

/// Installs the track outline for the map page (long press -> "Track map"):
/// the annotated gate segments in any local metric frame with the origin
/// somewhere near the track. The page renders the same infill
/// track_annotator draws: a filled quad between each consecutive pair of
/// gates plus the wraparound quad (a loaded track is always closed), colored
/// per sector, under the two edge polylines. `sector_splits` are indices of
/// gates that end a sector (ReferenceTrack::sector_indices); pass empty for
/// a single-color track. An empty `gates` clears the map back to "no
/// track". Safe to call from any task (locks LVGL).
///
/// The page's "Zoom" button switches to a second view at a fixed ~80 m
/// across, following the position (drag to pan, "Follow" to snap back),
/// which also draws each gate's timing line — extended past both edges by
/// the gate extension slider's value, previewed live while dragging — and
/// carries a -/slider/+ bar driving the same value as the menu's "Gate
/// extension" page. "Fit" goes back to the whole track.
void dashboard_ui_set_track_map(const std::vector<pacer::Segment> &gates,
                                const std::vector<int> &sector_splits = {});

/// True while the track-map page is open. The main loop checks this before
/// converting the fix into the map frame — no point paying for it (or the
/// LVGL lock) with nobody watching.
bool dashboard_ui_track_map_visible();

/// Current position in the same frame as dashboard_ui_set_track_map; the
/// map re-fits so track, position and a margin stay on screen. Cheap no-op
/// while the page is closed.
void dashboard_ui_set_track_map_position(double x_m, double y_m);

/// Value shown on the debug menu's "Gate extension" slider, in meters
/// (ReferenceTrack::gate_extension_m). Call once at startup with the value
/// the main loop loads tracks with. Safe to call from any task.
void dashboard_ui_set_gate_extension(double meters);

/// True exactly once after the user lets go of the gate extension slider on
/// a new value; `meters_out` gets it. The main loop polls this and rebuilds
/// the live gates (LiveTiming::UpdateGates) without resetting the session.
bool dashboard_ui_consume_gate_extension(double *meters_out);

/// True exactly once after the user taps the debug menu's "Reset session"
/// tile; the main loop polls this and starts the session over
/// (LiveTiming::ResetSession) on the loaded track.
bool dashboard_ui_consume_session_reset();

/// Current state of the debug menu's logging toggle (defaults to on).
bool dashboard_ui_logging_enabled();

/// Counters for the debug menu's logging page; cheap no-op while closed.
void dashboard_ui_set_log_stats(size_t written, size_t flushed);

void dashboard_ui_set_battery_level(uint8_t level);

void dashboard_ui_toggle_touch_enabled(void);

void dashboard_ui_show_menu(bool show);
