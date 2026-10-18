#include "storage.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"

#include <pacer/geometry/geometry.hpp>
#include <pacer/reference-track/reference-track.hpp>

namespace {

const char *TAG = "storage";
const char *kMountPoint = "/sdcard";

FILE *s_log = nullptr;
int s_unflushed = 0;
size_t s_appended = 0;
size_t s_flushed = 0;
// Sticky: set by the first failed write/flush (card pulled, full, I/O error).
// A short fwrite leaves a partial record that misaligns everything after it,
// so the log stops there and s_flushed freezes at the last good fsync — the
// logging page then shows the gap instead of counting phantom records.
bool s_failed = false;

void mark_failed(const char *what) {
  if (!s_failed) {
    ESP_LOGE(TAG, "log %s failed, logging stopped", what);
  }
  s_failed = true;
}

} // namespace

esp_err_t storage_mount() {
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
  mount_config.format_if_mount_failed = false;
  mount_config.max_files = 4;
  mount_config.allocation_unit_size = 16 * 1024;

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = CONFIG_PACER_SD_SPI_HOST;

  spi_bus_config_t bus_cfg = {};
  bus_cfg.mosi_io_num = CONFIG_PACER_SD_MOSI_GPIO;
  bus_cfg.miso_io_num = CONFIG_PACER_SD_MISO_GPIO;
  bus_cfg.sclk_io_num = CONFIG_PACER_SD_SCLK_GPIO;
  bus_cfg.quadwp_io_num = -1;
  bus_cfg.quadhd_io_num = -1;
  bus_cfg.max_transfer_sz = 4096;

  esp_err_t err = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg,
                                     SDSPI_DEFAULT_DMA);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
    return err;
  }

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = (gpio_num_t)CONFIG_PACER_SD_CS_GPIO;
  slot_config.host_id = (spi_host_device_t)host.slot;

  sdmmc_card_t *card = nullptr;
  err = esp_vfs_fat_sdspi_mount(kMountPoint, &host, &slot_config,
                                &mount_config, &card);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
    return err;
  }

  mkdir("/sdcard/pacer", 0775);
  mkdir("/sdcard/tracks", 0775);
  ESP_LOGI(TAG, "sd card mounted");
  return ESP_OK;
}

esp_err_t storage_log_open(std::string *path_out) {
  char path[64];
  for (int i = 0; i < 1000; ++i) {
    snprintf(path, sizeof(path), "/sdcard/pacer/SESS_%03d.dat", i);
    struct stat st;
    if (stat(path, &st) != 0) {
      break;
    }
  }
  s_log = fopen(path, "wb");
  if (!s_log) {
    ESP_LOGE(TAG, "cannot open %s", path);
    return ESP_FAIL;
  }
  if (path_out) {
    *path_out = path;
  }
  ESP_LOGI(TAG, "logging to %s", path);
  return ESP_OK;
}

void storage_log_append(int64_t timestamp_ms, const uGnssDecUbxNavPvt_t &pvt) {
  if (!s_log) {
    return;
  }
  ++s_appended;
  if (s_failed) {
    return;
  }
  if (fwrite(&timestamp_ms, sizeof(timestamp_ms), 1, s_log) != 1 ||
      fwrite(&pvt, sizeof(pvt), 1, s_log) != 1) {
    mark_failed("write");
    return;
  }
  if (++s_unflushed >= 25) {
    storage_log_flush();
  }
}

size_t storage_log_appended() { return s_appended; }

size_t storage_log_flushed() { return s_flushed; }

void storage_log_flush() {
  if (!s_log || s_failed) {
    return;
  }
  // fflush alone only drains the stdio buffer; the FAT directory entry
  // (file size) is committed by fsync. Without it a hard power-off — the
  // normal way this device shuts down — leaves a 0-byte file behind.
  if (fflush(s_log) != 0 || fsync(fileno(s_log)) != 0) {
    mark_failed("flush");
    return;
  }
  s_flushed = s_appended;
  s_unflushed = 0;
}

namespace {

void debug_append(std::string *debug_out, const std::string &entry) {
  if (!debug_out) {
    return;
  }
  if (!debug_out->empty()) {
    *debug_out += "  ";
  }
  *debug_out += entry;
}

bool has_json_ext(const std::string &name) {
  if (name.size() < 5) {
    return false;
  }
  std::string ext = name.substr(name.size() - 5);
  for (char &c : ext) {
    c = tolower(c);
  }
  return ext == ".json";
}

} // namespace

std::string storage_find_track(double lat, double lon, double *distance_m_out,
                               std::string *debug_out) {
  DIR *dir = opendir("/sdcard/tracks");
  if (!dir) {
    debug_append(debug_out, "tracks dir missing");
    return "";
  }

  pacer::GPSSample here{.lat = lat, .lon = lon, .altitude = 0};
  pacer::CoordinateSystem cs(here);

  std::string best_path;
  double best_dist = 1e18;
  int entries = 0;

  while (dirent *entry = readdir(dir)) {
    std::string name = entry->d_name;
    if (name == "." || name == "..") {
      continue;
    }
    ++entries;
    if (!has_json_ext(name)) {
      debug_append(debug_out, name + ": not *.json");
      continue;
    }
    std::string path = std::string("/sdcard/tracks/") + name;
    try {
      auto rt = pacer::ReferenceTrack::FromFile(path);
      if (rt.segments.empty()) {
        debug_append(debug_out, name + ": no segments");
        continue;
      }
      pacer::Segment start = rt.ToGlobal(rt.segments[0]);
      pacer::GPSSample start_gps{
          .lat = start.first.y, .lon = start.first.x, .altitude = 0};
      double dist = cs.Distance(here, start_gps);
      ESP_LOGI(TAG, "track %s: start line %.0f m away", name.c_str(), dist);
      char buf[32];
      snprintf(buf, sizeof(buf), ": %.0fm", dist);
      debug_append(debug_out, name + buf);
      if (dist < best_dist) {
        best_dist = dist;
        best_path = path;
      }
    } catch (const std::exception &e) {
      ESP_LOGW(TAG, "skipping %s: %s", name.c_str(), e.what());
      debug_append(debug_out, name + ": " + e.what());
    }
  }
  closedir(dir);

  if (entries == 0) {
    debug_append(debug_out, "tracks dir empty");
  }
  if (distance_m_out) {
    *distance_m_out = best_dist;
  }
  return best_path;
}

