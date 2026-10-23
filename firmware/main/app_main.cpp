// In-kart live timing dashboard.
//
// Data flow:
//   ubx_gps reader task --(queue)--> main loop:
//     log to SD (.dat, same format the desktop tools read)
//     -> LiveTiming::OnSample
//   main loop, every kUiPeriodUs of device clock, fix or no fix:
//     LiveTiming::SnapshotAt -> dashboard_ui
//
// On the first 2D/3D fix, the nearest /sdcard/tracks/*.json annotation is
// loaded as the reference track; until then the screen shows fix status.
// The delta runs against the session best, or — picked from the debug menu —
// against the best lap of a recorded session, replayed off the card.
//
// Demo mode (debug menu -> Demo) swaps the receiver for a recording played
// back at its own pace times a speed multiplier: its fixes take the same path
// through HandlePvt as live ones, while live fixes keep going to the log only.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "sdkconfig.h"
#include "soc/rtc_cntl_reg.h"

#include <pacer/datatypes/datatypes.hpp>
#include <pacer/geometry/geometry.hpp>
#include <pacer/live-timing/live-timing.hpp>
#include <pacer/reference-track/reference-track.hpp>

#include "battery.h"
#include "dashboard_ui.hpp"
#include "demo_player.hpp"
#include "storage.hpp"
#include "ubx_gps.hpp"

