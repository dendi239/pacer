#pragma once

// The driver-facing screen: NV3041A QSPI panel + GT911 touch + LVGL.
// Layout (480x272 landscape):
//
//   LAP 7                       12:34   <- lap number / session countdown
//              -0.42                    <- delta to session best, huge,
//              48.7                        green negative / red positive
//   LAST 48.912       BEST 48.299      <- completed lap times
//   [status: sats / track / logging]

#include <cstddef>
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

/// True exactly once after the user picks "Reload track" in the debug menu
/// (long press -> menu); the main loop polls this.
bool dashboard_ui_consume_track_reload();

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

/// Current state of the debug menu's logging toggle (defaults to on).
bool dashboard_ui_logging_enabled();

/// Counters for the debug menu's logging page; cheap no-op while closed.
void dashboard_ui_set_log_stats(size_t written, size_t flushed);
