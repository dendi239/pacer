#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <curl/curl.h>
#include <filesystem>
#include <glad/glad.h>
#include <hello_imgui/hello_imgui.h>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include "../3rdparty/hello_imgui/external/stb_hello_imgui/stb_image.h"

struct TrackPoint {
  float lat = 0.0f;
  float lon = 0.0f;
};

struct TrackSegment {
  TrackPoint a;         // inner edge
  TrackPoint b;         // outer edge
  bool complete = true; // if false, b is not set yet
};

struct MapTileImage;

struct TrackState {
  std::vector<TrackSegment> segments;
  int selected_segment = -1;
  int selected_end = 0; // 0 = a, 1 = b
  bool dragging_point = false;
  ImVec2 view_offset = {0.0f, 0.0f};
  float view_zoom = 1.0f;
  std::string state_filename = "track_annotation.json";
  std::string last_message;

  int map_tile_zoom = 17;
  float map_tile_lat = 51.37600f;
  float map_tile_lon = -0.36100f;
  int map_tile_x = 0;
  int map_tile_y = 0;
  std::string map_tile_status = "Idle";
  bool map_tile_requested = false;
  bool auto_load_tiles = true;
  bool show_infill = true;
  bool panning_map = false;
  std::map<std::tuple<int, int, int>, struct MapTileImage> tile_cache;
};

struct MapTileImage {
  GLuint texture = 0;
  int width = 0;
  int height = 0;
  bool valid = false;
  std::string url;
  std::string status = "Pending";
};

static size_t CurlWriteMemoryCallback(void *contents, size_t size, size_t nmemb,
                                      void *userp) {
  size_t total_size = size * nmemb;
  auto *buffer = static_cast<std::vector<unsigned char> *>(userp);
  auto *data = static_cast<unsigned char *>(contents);
  buffer->insert(buffer->end(), data, data + total_size);
  return total_size;
}

static std::pair<double, double> LatLonToTileXY(double latitude,
                                                double longitude, int zoom) {
  latitude = std::clamp(latitude, -85.05112878, 85.05112878);
  double lat_rad = latitude * M_PI / 180.0;
  double n = static_cast<double>(1 << zoom);
  double x = (longitude + 180.0) / 360.0 * n;
  double y =
      (1.0 - std::log(std::tan(lat_rad) + 1.0 / std::cos(lat_rad)) / M_PI) /
      2.0 * n;
  return {x, y};
}

static std::string BuildSatelliteTileUrl(double latitude, double longitude,
                                         int zoom, int &out_x, int &out_y) {
  auto [x, y] = LatLonToTileXY(latitude, longitude, zoom);
  out_x = static_cast<int>(std::floor(x));
  out_y = static_cast<int>(std::floor(y));
  int n = 1 << zoom;
  out_x = std::clamp(out_x, 0, n - 1);
  out_y = std::clamp(out_y, 0, n - 1);
  char url_buffer[512];
  std::snprintf(url_buffer, sizeof(url_buffer),
                "https://server.arcgisonline.com/ArcGIS/rest/services/"
                "World_Imagery/MapServer/tile/%d/%d/%d",
                zoom, out_y, out_x);
  return std::string(url_buffer);
}

static std::string BuildSatelliteTileUrl(int zoom, int x, int y) {
  char url_buffer[512];
  std::snprintf(url_buffer, sizeof(url_buffer),
                "https://server.arcgisonline.com/ArcGIS/rest/services/"
                "World_Imagery/MapServer/tile/%d/%d/%d",
                zoom, y, x);
  return std::string(url_buffer);
}

static std::pair<double, double> TileXYToLatLon(int zoom, double x, double y) {
  double n = static_cast<double>(1 << zoom);
  double lon = x / n * 360.0 - 180.0;
  double lat_rad = std::atan(std::sinh(M_PI * (1.0 - 2.0 * y / n)));
  double lat = lat_rad * 180.0 / M_PI;
  return {lat, lon};
}

