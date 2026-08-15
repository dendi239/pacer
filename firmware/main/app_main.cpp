// In-kart live timing dashboard.
//
// Data flow:
//   ubx_gps reader task --(queue)--> main loop:
//     log to SD (.dat, same format the desktop tools read)
//     -> LiveTiming::OnSample -> dashboard_ui at ~10 Hz
//
// On the first 2D/3D fix, the nearest /sdcard/tracks/*.json annotation is
// loaded as the reference track; until then the screen shows fix status.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "soc/rtc_cntl_reg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

#include <pacer/datatypes/datatypes.hpp>
#include <pacer/geometry/geometry.hpp>
#include <pacer/live-timing/live-timing.hpp>
#include <pacer/reference-track/reference-track.hpp>

#include "dashboard_ui.hpp"
#include "storage.hpp"
#include "ubx_gps.hpp"

namespace {

const char *TAG = "pacer";

QueueHandle_t s_pvt_queue = nullptr;

void OnPvt(const uGnssDecUbxNavPvt_t &pvt, void *) {
  // Reader task context: hand off and get back to the UART. Dropping on a
  // full queue is fine — the consumer only stalls on SD flush hiccups.
  xQueueSend(s_pvt_queue, &pvt, 0);
}

bool HasFix(const uGnssDecUbxNavPvt_t &pvt) {
  return pvt.fixType == U_GNSS_DEC_UBX_NAV_PVT_FIX_TYPE_2D ||
         pvt.fixType == U_GNSS_DEC_UBX_NAV_PVT_FIX_TYPE_3D ||
         pvt.fixType == U_GNSS_DEC_UBX_NAV_PVT_FIX_TYPE_GNSS_PLUS_DEAD_RECKONING;
}

// Worth writing to the .dat? fixType alone reads 2D/3D while the solution is
// still converging, and those early epochs sit hundreds of metres off — they
// pollute the log and, on a re-run, the track scan. gnssFixOK is the
// receiver's own verdict on the same question; hAcc caps how far off the
// samples that pass it may be (0 = keep whatever gnssFixOK accepts).
bool IsLogWorthy(const uGnssDecUbxNavPvt_t &pvt) {
  if (!HasFix(pvt) || !(pvt.flags & U_GNSS_DEC_UBX_NAV_PVT_FLAGS_GNSS_FIX_OK)) {
    return false;
  }
  constexpr uint32_t kMaxHAccMm = CONFIG_PACER_GPS_LOG_MAX_HACC_M * 1000u;
  return kMaxHAccMm == 0 || pvt.hAcc <= kMaxHAccMm;
}

pacer::GPSSample ToSample(const uGnssDecUbxNavPvt_t &pvt) {
  return pacer::GPSSample{
      .lat = static_cast<double>(pvt.lat) / 1e7,
      .lon = static_cast<double>(pvt.lon) / 1e7,
      .altitude = pvt.height / 1000.0,
      .full_speed = pvt.gSpeed / 1000.0,
      .ground_speed = pvt.gSpeed / 1000.0,
      .timestamp_ms = static_cast<int64_t>(pvt.iTOW),
  };
}

// Outline for a track picker thumbnail: the gate midpoints in the track's
// own local frame, thinned to at most kThumbPoints and closed back to the
// start. Thinning matters — a densely annotated track has hundreds of gates,
// and every point kept is held for as long as the picker lists it, to draw
// something 72 px wide.
constexpr size_t kThumbPoints = 48;

std::vector<pacer::Point> TrackOutline(const pacer::ReferenceTrack &rt) {
  std::vector<pacer::Point> outline;
  size_t n = rt.segments.size();
  if (n < 3) {
    return outline;
  }
  size_t stride = (n + kThumbPoints - 1) / kThumbPoints;
  for (size_t i = 0; i < n; i += stride) {
    outline.push_back((rt.segments[i].first + rt.segments[i].second) / 2.0);
  }
  outline.push_back(outline.front());
  return outline;
}

// Diagnostic for the reset-into-download-mode issue: GPIO0 is the boot strap;
// something appears to hold it low across warm resets. Samples the pin 1000x
// floating and 1000x with pull-up: low-with-pullup means actively driven low.
void LogStrapDiag(const char *when) {
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << GPIO_NUM_0;
  cfg.mode = GPIO_MODE_INPUT;
  gpio_config(&cfg);
  int low_float = 0, low_pullup = 0;
  for (int i = 0; i < 1000; ++i) {
    low_float += gpio_get_level(GPIO_NUM_0) == 0;
    esp_rom_delay_us(100);
  }
  gpio_pullup_en(GPIO_NUM_0);
  for (int i = 0; i < 1000; ++i) {
    low_pullup += gpio_get_level(GPIO_NUM_0) == 0;
    esp_rom_delay_us(100);
  }
  ESP_LOGI("strap-diag",
           "%s: reset_reason=%d rtc_option1=0x%08x gpio0 low: %d/1000 float, "
           "%d/1000 pullup",
           when, (int)esp_reset_reason(),
           (unsigned)REG_READ(RTC_CNTL_OPTION1_REG), low_float, low_pullup);
}

void StrapDiagTask(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    LogStrapDiag("periodic");
  }
}

} // namespace

