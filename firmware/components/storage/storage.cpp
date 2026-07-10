#include "storage.hpp"

#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"

#include <nlohmann/json.hpp>

#include <pacer/geometry/geometry.hpp>
#include <pacer/reference-track/reference-track.hpp>

namespace {

const char *TAG = "storage";
const char *kMountPoint = "/sdcard";

FILE *s_log = nullptr;
int s_unflushed = 0;
size_t s_appended = 0;
size_t s_flushed = 0;

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

double storage_session_minutes(double fallback_minutes) {
  std::ifstream file("/sdcard/pacer/config.json");
  if (!file.is_open()) {
    return fallback_minutes;
  }
  try {
    nlohmann::json json;
    file >> json;
    for (const char *key : {"session_minutes", "minutes"}) {
      if (json.contains(key) && json[key].is_number()) {
        return json[key].get<double>();
      }
    }
  } catch (const std::exception &e) {
    ESP_LOGW(TAG, "config.json: %s", e.what());
  }
  return fallback_minutes;
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
  fwrite(&timestamp_ms, sizeof(timestamp_ms), 1, s_log);
  fwrite(&pvt, sizeof(pvt), 1, s_log);
  ++s_appended;
  if (++s_unflushed >= 25) {
    storage_log_flush();
  }
}

size_t storage_log_appended() { return s_appended; }

size_t storage_log_flushed() { return s_flushed; }

void storage_log_flush() {
  if (!s_log) {
    return;
  }
  // fflush alone only drains the stdio buffer; the FAT directory entry
  // (file size) is committed by fsync. Without it a hard power-off — the
  // normal way this device shuts down — leaves a 0-byte file behind.
  fflush(s_log);
  fsync(fileno(s_log));
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