static bool DownloadImageToMemory(const std::string &url,
                                  std::vector<unsigned char> &image_data,
                                  std::string &error) {
  CURL *curl = curl_easy_init();
  if (!curl) {
    error = "Failed to initialize curl";
    return false;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteMemoryCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &image_data);
  curl_easy_setopt(curl, CURLOPT_USERAGENT,
                   "track_annotator/1.0 (+https://github.com/)");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    error = curl_easy_strerror(res);
    curl_easy_cleanup(curl);
    return false;
  }
  long response_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
  curl_easy_cleanup(curl);

  if (response_code != 200) {
    error = "Download failed: HTTP " + std::to_string(response_code);
    return false;
  }

  if (image_data.empty()) {
    error = "Downloaded image data is empty";
    return false;
  }

  return true;
}

static bool UpdateMapTileTexture(const std::vector<unsigned char> &image_data,
                                 const std::string &url, MapTileImage &tile,
                                 std::string &error) {
  int width = 0, height = 0, channels = 0;
  unsigned char *pixels = stbi_load_from_memory(
      image_data.data(), static_cast<int>(image_data.size()), &width, &height,
      &channels, 4);
  if (!pixels) {
    error = stbi_failure_reason();
    return false;
  }

  if (tile.texture) {
    glDeleteTextures(1, &tile.texture);
    tile.texture = 0;
  }

  glGenTextures(1, &tile.texture);
  glBindTexture(GL_TEXTURE_2D, tile.texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, pixels);
  glBindTexture(GL_TEXTURE_2D, 0);

  stbi_image_free(pixels);

  tile.width = width;
  tile.height = height;
  tile.valid = true;
  tile.url = url;
  tile.status = "Loaded";
  return true;
}

static bool LoadSatelliteTile(TrackState &state, int zoom, int x, int y,
                              std::string &error) {
  if (zoom < 0 || zoom > 19) {
    error = "Zoom out of range";
    return false;
  }
  int n = 1 << zoom;
  if (x < 0 || x >= n || y < 0 || y >= n) {
    error = "Tile coordinates out of range";
    return false;
  }

  auto key = std::make_tuple(zoom, x, y);
  auto &tile = state.tile_cache[key];
  if (tile.valid)
    return true;
  if (tile.status == "Loading") {
    error = "Already loading";
    return false;
  }

  tile.status = "Loading";
  std::vector<unsigned char> image_data;
  std::string url = BuildSatelliteTileUrl(zoom, x, y);
  bool ok = DownloadImageToMemory(url, image_data, error);
  if (!ok) {
    tile.status = "Error: " + error;
    return false;
  }

  ok = UpdateMapTileTexture(image_data, url, tile, error);
  if (!ok) {
    tile.status = "Error: " + error;
    return false;
  }

  return true;
}

static void EnsureVisibleTilesLoaded(TrackState &state,
                                     const ImVec2 &canvas_min,
                                     const ImVec2 &canvas_size) {
  if (!state.auto_load_tiles)
    return;

  int zoom = state.map_tile_zoom;
  auto [tile_xf, tile_yf] =
      LatLonToTileXY(state.map_tile_lat, state.map_tile_lon, zoom);
  int center_tx = static_cast<int>(std::floor(tile_xf));
  int center_ty = static_cast<int>(std::floor(tile_yf));
  float tile_screen_size = 256.0f * state.view_zoom;
  int extra_x =
      static_cast<int>(std::ceil((canvas_size.x * 0.5f) / tile_screen_size)) +
      1;
  int extra_y =
      static_cast<int>(std::ceil((canvas_size.y * 0.5f) / tile_screen_size)) +
      1;
  int min_tx = std::max(0, center_tx - extra_x);
  int max_tx = std::min((1 << zoom) - 1, center_tx + extra_x);
  int min_ty = std::max(0, center_ty - extra_y);
  int max_ty = std::min((1 << zoom) - 1, center_ty + extra_y);

  int requests = 0;
  for (int ty = min_ty; ty <= max_ty; ++ty) {
    for (int tx = min_tx; tx <= max_tx; ++tx) {
      auto key = std::make_tuple(zoom, tx, ty);
      auto it = state.tile_cache.find(key);
      if (it == state.tile_cache.end() ||
          (!it->second.valid && it->second.status != "Loading")) {
        if (requests >= 1)
          return;
        std::string error;
        if (LoadSatelliteTile(state, zoom, tx, ty, error)) {
          state.map_tile_status =
              "Loaded tile " + std::to_string(tx) + "/" + std::to_string(ty);
        } else {
          state.map_tile_status = "Error: " + error;
        }
        ++requests;
      }
    }
  }
}

