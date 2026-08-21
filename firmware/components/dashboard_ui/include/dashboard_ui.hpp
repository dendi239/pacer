#pragma once

// The driver-facing screen: NV3041A QSPI panel + GT911 touch + LVGL.
// Layout (480x272 landscape):
//
//   LAP 7                        8:12   <- lap number / session elapsed
//              -0.42                    <- delta to session best, huge,
//              48.7                        green negative / red positive
//   LAST 48.912       BEST 48.299      <- completed lap times
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

/// Live value for the debug menu's "next timing line" page; NaN shows
/// "no track". Cheap no-op while that page is closed.
void dashboard_ui_set_next_line_distance(double meters);

/// True while the debug menu's "Track offset" page is open. The main loop
/// checks this before running LiveTiming::OffsetFromTrack(), which scans
/// every gate — no point paying for it with nobody watching.
bool dashboard_ui_track_offset_visible();

/// Values for the track-offset page (see pacer::TrackOffset); lateral NaN
/// shows "no track". The offset turns red once |lateral| > half_width, i.e.
/// the fix left the annotated track. Cheap no-op while the page is closed.
void dashboard_ui_set_track_offset(double lateral_m, double half_width_m,
                                   size_t gate, size_t gate_count);

/// Installs the track outline for the map page (long press -> "Track map"):
/// the annotated gate segments in any local metric frame with the origin
/// somewhere near the track. The page renders the same infill
/// track_annotator draws: a filled quad between each consecutive pair of
/// gates plus the wraparound quad (a loaded track is always closed), colored
/// per sector, under the two edge polylines. `sector_splits` are indices of
/// gates that end a sector (ReferenceTrack::sector_indices); pass empty for
/// a single-color track. An empty `gates` clears the map back to "no
/// track". Safe to call from any task (locks LVGL).
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

/// Live receiver state for the debug menu's "GPS" page. The page pairs this
/// with the static configuration the firmware pushed at boot, so the two can
/// be read against each other: `corrections` and `interval_ms` are what say
/// whether SBAS actually locked and whether the requested rate survived it.
struct DashboardGpsState {
  /// uGnssDecUbxNavPvt_t::fixType (0 none, 2 2D, 3 3D, 4 GNSS+DR).
  int fix_type = 0;
  /// The receiver's own gnssFixOK bit — a fixType can read 3D while the
  /// solution is still outside the configured masks.
  bool fix_ok = false;
  int num_sv = 0;
  double h_acc_m = 0;
  /// pDOP in its natural units, i.e. the PVT field already divided by 100.
  double pdop = 0;
  /// diffSoln: differential (SBAS/RTCM) corrections were applied.
  bool diff_soln = false;
  /// carrSoln: 0 none, 1 RTK float, 2 RTK fixed.
  int carr_soln = 0;
  /// Measured spacing between fixes; 0 until two have arrived. Compare
  /// against the configured rate — a receiver that cannot hold the rate it
  /// was asked for reports the shortfall here and nowhere else.
  double interval_ms = 0;
};

/// Cheap no-op while the GPS page is closed.
void dashboard_ui_set_gps_state(const DashboardGpsState &state);

/// The boot-time configuration block on the GPS page, shown in grey under
/// the live figures — pass ubx_gps_config_summary(). Static text, so call it
/// once at startup; the UI keeps no dependency on the GPS component itself.
void dashboard_ui_set_gps_config(const char *text);

/// Current state of the debug menu's logging toggle (defaults to on).
bool dashboard_ui_logging_enabled();

/// Counters for the debug menu's logging page; cheap no-op while closed.
void dashboard_ui_set_log_stats(size_t written, size_t flushed);