namespace {

const char *TAG = "pacer";

// Repaint period for the timing fields. Driven by the device clock, so the
// session and lap clocks keep ticking between fixes and through an outage.
constexpr int64_t kUiPeriodUs = 100 * 1000;

// A fix plus the device time it came off the UART. Stamped in the reader
// task, so SD flush stalls in the consumer don't skew the device->GPS clock
// mapping the display runs on.
struct PvtMsg {
  uGnssDecUbxNavPvt_t pvt;
  int64_t received_us;
};

QueueHandle_t s_pvt_queue = nullptr;

void OnPvt(const uGnssDecUbxNavPvt_t &pvt, void *) {
  // Reader task context: hand off and get back to the UART. Dropping on a
  // full queue is fine — the consumer only stalls on SD flush hiccups.
  PvtMsg msg{pvt, esp_timer_get_time()};
  xQueueSend(s_pvt_queue, &msg, 0);
}

bool HasFix(const uGnssDecUbxNavPvt_t &pvt) {
  return pvt.fixType == U_GNSS_DEC_UBX_NAV_PVT_FIX_TYPE_2D ||
         pvt.fixType == U_GNSS_DEC_UBX_NAV_PVT_FIX_TYPE_3D ||
         pvt.fixType ==
             U_GNSS_DEC_UBX_NAV_PVT_FIX_TYPE_GNSS_PLUS_DEAD_RECKONING;
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
      .vel_n = pvt.velN / 1000.0, // mm/s to m/s
      .vel_e = pvt.velE / 1000.0,
      .vel_d = pvt.velD / 1000.0,
      .h_acc = pvt.hAcc / 1000.0, // mm to m
      .s_acc = pvt.sAcc / 1000.0,
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


// Everything the main loop keeps between iterations, and the steps it runs
// on each one. Single-threaded: only app_main's task touches it.
struct App {
  // Hardware found at startup.
  bool sd_ok = false;
  // A mounted card can still refuse the session file (full, read-only); the
  // status line then says NO LOG instead of the toggle pretending to write.
  bool log_ok = false;
  std::string log_path; // this boot's session log, when log_ok
  QueueHandle_t battery_queue = nullptr;

  // Reference track and the timing session running against it.
  pacer::LiveTiming timing;
  bool track_loaded = false;
  std::string track_name;
  std::string track_path;
  // Cleared when the picker names a track: an explicit choice is not
  // second-guessed by the nearest-track scan, even if it failed to load.
  bool auto_scan = true;
  int samples_until_scan = 1;

  // Entries offered by the debug menu's track picker, refreshed whenever we
  // touch the card for a reload.
  std::vector<DashboardTrack> track_list;
  // Start line of each track_list entry that parsed, for guessing which
  // track a recording was driven on. Refreshed alongside track_list.
  struct TrackStart {
    std::string path;
    pacer::GPSSample start;
  };
  std::vector<TrackStart> track_starts;

  // Frame for the track-map page: origin at the "median" of the track (mean
  // of the annotated gate midpoints), so the outline is centered on its own
  // geometry rather than on whichever gate the track file happens to start
  // with. Valid only while track_loaded.
  pacer::CoordinateSystem map_cs;
  bool map_ready = false;

  // Debug menu's gate extension slider. Not part of the track file, so it
  // applies to every track loaded from here on, and the installed track is
  // kept (annotated gates only, a few KB) so a change can rebuild its live
  // gates without a reload.
  double gate_extension_m = pacer::ReferenceTrack{}.gate_extension_m;
  pacer::ReferenceTrack loaded_rt;

  // Recording the installed reference lap came from (LiveTiming holds the
  // lap itself); empty while the delta runs against the session best alone.
  // Cleared along with the track: its gate times are only good for the
  // gates they were replayed on.
  std::string reference_path;

  // Debug menu's log toggle, as of the last loop iteration.
  bool logging = false;

  // Recording played in place of the receiver while active.
  DemoPlayer demo;
  // The card's recordings as of the last PushRecordingList, for guessing
  // which track a demo pick was driven on.
  std::vector<StorageRecording> recordings;

  int samples_since_ui = 0;
  int samples_since_debug = 0;

  // Maps the device clock onto the GPS one LiveTiming runs on: the last
  // sample fed to it, and the device time it arrived. The display reads the
  // clocks at anchor + device time elapsed since (times the demo speed, which
  // runs the GPS clock faster), so they tick at the UI rate rather than the
  // fix rate and don't freeze when fixes stop.
  bool have_clock_anchor = false;
  int64_t anchor_gps_ms = 0;
  int64_t anchor_device_us = 0;
  int64_t next_ui_us = 0;

  void Start();
  [[noreturn]] void Run();

  // Main loop steps, in the order Run calls them.
  void RepaintIfDue();
  void PollBattery();
  void HandleReload();
  void HandleReferencePick();
  void HandleGateExtension();
  void HandleSessionReset();
  void UpdateLogging();
  void HandleDemoPick();
  void PumpDemo();
  void HandlePvt(const PvtMsg &msg);
  void LogSample(const PvtMsg &msg);
  void UpdateDiagnostics(const uGnssDecUbxNavPvt_t &pvt);
  bool TryAutoLoadTrack(const pacer::GPSSample &sample);
  void FeedTiming(const pacer::GPSSample &sample, int64_t received_us);
  void UpdatePositionReadouts(const uGnssDecUbxNavPvt_t &pvt,
                              const pacer::GPSSample &sample);

  // Track helpers.
  std::string LoadTrack(const std::string &path);
  void UnloadTrack();
  void RescanTrackList();

  // Reference lap helpers.
  std::string LoadReferenceLap(const std::string &path);
  void ClearReferenceLap();
  void PushRecordingList();
  std::string GuessTrack(const StorageRecording &rec) const;

  // Demo helpers.
  void StartDemo(const std::string &path);
  void StopDemo();
  int ClockRate() const { return demo.active() ? demo.speed() : 1; }
};

void App::Start() {
  LogStrapDiag("boot");
  xTaskCreate(StrapDiagTask, "strap_diag", 4096, nullptr, 1, nullptr);
  ESP_ERROR_CHECK(dashboard_ui_start());
  dashboard_ui_set_status("mounting sd card...");

  sd_ok = storage_mount() == ESP_OK;
  log_ok = sd_ok && storage_log_open(&log_path) == ESP_OK;

  s_pvt_queue = xQueueCreate(64, sizeof(PvtMsg));
  ESP_ERROR_CHECK(ubx_gps_start(OnPvt, nullptr));
  dashboard_ui_set_status(sd_ok ? "waiting for gps fix..."
                                : "NO SD CARD - waiting for gps fix...");

  // Optional hardware: without a gauge the label just stays "BAT: ?".
  esp_err_t battery_err = battery_sensor_init(&battery_queue);
  if (battery_err != ESP_OK) {
    ESP_LOGW(TAG, "no battery gauge: %s", esp_err_to_name(battery_err));
  }

  dashboard_ui_set_gate_extension(gate_extension_m);
  RescanTrackList();
  dashboard_ui_set_track_list(track_list, track_path);
  PushRecordingList();
}

void App::Run() {
  while (true) {
    RepaintIfDue();

    // Sleep until the next repaint at the latest; +1 so a sub-tick remainder
    // still blocks instead of spinning. Clamped: a repaint stuck on the LVGL
    // lock past the deadline must not wrap into an hours-long wait.
    int64_t wait_ms = std::max<int64_t>(
        0, (std::min(next_ui_us, demo.NextDueUs()) - esp_timer_get_time()) /
               1000);
    TickType_t wait = pdMS_TO_TICKS(wait_ms) + 1;

    // UI-driven work comes before waiting on the GPS, so it still lands
    // while the receiver is silent or resyncing.
    PollBattery();
    HandleReload();
    HandleReferencePick();
    HandleGateExtension();
    HandleSessionReset();
    UpdateLogging();
    HandleDemoPick();
    PumpDemo();

    // The receiver is logged throughout; during a demo it just doesn't
    // drive the screen.
    PvtMsg msg;
    if (xQueueReceive(s_pvt_queue, &msg, wait) == pdTRUE) {
      LogSample(msg);
      if (!demo.active()) {
        HandlePvt(msg);
      }
    }
  }
}

void App::RepaintIfDue() {
  int64_t now_us = esp_timer_get_time();
  if (now_us < next_ui_us) {
    return;
  }
  next_ui_us = now_us + kUiPeriodUs;
  if (track_loaded) {
    dashboard_ui_update(
        have_clock_anchor
            ? timing.SnapshotAt(anchor_gps_ms + (now_us - anchor_device_us) *
                                                    ClockRate() / 1000)
            : timing.Snapshot());
  }
}

void App::PollBattery() {
  uint8_t battery_level = 0;
  if (battery_queue &&
      xQueueReceive(battery_queue, &battery_level, 0) == pdTRUE) {
    dashboard_ui_set_battery_level(battery_level);
  }
}

// Debug menu action: drop the track and session state, then either load the
// track the picker named or rescan /sdcard/tracks for the nearest one on the
// next samples.
void App::HandleReload() {
  std::string picked;
  if (!dashboard_ui_consume_track_reload(&picked)) {
    return;
  }
  UnloadTrack();
  RescanTrackList();
  samples_until_scan = 1;
  auto_scan = picked.empty();
  if (picked.empty()) {
    dashboard_ui_set_status("track reload requested...");
    ESP_LOGI(TAG, "track reload requested");
  } else {
    // A named track needs no fix and no scan — load it right here.
    ESP_LOGI(TAG, "track picked: %s", picked.c_str());
    std::string err = LoadTrack(picked);
    if (!err.empty()) {
      std::string status = "cannot load " + picked + ": " + err;
      dashboard_ui_set_status(status.c_str());
    }
  }
  dashboard_ui_set_track_list(track_list, track_path);
  // UnloadTrack dropped the reference lap with the track.
  PushRecordingList();
}

// Debug menu action: take the delta reference from a recording's best lap,
// or (empty path) go back to the session best alone. Either way the session
// starts over, like a fresh track load — the reference a lap is measured
// against shouldn't change under it.
void App::HandleReferencePick() {
  std::string picked;
  if (!dashboard_ui_consume_reference_pick(&picked)) {
    return;
  }
  if (picked.empty()) {
    ESP_LOGI(TAG, "reference lap: session best");
    ClearReferenceLap();
    dashboard_ui_set_status("reference lap: session best");
  } else if (!track_loaded) {
    dashboard_ui_set_status("reference lap: load a track first");
  } else {
    std::string err = LoadReferenceLap(picked);
    if (!err.empty()) {
      std::string status = "reference lap: " + err;
      dashboard_ui_set_status(status.c_str());
    }
  }
  PushRecordingList();
}

// Debug menu slider: rebuild the installed track's gates at the new
// extension. Laps and the delta reference survive (see UpdateGates).
void App::HandleGateExtension() {
  double new_extension_m = 0;
  if (!dashboard_ui_consume_gate_extension(&new_extension_m) ||
      new_extension_m == gate_extension_m) {
    return;
  }
  gate_extension_m = new_extension_m;
  ESP_LOGI(TAG, "gate extension %.1f m", gate_extension_m);
  if (!track_loaded) {
    return;
  }
  loaded_rt.gate_extension_m = gate_extension_m;
  try {
    timing.UpdateGates(loaded_rt);
    // A gate count change resets the session and drops the reference lap;
    // the picker should stop claiming one is in use.
    if (!reference_path.empty() && !timing.InstalledReferenceLap()) {
      reference_path.clear();
      PushRecordingList();
    }
  } catch (const std::exception &e) {
    // UpdateGates left an empty, reset session behind; say so rather than
    // show a dashboard that silently stopped timing.
    ESP_LOGE(TAG, "gate extension: %s", e.what());
    std::string status = std::string("gate extension failed: ") + e.what();
    dashboard_ui_set_status(status.c_str());
  }
}

// Debug menu action: start the session over on the loaded track — laps,
// session clock and best lap go; the track and a recorded reference lap stay.
void App::HandleSessionReset() {
  if (!dashboard_ui_consume_session_reset()) {
    return;
  }
  if (!track_loaded) {
    dashboard_ui_set_status("reset session: no track loaded");
    return;
  }
  ESP_LOGI(TAG, "session reset");
  timing.ResetSession();
  dashboard_ui_set_status("session reset");
}

// Debug menu toggle gates the log; on pause, commit what's pending so pulling
// the card right after is safe — run before the GPS wait, so the flush
// doesn't hinge on the next fix arriving.
void App::UpdateLogging() {
  bool was_logging = logging;
  logging = log_ok && dashboard_ui_logging_enabled();
  if (was_logging && !logging) {
    storage_log_flush();
  }
}

// Debug menu action: play a recording in place of the receiver, or (empty
// path) go back to live GPS.
void App::HandleDemoPick() {
  std::string picked;
  if (!dashboard_ui_consume_demo_pick(&picked)) {
    return;
  }
  if (picked.empty()) {
    StopDemo();
  } else {
    StartDemo(picked);
  }
  dashboard_ui_set_demo_recording(demo.path());
}

// Feeds every demo record that has come due, as if it had just come off the
// UART, and follows the speed button.
void App::PumpDemo() {
  if (!demo.active()) {
    return;
  }
  int64_t now_us = esp_timer_get_time();
  int speed = dashboard_ui_demo_speed();
  if (speed != demo.speed()) {
    // Re-anchor at the old rate first, so the clocks on screen don't jump.
    if (have_clock_anchor) {
      anchor_gps_ms += (now_us - anchor_device_us) * demo.speed() / 1000;
      anchor_device_us = now_us;
    }
    demo.SetSpeed(speed, now_us);
  }
  // Hands back to Run() at the repaint deadline even with records still due:
  // a speed the CPU can't keep up with would otherwise keep this loop busy
  // indefinitely, and the screen would sit frozen and then jump whole laps
  // ahead. Falling behind instead just plays slower (see kMaxLagUs).
  PvtMsg msg;
  bool restarted = false;
  int64_t due_us = 0;
  while ((now_us = esp_timer_get_time()) < next_ui_us &&
         demo.Poll(now_us, &msg.pvt, &due_us, &restarted)) {
    // Anchored at when it is handled, not when it was due: while behind,
    // the due time lags by up to kMaxLagUs, and the screen clocks would run
    // that times the speed ahead of the last sample.
    msg.received_us = now_us;
    if (restarted) {
      // Looped: the recording's clock jumps back to its start.
      ESP_LOGI(TAG, "demo: looping %s", demo.path().c_str());
      if (track_loaded) {
        timing.ResetSession();
      }
      have_clock_anchor = false;
    }
    HandlePvt(msg);
  }
  if (!demo.active()) {
    // Poll stops the player if the card stops answering mid-file.
    StopDemo();
    dashboard_ui_set_status("demo: recording unreadable");
    dashboard_ui_set_demo_recording("");
  }
}

void App::HandlePvt(const PvtMsg &msg) {
  const uGnssDecUbxNavPvt_t &pvt = msg.pvt;
  UpdateDiagnostics(pvt);

  if (!HasFix(pvt)) {
    char status[64];
    snprintf(status, sizeof(status), "no fix (%d sats)", pvt.numSV);
    dashboard_ui_set_status(status);
    return;
  }

  pacer::GPSSample sample = ToSample(pvt);
  if (!track_loaded && !TryAutoLoadTrack(sample)) {
    return;
  }
  FeedTiming(sample, msg.received_us);
  UpdatePositionReadouts(pvt, sample);
}

// Poor-quality samples are dropped rather than pausing the log: the toggle is
// the user's intent, this is just the receiver having a bad moment, and the
// session continues either way (the .dat carries its own timestamps, so a gap
// reads as a gap). Stamped with the arrival time, not now: after an SD stall
// the queue drains in a burst, and dequeue time would squash those fixes
// together.
void App::LogSample(const PvtMsg &msg) {
  if (logging && IsLogWorthy(msg.pvt)) {
    storage_log_append(msg.received_us / 1000, msg.pvt);
  }
}

// Raw receiver state in the corner, unconditionally — the point is to see
// what the GPS reports even before a fix or track. hAcc is here so an
// unexpectedly idle log counter has a visible explanation.
void App::UpdateDiagnostics(const uGnssDecUbxNavPvt_t &pvt) {
  // A sped-up demo delivers samples faster; keep the screen rate the same.
  if (++samples_since_debug < 5 * ClockRate()) {
    return;
  }
  samples_since_debug = 0;
  char dbg[96];
  snprintf(dbg, sizeof(dbg), "%.6f %.6f  %.1f km/h  %d sat  %.1f m",
           pvt.lat / 1e7, pvt.lon / 1e7, pvt.gSpeed * 0.0036, pvt.numSV,
           pvt.hAcc / 1000.0);
  dashboard_ui_set_debug(dbg);
  dashboard_ui_set_log_stats(storage_log_appended(), storage_log_flushed());
}

// No track yet: every ~second, load the nearest one on the card. Returns
// whether a track is installed afterwards.
bool App::TryAutoLoadTrack(const pacer::GPSSample &sample) {
  if (!sd_ok) {
    dashboard_ui_set_status("fix ok - NO SD CARD");
    return false;
  }
  if (!auto_scan || --samples_until_scan > 0) {
    return false;
  }
  samples_until_scan = 25; // rescan ~1/s, not per 25 Hz sample
  double dist = 0;
  std::string scan_debug;
  std::string path =
      storage_find_track(sample.lat, sample.lon, &dist, &scan_debug);
  if (!path.empty()) {
    std::string err = LoadTrack(path);
    if (err.empty()) {
      // Tick the picker on whatever the scan settled on, and undim the
      // recordings driven there.
      dashboard_ui_set_track_list(track_list, track_path);
      PushRecordingList();
      ESP_LOGI(TAG, "using track %s (%.0f m away)", track_name.c_str(), dist);
      return true;
    }
    scan_debug += "  load: " + err;
  }
  std::string status = "no track | " + scan_debug;
  dashboard_ui_set_status(status.c_str());
  return false;
}

void App::FeedTiming(const pacer::GPSSample &sample, int64_t received_us) {
  timing.OnSample(sample);
  have_clock_anchor = true;
  anchor_gps_ms = sample.timestamp_ms;
  anchor_device_us = received_us;
}

// Position-driven readouts: 25 Hz in, ~8 Hz to the screen. The timing fields
// repaint off the device clock in RepaintIfDue.
void App::UpdatePositionReadouts(const uGnssDecUbxNavPvt_t &pvt,
                                 const pacer::GPSSample &sample) {
  if (++samples_since_ui < 3 * ClockRate()) {
    return;
  }
  samples_since_ui = 0;
  // Both map calls are skipped while the page is closed.
  if (map_ready && dashboard_ui_track_map_visible()) {
    pacer::Vec3f local = map_cs.Local(sample);
    dashboard_ui_set_track_map_position(local.x, local.y);
  }
  // A recorded reference is called out, since the delta then reads against
  // a lap the driver hasn't driven today.
  char ref[24] = "";
  if (const auto &lap = timing.InstalledReferenceLap()) {
    snprintf(ref, sizeof(ref), " | REF %.3f", lap->time_s);
  }
  char demo_tag[16] = "";
  if (demo.active()) {
    snprintf(demo_tag, sizeof(demo_tag), " | DEMO %dx", demo.speed());
  }
  char status[136];
  snprintf(status, sizeof(status), "%s | %d sats | %.0f km/h%s%s%s",
           track_name.c_str(), pvt.numSV, sample.full_speed * 3.6, ref,
           demo_tag, log_ok ? "" : " | NO LOG");
  dashboard_ui_set_status(status);
}

// Installs `path` as the reference track (and its outline on the map page).
// Throws nothing: a file that won't parse just leaves the track unloaded and
// comes back as the error message; success returns "".
std::string App::LoadTrack(const std::string &path) {
  try {
    auto rt = pacer::ReferenceTrack::FromFile(path);
    rt.gate_extension_m = gate_extension_m;
    timing.SetReferenceTrack(rt);
    track_loaded = true;
    dashboard_ui_set_track_loaded(true);
    if (!rt.segments.empty()) {
      pacer::Point median{};
      for (const auto &seg : rt.segments) {
        median += (seg.first + seg.second) / 2.0;
      }
      median /= static_cast<double>(rt.segments.size());
      map_cs = pacer::CoordinateSystem(
          rt.cs.Global(pacer::Vec3f{median.x, median.y, 0}));
      // Re-express the annotated gates in the map frame; the UI draws their
      // endpoints as the two track edges.
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
    loaded_rt = std::move(rt);
    track_path = path;
    size_t slash = path.rfind('/');
    track_name = slash == std::string::npos ? path : path.substr(slash + 1);
    std::string status = "loaded track " + track_name;
    dashboard_ui_set_status(status.c_str());
    return std::string();
  } catch (const std::exception &e) {
    ESP_LOGE(TAG, "loading %s: %s", path.c_str(), e.what());
    return std::string(e.what());
  }
}

// Drops the track, its map and the timing session run against it.
void App::UnloadTrack() {
  track_loaded = false;
  dashboard_ui_set_track_loaded(false);
  track_name.clear();
  track_path.clear();
  timing = pacer::LiveTiming{};
  reference_path.clear();
  loaded_rt = pacer::ReferenceTrack{};
  map_ready = false;
  dashboard_ui_set_track_map({});
}

// Re-reads the card's track files for the picker. Each one is parsed for its
// outline; they are small and few, and the nearest-track scan parses the same
// files once a second anyway.
void App::RescanTrackList() {
  track_list.clear();
  track_starts.clear();
  if (!sd_ok) {
    return;
  }
  for (const std::string &path : storage_list_tracks()) {
    ESP_LOGI(TAG, "reading %s", path.c_str());
    DashboardTrack entry;
    entry.path = path;
    try {
      auto rt = pacer::ReferenceTrack::FromFile(path);
      entry.outline = TrackOutline(rt);
      if (!rt.segments.empty()) {
        // Same point storage_find_track measures to.
        pacer::Segment start = rt.ToGlobal(rt.segments[0]);
        track_starts.push_back(TrackStart{
            path, {.lat = start.first.y, .lon = start.first.x, .altitude = 0}});
      }
    } catch (const std::exception &e) {
      // Listed anyway, without a thumbnail: seeing the name and getting a
      // load error beats the file silently not existing.
      ESP_LOGW(TAG, "picker: %s: %s", path.c_str(), e.what());
    }
    track_list.push_back(std::move(entry));
  }
}

// Replays `path` through the installed track's timing — the live instance,
// not a second one: a second gate set is what the heap can't spare (see
// LiveTiming::SetReferenceTrack) — and installs its best lap as the
// reference. Same clock as live (iTOW, via ToSample), so the gate times
// carry straight over. On failure the previous reference is put back and
// the error comes back; success returns "".
std::string App::LoadReferenceLap(const std::string &path) {
  size_t slash = path.rfind('/');
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  std::string status = "reading " + name + "...";
  dashboard_ui_set_status(status.c_str());
  ESP_LOGI(TAG, "reference lap from %s", path.c_str());

  std::optional<pacer::ReferenceLap> previous = timing.InstalledReferenceLap();
  timing.SetReferenceLap(std::nullopt);
  int64_t started_us = esp_timer_get_time();
  size_t fed = 0;
  long records = storage_read_recording(
      path, [&](int64_t, const uGnssDecUbxNavPvt_t &pvt) {
        if (HasFix(pvt)) {
          timing.OnSample(ToSample(pvt));
        }
        // Let the idle task in: a long session is seconds of this, and the
        // task watchdog watches idle.
        if (++fed % 2048 == 0) {
          vTaskDelay(1);
        }
      });
  std::optional<pacer::ReferenceLap> lap = timing.DeltaReference();
  ESP_LOGI(TAG, "replayed %ld records in %lld ms", records,
           (esp_timer_get_time() - started_us) / 1000);

  if (records < 0 || !lap) {
    // No lap is not a bug: the recording is likely from another track.
    timing.SetReferenceLap(std::move(previous));
    return name + (records < 0 ? ": cannot read" : ": no full lap on this track");
  }
  ESP_LOGI(TAG, "reference lap %.3f s", lap->time_s);
  status = "reference lap: " + name + " " + std::to_string(records) +
           " records, best ";
  char lap_s[16];
  snprintf(lap_s, sizeof(lap_s), "%.3f", lap->time_s);
  status += lap_s;
  dashboard_ui_set_status(status.c_str());
  timing.SetReferenceLap(std::move(lap));
  reference_path = path;
  return std::string();
}

void App::ClearReferenceLap() {
  timing.SetReferenceLap(std::nullopt);
  reference_path.clear();
}

// Re-lists the card's recordings for the picker, marking the one in use.
// Cheap enough to redo after every pick: a stat and two short reads each.
void App::PushRecordingList() {
  std::vector<DashboardRecording> list;
  recordings.clear();
  if (sd_ok) {
    recordings = storage_list_recordings();
    for (const StorageRecording &rec : recordings) {
      std::string track = GuessTrack(rec);
      list.push_back(DashboardRecording{
          .path = rec.path,
          .duration_s = rec.duration_s,
          .live = log_ok && rec.path == log_path,
          .track_path = track,
          .on_loaded_track = track_loaded && track == track_path,
      });
    }
  }
  const auto &lap = timing.InstalledReferenceLap();
  dashboard_ui_set_recording_list(list, reference_path,
                                  lap ? lap->time_s : 0);
}

// The track whose start line is nearest the recording's mid-session fix, or
// "" if none is within kMaxTrackGuessM. A kart circuit fits well inside that
// radius; tracks further apart than it never get confused.
std::string App::GuessTrack(const StorageRecording &rec) const {
  constexpr double kMaxTrackGuessM = 3000;
  if (std::isnan(rec.lat) || std::isnan(rec.lon)) {
    return std::string();
  }
  pacer::GPSSample here{.lat = rec.lat, .lon = rec.lon, .altitude = 0};
  pacer::CoordinateSystem cs(here);
  const std::string *best = nullptr;
  double best_dist = kMaxTrackGuessM;
  for (const TrackStart &t : track_starts) {
    double dist = cs.Distance(here, t.start);
    if (dist < best_dist) {
      best_dist = dist;
      best = &t.path;
    }
  }
  return best ? *best : std::string();
}

// Starts `path` playing from the top, on the track it was driven on: kept if
// that's the one loaded (so is the reference lap), loaded if the card has it,
// and otherwise left to the nearest-track scan over the demo's own fixes.
// Either way the session starts over.
void App::StartDemo(const std::string &path) {
  if (!demo.Start(path, dashboard_ui_demo_speed(), esp_timer_get_time())) {
    // Start dropped whatever was playing; wind that down like a stop.
    StopDemo();
    dashboard_ui_set_status("demo: cannot read recording");
    return;
  }
  ESP_LOGI(TAG, "demo: playing %s at %dx", path.c_str(), demo.speed());
  have_clock_anchor = false;

  std::string track;
  for (const StorageRecording &rec : recordings) {
    if (rec.path == path) {
      track = GuessTrack(rec);
      break;
    }
  }
  if (track_loaded && track == track_path) {
    timing.ResetSession();
    return;
  }
  UnloadTrack();
  auto_scan = track.empty();
  samples_until_scan = 1;
  if (!track.empty()) {
    std::string err = LoadTrack(track);
    if (!err.empty()) {
      std::string status = "demo: cannot load " + track + ": " + err;
      dashboard_ui_set_status(status.c_str());
    }
  }
  dashboard_ui_set_track_list(track_list, track_path);
  PushRecordingList();
}

// Back to the receiver. The track stays; the session starts over, since its
// laps were the recording's.
void App::StopDemo() {
  demo.Stop();
  if (track_loaded) {
    timing.ResetSession();
  }
  have_clock_anchor = false;
  dashboard_ui_set_status("demo stopped");
}

} // namespace

extern "C" void app_main(void) {
  // Lives on app_main's stack (CONFIG_ESP_MAIN_TASK_STACK_SIZE), as the
  // locals it replaces did; Run never returns.
  App app;
  app.Start();
  app.Run();
}