static ImVec2 WorldToScreen(const TrackPoint &point, const ImVec2 &canvas_min,
                            const ImVec2 &canvas_max, const TrackState &state) {
  int zoom = state.map_tile_zoom;
  auto [center_x, center_y] =
      LatLonToTileXY(state.map_tile_lat, state.map_tile_lon, zoom);
  auto [point_x, point_y] = LatLonToTileXY(point.lat, point.lon, zoom);
  float tile_screen_size = 256.0f * state.view_zoom;
  ImVec2 center =
      ImVec2((canvas_min.x + canvas_max.x) * 0.5f + state.view_offset.x,
             (canvas_min.y + canvas_max.y) * 0.5f + state.view_offset.y);
  return ImVec2(center.x + (point_x - center_x) * tile_screen_size,
                center.y + (point_y - center_y) * tile_screen_size);
}

static TrackPoint ScreenToWorld(const ImVec2 &screen, const ImVec2 &canvas_min,
                                const ImVec2 &canvas_max,
                                const TrackState &state) {
  int zoom = state.map_tile_zoom;
  auto [center_x, center_y] =
      LatLonToTileXY(state.map_tile_lat, state.map_tile_lon, zoom);
  float tile_screen_size = 256.0f * state.view_zoom;
  ImVec2 center =
      ImVec2((canvas_min.x + canvas_max.x) * 0.5f + state.view_offset.x,
             (canvas_min.y + canvas_max.y) * 0.5f + state.view_offset.y);
  ImVec2 delta = ImVec2(screen.x - center.x, screen.y - center.y);
  double point_x = center_x + delta.x / tile_screen_size;
  double point_y = center_y + delta.y / tile_screen_size;
  auto [lat, lon] = TileXYToLatLon(zoom, point_x, point_y);
  return TrackPoint{static_cast<float>(lat), static_cast<float>(lon)};
}

static bool PointHit(const ImVec2 &screen_pos, const ImVec2 &point_screen,
                     float radius) {
  float dx = screen_pos.x - point_screen.x;
  float dy = screen_pos.y - point_screen.y;
  return dx * dx + dy * dy <= radius * radius;
}

static void EnsureSampleTrack(TrackState &state) {
  if (!state.segments.empty())
    return;

  // Create sample segments by pairing nearby sample coordinates.
  std::vector<TrackPoint> sample = {
      {51.37655f, -0.36220f}, {51.37640f, -0.36190f}, {51.37630f, -0.36150f},
      {51.37620f, -0.36110f}, {51.37610f, -0.36070f}, {51.37595f, -0.36040f},
      {51.37590f, -0.36080f}, {51.37580f, -0.36130f}, {51.37600f, -0.36160f},
      {51.37620f, -0.36180f}, {51.37640f, -0.36170f}, {51.37655f, -0.36140f},
  };
  for (size_t i = 0; i + 1 < sample.size(); i += 2) {
    TrackSegment s;
    s.a = sample[i];
    s.b = sample[i + 1];
    s.complete = true;
    state.segments.push_back(s);
  }
}

static nlohmann::json ToJson(const TrackState &state) {
  nlohmann::json json;
  json["segments"] = nlohmann::json::array();
  for (const auto &seg : state.segments) {
    // Only store completed segments, without the boolean flag
    if (seg.complete) {
      json["segments"].push_back(
          {{seg.a.lat, seg.a.lon}, {seg.b.lat, seg.b.lon}});
    }
  }
  return json;
}