extern "C" void app_main(void) {
  LogStrapDiag("boot");
  xTaskCreate(StrapDiagTask, "strap_diag", 4096, nullptr, 1, nullptr);
  ESP_ERROR_CHECK(dashboard_ui_start());
  dashboard_ui_set_status("mounting sd card...");

  bool sd_ok = storage_mount() == ESP_OK;
  double session_minutes = CONFIG_PACER_SESSION_MINUTES;
  if (sd_ok) {
    session_minutes = storage_session_minutes(session_minutes);
    storage_log_open();
  }

  s_pvt_queue = xQueueCreate(64, sizeof(uGnssDecUbxNavPvt_t));
  ESP_ERROR_CHECK(ubx_gps_start(OnPvt, nullptr));
  dashboard_ui_set_status(sd_ok ? "waiting for gps fix..."
                                : "NO SD CARD - waiting for gps fix...");

  pacer::LiveTiming timing;
  bool track_loaded = false;
  std::string track_name;
  std::string track_path;

  // Entries offered by the debug menu's track picker, refreshed whenever we
  // touch the card for a reload.
  std::vector<DashboardTrack> track_list;

  // Frame for the track-map page: origin at the "median" of the track (mean
  // of the annotated gate midpoints), so the outline is centered on its own
  // geometry rather than on whichever gate the track file happens to start
  // with. Valid only while track_loaded.
  pacer::CoordinateSystem map_cs;
  bool map_ready = false;

  // Installs `path` as the reference track (and its outline on the map page).
  // Throws nothing: a file that won't parse just leaves the track unloaded.
  auto load_track = [&](const std::string &path) {
    try {
      auto rt = pacer::ReferenceTrack::FromFile(path);
      timing.SetReferenceTrack(
          rt, pacer::SessionConfig{.session_length_s = session_minutes * 60.0});
      track_loaded = true;
      if (!rt.segments.empty()) {
        pacer::Point median{};
        for (const auto &seg : rt.segments) {
          median += (seg.first + seg.second) / 2.0;
        }
        median /= static_cast<double>(rt.segments.size());
        map_cs = pacer::CoordinateSystem(
            rt.cs.Global(pacer::Vec3f{median.x, median.y, 0}));
        // Re-express the annotated gates in the map frame; the UI draws
        // their endpoints as the two track edges.
        auto to_map = [&](const pacer::Point &p) {
          return pacer::ToPoint(
              map_cs.Local(rt.cs.Global(pacer::Vec3f{p.x, p.y, 0})));
        };
        std::vector<pacer::Segment> map_gates;
        map_gates.reserve(rt.segments.size());
        for (const auto &seg : rt.segments) {
          map_gates.push_back(
              pacer::Segment{to_map(seg.first), to_map(seg.second)});
        }
        dashboard_ui_set_track_map(map_gates, rt.sector_indices);
        map_ready = true;
      }
      track_path = path;
      size_t slash = path.rfind('/');
      track_name = slash == std::string::npos ? path : path.substr(slash + 1);
      return std::string();
    } catch (const std::exception &e) {
      ESP_LOGE(TAG, "loading %s: %s", path.c_str(), e.what());
      return std::string(e.what());
    }
  };

  // Re-reads the card's track files for the picker. Each one is parsed for
  // its outline; they are small and few, and the nearest-track scan parses
  // the same files once a second anyway.
  auto rescan_track_list = [&] {
    track_list.clear();
    if (!sd_ok) {
      return;
    }
    for (const std::string &path : storage_list_tracks()) {
      DashboardTrack entry;
      entry.path = path;
      try {
        entry.outline = TrackOutline(pacer::ReferenceTrack::FromFile(path));
      } catch (const std::exception &e) {
        // Listed anyway, without a thumbnail: seeing the name and getting a
        // load error beats the file silently not existing.
        ESP_LOGW(TAG, "picker: %s: %s", path.c_str(), e.what());
      }
      track_list.push_back(std::move(entry));
    }
  };

  rescan_track_list();
  dashboard_ui_set_track_list(track_list, track_path);

  uGnssDecUbxNavPvt_t pvt;
  int samples_since_ui = 0;
  int samples_since_debug = 0;
  int samples_until_scan = 1;
  bool was_logging = false;
  // Cleared when the picker names a track: an explicit choice is not
  // second-guessed by the nearest-track scan, even if it failed to load.
  bool auto_scan = true;

  while (true) {
    if (xQueueReceive(s_pvt_queue, &pvt, pdMS_TO_TICKS(500)) != pdTRUE) {
      continue;
    }

    // Debug menu action: drop the track and session state, re-read config,
    // then either load the track the picker named or rescan /sdcard/tracks
    // for the nearest one on the next samples.
    std::string picked;
    if (dashboard_ui_consume_track_reload(&picked)) {
      track_loaded = false;
      track_name.clear();
      track_path.clear();
      timing = pacer::LiveTiming{};
      map_ready = false;
      dashboard_ui_set_track_map({});
      if (sd_ok) {
        session_minutes = storage_session_minutes(CONFIG_PACER_SESSION_MINUTES);
        rescan_track_list();
      }
      samples_until_scan = 1;
      auto_scan = picked.empty();
      if (picked.empty()) {
        dashboard_ui_set_status("track reload requested...");
        ESP_LOGI(TAG, "track reload requested");
      } else {
        // A named track needs no fix and no scan — load it right here.
        ESP_LOGI(TAG, "track picked: %s", picked.c_str());
        std::string err = load_track(picked);
        if (!err.empty()) {
          std::string status = "cannot load " + picked + ": " + err;
          dashboard_ui_set_status(status.c_str());
        }
      }
      dashboard_ui_set_track_list(track_list, track_path);
    }

    // Debug menu toggle gates the log; on pause, commit what's pending so
    // pulling the card right after is safe. Poor-quality samples are dropped
    // rather than pausing the log: the toggle is the user's intent, this is
    // just the receiver having a bad moment, and the session continues either
    // way (the .dat carries its own timestamps, so a gap reads as a gap).
    bool logging = sd_ok && dashboard_ui_logging_enabled();
    if (was_logging && !logging) {
      storage_log_flush();
    }
    was_logging = logging;
    if (logging && IsLogWorthy(pvt)) {
      storage_log_append(esp_timer_get_time() / 1000, pvt);
    }

    // Raw receiver state in the corner, unconditionally — the point is to
    // see what the GPS reports even before a fix or track. hAcc is here so
    // an unexpectedly idle log counter has a visible explanation.
    if (++samples_since_debug >= 5) {
      samples_since_debug = 0;
      char dbg[96];
      snprintf(dbg, sizeof(dbg), "%.6f %.6f  %.1f km/h  %d sat  %.1f m",
               pvt.lat / 1e7, pvt.lon / 1e7, pvt.gSpeed * 0.0036, pvt.numSV,
               pvt.hAcc / 1000.0);
      dashboard_ui_set_debug(dbg);
      dashboard_ui_set_log_stats(storage_log_appended(), storage_log_flushed());
    }

    if (!HasFix(pvt)) {
      char status[64];
      snprintf(status, sizeof(status), "no fix (%d sats)", pvt.numSV);
      dashboard_ui_set_status(status);
      continue;
    }

    pacer::GPSSample sample = ToSample(pvt);

    if (!track_loaded) {
      if (sd_ok && auto_scan && --samples_until_scan <= 0) {
        samples_until_scan = 25; // rescan ~1/s, not per 25 Hz sample
        double dist = 0;
        std::string scan_debug;
        std::string path =
            storage_find_track(sample.lat, sample.lon, &dist, &scan_debug);
        if (!path.empty()) {
          std::string err = load_track(path);
          if (err.empty()) {
            // Tick the picker on whatever the scan settled on.
            dashboard_ui_set_track_list(track_list, track_path);
            ESP_LOGI(TAG, "using track %s (%.0f m away)", track_name.c_str(),
                     dist);
          } else {
            scan_debug += "  load: " + err;
          }
        }
        if (!track_loaded) {
          std::string status = "no track | " + scan_debug;
          dashboard_ui_set_status(status.c_str());
        }
      } else if (!sd_ok) {
        dashboard_ui_set_status("fix ok - NO SD CARD");
      }
      if (!track_loaded) {
        continue;
      }
    }

    timing.OnSample(sample);

    // 25 Hz in, ~8 Hz to the screen: plenty for eyes, cheap for LVGL.
    if (++samples_since_ui >= 3) {
      samples_since_ui = 0;
      dashboard_ui_update(timing.Snapshot());
      dashboard_ui_set_next_line_distance(timing.DistanceToNextLine(sample));
      // Both map calls are skipped while the page is closed.
      if (map_ready && dashboard_ui_track_map_visible()) {
        pacer::Vec3f local = map_cs.Local(sample);
        dashboard_ui_set_track_map_position(local.x, local.y);
      }
      // Full gate scan — only worth it while that debug page is open.
      if (dashboard_ui_track_offset_visible()) {
        if (auto off = timing.OffsetFromTrack(sample)) {
          dashboard_ui_set_track_offset(off->lateral_m, off->half_width_m,
                                        off->gate,
                                        timing.Snapshot().gate_count);
        }
      }
      char status[96];
      snprintf(status, sizeof(status), "%s | %d sats | %.0f km/h%s",
               track_name.c_str(), pvt.numSV, sample.full_speed * 3.6,
               sd_ok ? "" : " | NO LOG");
      dashboard_ui_set_status(status);
    }
  }
}