std::vector<std::string> storage_list_tracks() {
  std::vector<std::string> paths;
  DIR *dir = opendir("/sdcard/tracks");
  if (!dir) {
    return paths;
  }
  while (dirent *entry = readdir(dir)) {
    std::string name = entry->d_name;
    if (name == "." || name == ".." || !has_json_ext(name)) {
      continue;
    }
    paths.push_back(std::string("/sdcard/tracks/") + name);
  }
  closedir(dir);
  // readdir order is whatever FAT happens to hold; the picker should look
  // the same every time it's opened.
  std::sort(paths.begin(), paths.end());
  return paths;
}

namespace {

// One log record exactly as storage_log_append writes it: two fwrites, so
// no padding between the timestamp and the PVT payload.
constexpr size_t kRecordSize = sizeof(int64_t) + sizeof(uGnssDecUbxNavPvt_t);

// Shorter logs can't hold a whole lap (LiveTiming's min_lap_s is 15 s, and
// there is an out lap before it): 30 s at 25 Hz.
constexpr size_t kMinRecordingRecords = 30 * 25;

// "SESS_012.dat" -> 12; -1 for anything else in the directory.
int SessionNumber(const char *name) {
  int n = -1;
  char ext[8] = {};
  if (sscanf(name, "SESS_%d.%7s", &n, ext) != 2 || strcasecmp(ext, "dat")) {
    return -1;
  }
  return n;
}

bool ReadTimestampAt(FILE *f, long offset, int64_t *out) {
  return fseek(f, offset, SEEK_SET) == 0 && fread(out, sizeof(*out), 1, f) == 1;
}

} // namespace

std::vector<StorageRecording> storage_list_recordings(size_t max_count) {
  std::vector<std::pair<int, std::string>> sessions;
  if (DIR *dir = opendir("/sdcard/pacer")) {
    while (dirent *entry = readdir(dir)) {
      int n = SessionNumber(entry->d_name);
      if (n >= 0) {
        sessions.emplace_back(n, std::string("/sdcard/pacer/") + entry->d_name);
      }
    }
    closedir(dir);
  }
  std::sort(sessions.begin(), sessions.end(),
            [](const auto &a, const auto &b) { return a.first > b.first; });

  std::vector<StorageRecording> out;
  for (const auto &[n, path] : sessions) {
    if (out.size() >= max_count) {
      break;
    }
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
      continue;
    }
    size_t records = st.st_size / kRecordSize;
    if (records < kMinRecordingRecords) {
      continue;
    }
    StorageRecording rec{.path = path, .records = records};
    // First and last timestamps plus one fix — three short reads, not a
    // full scan. Only fix-worthy samples are logged, so any record will do.
    if (FILE *f = fopen(path.c_str(), "rb")) {
      int64_t first = 0, last = 0;
      if (ReadTimestampAt(f, 0, &first) &&
          ReadTimestampAt(f, (long)((records - 1) * kRecordSize), &last)) {
        rec.duration_s = (last - first) / 1000.0;
      }
      uGnssDecUbxNavPvt_t pvt;
      long mid = (long)((records / 2) * kRecordSize + sizeof(int64_t));
      if (fseek(f, mid, SEEK_SET) == 0 && fread(&pvt, sizeof(pvt), 1, f) == 1) {
        rec.lat = pvt.lat / 1e7;
        rec.lon = pvt.lon / 1e7;
      }
      fclose(f);
    }
    out.push_back(std::move(rec));
  }
  return out;
}

long storage_read_recording(
    const std::string &path,
    const std::function<void(int64_t, const uGnssDecUbxNavPvt_t &)>
        &on_record) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) {
    ESP_LOGE(TAG, "cannot open %s", path.c_str());
    return -1;
  }
  // Whole records at a time, many per SD read: a 15 minute session is
  // ~2 MB, and record-sized freads through newlib's small default buffer
  // turn that into tens of thousands of SPI transactions.
  constexpr size_t kChunkRecords = 64;
  std::unique_ptr<uint8_t[]> buf(new uint8_t[kChunkRecords * kRecordSize]);
  long total = 0;
  size_t got = 0;
  while ((got = fread(buf.get(), kRecordSize, kChunkRecords, f)) > 0) {
    for (size_t i = 0; i < got; ++i) {
      const uint8_t *rec = buf.get() + i * kRecordSize;
      int64_t timestamp_ms;
      uGnssDecUbxNavPvt_t pvt;
      memcpy(&timestamp_ms, rec, sizeof(timestamp_ms));
      memcpy(&pvt, rec + sizeof(timestamp_ms), sizeof(pvt));
      on_record(timestamp_ms, pvt);
    }
    total += (long)got;
  }
  fclose(f);
  return total;
}