static bool SaveState(const TrackState &state, const std::string &filename) {
  nlohmann::json json = ToJson(state);
  std::ofstream file(filename);
  if (!file.is_open())
    return false;
  file << json.dump(2);
  return true;
}

static bool LoadState(TrackState &state, const std::string &filename) {
  std::ifstream file(filename);
  if (!file.is_open())
    return false;
  nlohmann::json json;
  file >> json;
  if (!json.contains("segments") || !json["segments"].is_array())
    return false;

  state.segments.clear();
  for (const auto &entry : json["segments"]) {
    if (entry.is_array() && entry.size() >= 2) {
      TrackSegment s;
      if (entry[0].is_array() && entry[0].size() >= 2) {
        s.a.lat = entry[0][0].get<float>();
        s.a.lon = entry[0][1].get<float>();
      }
      if (entry[1].is_array() && entry[1].size() >= 2) {
        s.b.lat = entry[1][0].get<float>();
        s.b.lon = entry[1][1].get<float>();
      }
      // All stored segments are complete; no boolean flag to read
      s.complete = true;
      state.segments.push_back(s);
    }
  }

  // Do not restore view_offset or view_zoom from file. Instead center the map
  // on the geometric center of the loaded segments and reset pan offset.
  state.view_offset = ImVec2(0.0f, 0.0f);
  // compute center of all segment endpoints
  double sum_lat = 0.0;
  double sum_lon = 0.0;
  int count = 0;
  for (const auto &s : state.segments) {
    sum_lat += s.a.lat;
    sum_lon += s.a.lon;
    ++count;
    sum_lat += s.b.lat;
    sum_lon += s.b.lon;
    ++count;
  }
  if (count > 0) {
    state.map_tile_lat = static_cast<float>(sum_lat / count);
    state.map_tile_lon = static_cast<float>(sum_lon / count);
  }

  state.selected_segment = -1;
  state.dragging_point = false;

  return true;
}

static void ShowAnnotatorGui(TrackState &state) {
  EnsureSampleTrack(state);

  ImGui::Begin("Track Annotator");

  // Two-column layout: left = controls (~30%), right = map (~70%)
  float avail_w = ImGui::GetContentRegionAvail().x;
  float left_w = avail_w * 0.30f;
  ImGui::Columns(2, "main_columns", false);
  ImGui::SetColumnWidth(0, left_w);

  // Left panel is a child window (scrollable) so it doesn't scroll the map.
  ImGui::BeginChild("left_panel", ImVec2(left_w, 0), true,
                    ImGuiWindowFlags_None);
  ImGui::Text(
      "Open-source satellite imagery background downloaded with libcurl");
  ImGui::Text(
      "Left click to pan the map or drag a point, right click to add a point.");
  ImGui::Text("Scroll wheel zooms the map.");
  ImGui::Spacing();

  // --- Left column: controls and lists ---
  ImGui::PushItemWidth(140.0f);
  ImGui::InputInt("Tile Zoom", &state.map_tile_zoom);
  ImGui::SameLine();
  ImGui::InputFloat("Latitude", &state.map_tile_lat, 0.1f, 1.0f, "%.5f");
  ImGui::SameLine();
  ImGui::InputFloat("Longitude", &state.map_tile_lon, 0.1f, 1.0f, "%.5f");
  ImGui::PopItemWidth();

  state.map_tile_zoom = std::clamp(state.map_tile_zoom, 0, 19);
  state.map_tile_lat =
      std::clamp(state.map_tile_lat, -85.05112878f, 85.05112878f);
  if (state.map_tile_lon < -180.0f)
    state.map_tile_lon += 360.0f;
  if (state.map_tile_lon > 180.0f)
    state.map_tile_lon -= 360.0f;

  ImGui::Checkbox("Auto-load visible tiles", &state.auto_load_tiles);
  ImGui::Checkbox("Show infill", &state.show_infill);
  ImGui::SameLine();
  bool should_fetch_tile = false;
  if (!state.map_tile_requested) {
    should_fetch_tile = true;
    state.map_tile_requested = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Fetch Satellite Tile")) {
    should_fetch_tile = true;
  }

  if (should_fetch_tile) {
    state.map_tile_status = "Fetching...";
    std::vector<unsigned char> image_data;
    std::string error;
    std::string url = BuildSatelliteTileUrl(
        state.map_tile_lat, state.map_tile_lon, state.map_tile_zoom,
        state.map_tile_x, state.map_tile_y);
    auto key = std::make_tuple(state.map_tile_zoom, state.map_tile_x,
                               state.map_tile_y);
    auto &tile = state.tile_cache[key];
    bool ok = DownloadImageToMemory(url, image_data, error) &&
              UpdateMapTileTexture(image_data, url, tile, error);
    if (ok) {
      state.map_tile_status = "Loaded " + std::to_string(tile.width) + "x" +
                              std::to_string(tile.height);
      HelloImGui::Log(HelloImGui::LogLevel::Info, "Loaded satellite tile %s",
                      url.c_str());
    } else {
      tile.status = "Error: " + error;
      state.map_tile_status = "Error: " + error;
      HelloImGui::Log(HelloImGui::LogLevel::Error, "Satellite tile failed: %s",
                      error.c_str());
    }
  }
  ImGui::SameLine();
  ImGui::Text("%s", state.map_tile_status.c_str());

  if (ImGui::Button("Reset View")) {
    state.view_offset = {0.0f, 0.0f};
    state.view_zoom = 1.0f;
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset Track")) {
    state.segments.clear();
    state.selected_segment = -1;
    state.selected_end = 0;
    EnsureSampleTrack(state);
  }

  ImGui::Separator();

  // Selected segment editor, save/load and segment table live in left column.
  ImGui::Spacing();
  ImGui::Text("Selected segment: %d / %zu", state.selected_segment,
              state.segments.size());
  if (state.selected_segment >= 0 &&
      state.selected_segment < (int)state.segments.size()) {
    auto &seg = state.segments[state.selected_segment];
    ImGui::Text("Point A");
    ImGui::SameLine();
    ImGui::DragFloat("A Latitude", &seg.a.lat, 0.00005f, -85.0f, 85.0f);
    ImGui::SameLine();
    ImGui::DragFloat("A Longitude", &seg.a.lon, 0.00005f, -180.0f, 180.0f);
    ImGui::Text("Point B");
    ImGui::SameLine();
    ImGui::DragFloat("B Latitude", &seg.b.lat, 0.00005f, -85.0f, 85.0f);
    ImGui::SameLine();
    ImGui::DragFloat("B Longitude", &seg.b.lon, 0.00005f, -180.0f, 180.0f);
    if (ImGui::Button("Delete segment") && state.selected_segment >= 0) {
      auto it = state.segments.begin() + state.selected_segment;
      state.segments.erase(it);
      state.selected_segment = -1;
    }
  }

  ImGui::Spacing();
  ImGui::InputText("State file##statefile", &state.state_filename);
  if (ImGui::Button("Save state##save")) {
    if (!SaveState(state, state.state_filename)) {
      state.last_message =
          "Unable to write state to '" + state.state_filename + "'";
      HelloImGui::Log(HelloImGui::LogLevel::Error, "%s",
                      state.last_message.c_str());
    } else {
      state.last_message = "Saved state to '" + state.state_filename + "'";
      HelloImGui::Log(HelloImGui::LogLevel::Info, "%s",
                      state.last_message.c_str());
    }
  }
  if (ImGui::Button("Load state##load")) {
    if (!LoadState(state, state.state_filename)) {
      state.last_message =
          "Unable to load state from '" + state.state_filename + "'";
      HelloImGui::Log(HelloImGui::LogLevel::Error, "%s",
                      state.last_message.c_str());
      // Try to provide more context: attempt to stat the file from cwd
      std::ifstream f(state.state_filename);
      if (!f.is_open()) {
        try {
          std::filesystem::path p =
              std::filesystem::current_path() / state.state_filename;
          state.last_message += " (tried: " + p.string() + ")";
        } catch (...) {
        }
      }
    } else {
      state.last_message = "Loaded state from '" + state.state_filename + "'";
      HelloImGui::Log(HelloImGui::LogLevel::Info, "%s",
                      state.last_message.c_str());
    }
  }

  if (ImGui::BeginTable("points_table", 2, ImGuiTableFlags_Borders)) {
    ImGui::TableSetupColumn("Index");
    ImGui::TableSetupColumn("Point");
    ImGui::TableHeadersRow();
    for (int i = 0; i < (int)state.segments.size(); ++i) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      if (ImGui::Selectable(std::to_string(i).c_str(),
                            state.selected_segment == i,
                            ImGuiSelectableFlags_SpanAllColumns)) {
        state.selected_segment = i;
      }
      ImGui::TableSetColumnIndex(1);
      ImGui::Text("A: %.5f, %.5f | B: %.5f, %.5f", state.segments[i].a.lat,
                  state.segments[i].a.lon, state.segments[i].b.lat,
                  state.segments[i].b.lon);
    }
    ImGui::EndTable();
  }
  if (!state.last_message.empty()) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.5f, 1.0f), "%s",
                       state.last_message.c_str());
  }

  // End left child and move to right column for map
  ImGui::EndChild();
  ImGui::NextColumn();

  ImVec2 avail = ImGui::GetContentRegionAvail();
  float canvas_h = std::max(200.0f, avail.y);
  ImVec2 canvas_size = ImVec2(avail.x, canvas_h);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(12, 12, 18, 255));
  ImGui::BeginChild("map_canvas", canvas_size, true,
                    ImGuiWindowFlags_NoScrollbar);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f);

  ImVec2 canvas_min = ImGui::GetCursorScreenPos();
  ImVec2 canvas_max =
      ImVec2(canvas_min.x + canvas_size.x, canvas_min.y + canvas_size.y);
  ImDrawList *draw_list = ImGui::GetWindowDrawList();

  ImGui::Dummy(canvas_size);

  auto RenderVisibleTiles = [&](const TrackState &state) {
    int zoom = state.map_tile_zoom;
    auto [tile_xf, tile_yf] =
        LatLonToTileXY(state.map_tile_lat, state.map_tile_lon, zoom);
    int center_tx = static_cast<int>(std::floor(tile_xf));
    int center_ty = static_cast<int>(std::floor(tile_yf));
    double frac_x = tile_xf - center_tx;
    double frac_y = tile_yf - center_ty;
    float tile_screen_size = 256.0f * state.view_zoom;
    ImVec2 center_screen =
        ImVec2(canvas_min.x + canvas_size.x * 0.5f + state.view_offset.x,
               canvas_min.y + canvas_size.y * 0.5f + state.view_offset.y);
    ImVec2 origin = ImVec2(center_screen.x - frac_x * tile_screen_size,
                           center_screen.y - frac_y * tile_screen_size);
    int extra_x =
        static_cast<int>(std::ceil((canvas_size.x * 0.5f) / tile_screen_size)) +
        2;
    int extra_y =
        static_cast<int>(std::ceil((canvas_size.y * 0.5f) / tile_screen_size)) +
        2;
    int min_tx = std::max(0, center_tx - extra_x);
    int max_tx = std::min((1 << zoom) - 1, center_tx + extra_x);
    int min_ty = std::max(0, center_ty - extra_y);
    int max_ty = std::min((1 << zoom) - 1, center_ty + extra_y);

    for (int ty = min_ty; ty <= max_ty; ++ty) {
      for (int tx = min_tx; tx <= max_tx; ++tx) {
        ImVec2 tile_min =
            ImVec2(origin.x + (tx - center_tx) * tile_screen_size,
                   origin.y + (ty - center_ty) * tile_screen_size);
        ImVec2 tile_max = ImVec2(tile_min.x + tile_screen_size,
                                 tile_min.y + tile_screen_size);
        auto key = std::make_tuple(zoom, tx, ty);
        auto it = state.tile_cache.find(key);

        if (it != state.tile_cache.end() && it->second.valid) {
          draw_list->AddImage((ImTextureID)(intptr_t)it->second.texture,
                              tile_min, tile_max, ImVec2(0.0f, 0.0f),
                              ImVec2(1.0f, 1.0f));
        } else {
          draw_list->AddRectFilled(tile_min, tile_max,
                                   IM_COL32(18, 22, 35, 255));
          draw_list->AddRect(tile_min, tile_max, IM_COL32(80, 92, 120, 150),
                             0.0f, 0, 1.0f);
          if (it != state.tile_cache.end()) {
            draw_list->AddText(ImVec2(tile_min.x + 4.0f, tile_min.y + 4.0f),
                               IM_COL32(200, 200, 220, 180),
                               it->second.status.c_str());
          }
        }
      }
    }
  };

  EnsureVisibleTilesLoaded(state, canvas_min, canvas_size);
  RenderVisibleTiles(state);
  ImU32 track_color = IM_COL32(180, 220, 255, 220);
  ImU32 selected_color = IM_COL32(255, 120, 120, 220);

  ImVec2 mouse_pos = ImGui::GetIO().MousePos;
  bool hovered = ImGui::IsItemHovered();
  bool left_clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  bool right_clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
  bool left_drag = hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left);
  float wheel = hovered ? ImGui::GetIO().MouseWheel : 0.0f;

  if (wheel != 0.0f) {
    float old_zoom = state.view_zoom;
    state.view_zoom =
        std::clamp(state.view_zoom * (wheel > 0 ? 1.1f : 0.9f), 0.4f, 4.0f);
    TrackPoint world_at_cursor =
        ScreenToWorld(mouse_pos, canvas_min, canvas_max, state);
    if (old_zoom != state.view_zoom) {
      ImVec2 after_zoom =
          WorldToScreen(world_at_cursor, canvas_min, canvas_max, state);
      state.view_offset.x += mouse_pos.x - after_zoom.x;
      state.view_offset.y += mouse_pos.y - after_zoom.y;
    }
  }

  if (left_drag && state.panning_map) {
    ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
    state.view_offset.x += delta.x;
    state.view_offset.y += delta.y;
    ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
  }

  if (right_clicked) {
    TrackPoint world = ScreenToWorld(mouse_pos, canvas_min, canvas_max, state);
    if (state.segments.empty() || state.segments.back().complete) {
      TrackSegment s;
      s.a = world;
      s.b = world;
      s.complete = false;
      state.segments.push_back(s);
      state.selected_segment = static_cast<int>(state.segments.size() - 1);
      state.selected_end = 0;
      state.dragging_point = true;
    } else {
      // complete the last segment's b point
      int idx = static_cast<int>(state.segments.size() - 1);
      state.segments[idx].b = world;
      state.segments[idx].complete = true;
      state.selected_segment = idx;
      state.selected_end = 1;
      state.dragging_point = true;
    }
  }

  int hit_segment = -1;
  int hit_end = 0;
  float hit_radius = 10.0f;
  for (int i = 0; i < (int)state.segments.size(); ++i) {
    ImVec2 sa =
        WorldToScreen(state.segments[i].a, canvas_min, canvas_max, state);
    ImVec2 sb =
        WorldToScreen(state.segments[i].b, canvas_min, canvas_max, state);
    if (PointHit(mouse_pos, sa, hit_radius)) {
      hit_segment = i;
      hit_end = 0;
      break;
    }
    if (PointHit(mouse_pos, sb, hit_radius)) {
      hit_segment = i;
      hit_end = 1;
      break;
    }
  }

  if (left_clicked) {
    if (hit_segment != -1) {
      state.selected_segment = hit_segment;
      state.selected_end = hit_end;
      state.dragging_point = true;
      state.panning_map = false;
    } else {
      state.selected_segment = -1;
      state.dragging_point = false;
      state.panning_map = true;
    }
  }
  if (state.dragging_point && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
      state.selected_segment >= 0 &&
      state.selected_segment < (int)state.segments.size()) {
    TrackPoint pt = ScreenToWorld(mouse_pos, canvas_min, canvas_max, state);
    if (state.selected_end == 0)
      state.segments[state.selected_segment].a = pt;
    else
      state.segments[state.selected_segment].b = pt;
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    state.dragging_point = false;
    state.panning_map = false;
  }

  // Draw quads between consecutive segments (a_i, b_i, b_{i+1}, a_{i+1})
  if (state.segments.size() >= 2) {
    for (int i = 0; i + 1 < (int)state.segments.size(); ++i) {
      ImVec2 a0 =
          WorldToScreen(state.segments[i].a, canvas_min, canvas_max, state);
      ImVec2 b0 =
          WorldToScreen(state.segments[i].b, canvas_min, canvas_max, state);
      ImVec2 a1 =
          WorldToScreen(state.segments[i + 1].a, canvas_min, canvas_max, state);
      ImVec2 b1 =
          WorldToScreen(state.segments[i + 1].b, canvas_min, canvas_max, state);
      ImVec2 poly[4] = {a0, b0, b1, a1};
      if (state.show_infill) {
        draw_list->AddConvexPolyFilled(poly, 4, track_color);
      }
      draw_list->AddPolyline(poly, 4, IM_COL32(40, 60, 80, 200), true, 2.0f);
    }
  }

  // Draw segment endpoints
  for (int i = 0; i < (int)state.segments.size(); ++i) {
    ImVec2 sa =
        WorldToScreen(state.segments[i].a, canvas_min, canvas_max, state);
    ImVec2 sb =
        WorldToScreen(state.segments[i].b, canvas_min, canvas_max, state);
    ImU32 colorA = (state.selected_segment == i && state.selected_end == 0)
                       ? selected_color
                       : IM_COL32(220, 240, 255, 220);
    ImU32 colorB = (state.selected_segment == i && state.selected_end == 1)
                       ? selected_color
                       : IM_COL32(200, 220, 200, 220);
    draw_list->AddLine(sa, sb, IM_COL32(180, 200, 220, 160), 2.0f);
    draw_list->AddCircleFilled(sa, 6.0f, colorA);
    draw_list->AddCircle(sa, 6.5f, IM_COL32(60, 80, 100, 180), 12, 2);
    draw_list->AddCircleFilled(sb, 6.0f, colorB);
    draw_list->AddCircle(sb, 6.5f, IM_COL32(60, 80, 100, 180), 12, 2);
  }

  draw_list->AddRect(canvas_min, canvas_max, IM_COL32(255, 255, 255, 80), 0.0f,
                     0, 2.0f);

  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::Columns(1);

  ImGui::End();
}

int main(int, char **) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  TrackState state;
  HelloImGui::RunnerParams runnerParams;
  runnerParams.appWindowParams.windowTitle = "Track Annotator";
  runnerParams.imGuiWindowParams.menuAppTitle = "Track Annotator";
  runnerParams.appWindowParams.windowGeometry.size = {1200, 900};
  runnerParams.appWindowParams.restorePreviousGeometry = true;
  runnerParams.imGuiWindowParams.showMenuBar = false;
  runnerParams.imGuiWindowParams.defaultImGuiWindowType =
      HelloImGui::DefaultImGuiWindowType::ProvideFullScreenDockSpace;
  runnerParams.iniFolderType = HelloImGui::IniFolderType::AppUserConfigFolder;
  runnerParams.iniFilename = "TrackAnnotator/track_annotator.ini";
  runnerParams.callbacks.ShowGui = [&state] { ShowAnnotatorGui(state); };
  HelloImGui::Run(runnerParams);
  curl_global_cleanup();
  return 0;
}
