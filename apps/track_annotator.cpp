#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <curl/curl.h>
#include <filesystem>
#include <glad/glad.h>
#include <hello_imgui/hello_imgui.h>
#include <imgui.h>
#include <imgui_stdlib.h>

#include <pacer/reference-track/reference-track.hpp>

#include "../3rdparty/hello_imgui/external/stb_hello_imgui/stb_image.h"

struct TrackPoint {
  float lat = 0.0f;
  float lon = 0.0f;
};

struct TrackSegment {
  TrackPoint a;                 // inner edge
  TrackPoint b;                 // outer edge
  bool complete = true;         // if false, b is not set yet
  bool is_sector_split = false; // marks this gate as a sector boundary
};

struct MapTileImage;
struct TileLoader;

// The undoable part of the annotation: the drawn gates and whether the
// track loops back on itself. View/camera state, tile settings, and file
// paths are intentionally excluded from undo/redo history.
struct TrackDocument {
  std::vector<TrackSegment> segments;
  bool track_closed = false;
};

struct TrackState {
  std::vector<TrackSegment> segments;
  int selected_segment = -1;
  int selected_end = 0;      // 0 = a, 1 = b
  int hovered_segment = -1;  // row currently hovered in the segment table
  bool track_closed = false; // joins last gate to first; blocks new gates
  std::vector<TrackDocument> undo_stack;
  std::vector<TrackDocument> redo_stack;
  bool dragging_point = false;
  ImVec2 view_offset = {0.0f, 0.0f};
  float view_zoom = 1.0f;
  std::string state_filename = "track_annotation.json";
  std::string last_message;

  int map_tile_zoom = 19;
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
  std::unique_ptr<TileLoader> tile_loader;
};

struct MapTileImage {
  GLuint texture = 0;
  int width = 0;
  int height = 0;
  bool valid = false;
  std::string url;
  std::string status = "Pending";
};

// Records the current document (segments + track_closed) onto the undo
// stack and clears redo history, as usual once a new edit is made. Call
// this right before a mutation, so the pushed snapshot is the pre-edit
// state.
static void PushUndo(TrackState &state) {
  constexpr size_t kMaxUndoHistory = 200;
  state.undo_stack.push_back(TrackDocument{state.segments, state.track_closed});
  state.redo_stack.clear();
  if (state.undo_stack.size() > kMaxUndoHistory) {
    state.undo_stack.erase(state.undo_stack.begin());
  }
}

static void RestoreDocument(TrackState &state, TrackDocument doc) {
  state.segments = std::move(doc.segments);
  state.track_closed = doc.track_closed;
  state.selected_segment = -1;
  state.dragging_point = false;
}

static void Undo(TrackState &state) {
  if (state.undo_stack.empty()) {
    return;
  }
  state.redo_stack.push_back(TrackDocument{state.segments, state.track_closed});
  TrackDocument doc = std::move(state.undo_stack.back());
  state.undo_stack.pop_back();
  RestoreDocument(state, std::move(doc));
}

static void Redo(TrackState &state) {
  if (state.redo_stack.empty()) {
    return;
  }
  state.undo_stack.push_back(TrackDocument{state.segments, state.track_closed});
  TrackDocument doc = std::move(state.redo_stack.back());
  state.redo_stack.pop_back();
  RestoreDocument(state, std::move(doc));
}

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

struct TileRequest {
  int zoom;
  int x;
  int y;
  std::string url;
};

struct TileResult {
  int zoom;
  int x;
  int y;
  std::string url;
  bool ok = false;
  std::vector<unsigned char> image_data;
  std::string error;
};

// Downloads tiles on a small worker pool so satellite fetches never block the
// render thread. Textures are still created from the results on the main
// thread by ApplyTileResults, since the GL context lives there.
struct TileLoader {
  explicit TileLoader(size_t thread_count) {
    for (size_t i = 0; i < thread_count; ++i)
      workers.emplace_back([this] { WorkerLoop(); });
  }

  ~TileLoader() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stop = true;
    }
    cv.notify_all();
    for (auto &worker : workers)
      worker.join();
  }

  void Enqueue(TileRequest request) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      pending.push_back(std::move(request));
    }
    cv.notify_one();
  }

  std::vector<TileResult> DrainResults() {
    std::vector<TileResult> results;
    std::lock_guard<std::mutex> lock(mutex);
    results.swap(completed);
    return results;
  }

  void WorkerLoop() {
    while (true) {
      TileRequest request;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return stop || !pending.empty(); });
        if (stop && pending.empty())
          return;
        request = std::move(pending.front());
        pending.pop_front();
      }

      TileResult result;
      result.zoom = request.zoom;
      result.x = request.x;
      result.y = request.y;
      result.url = request.url;
      result.ok =
          DownloadImageToMemory(request.url, result.image_data, result.error);

      std::lock_guard<std::mutex> lock(mutex);
      completed.push_back(std::move(result));
    }
  }

  std::mutex mutex;
  std::condition_variable cv;
  std::deque<TileRequest> pending;
  std::vector<TileResult> completed;
  std::vector<std::thread> workers;
  bool stop = false;
};

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

// Enqueues a background download for (zoom, x, y) if it isn't already cached
// or in flight. Returns immediately; the result is picked up later by
// ApplyTileResults once the worker thread finishes.
static void RequestTile(TrackState &state, int zoom, int x, int y) {
  if (zoom < 0 || zoom > 19)
    return;
  int n = 1 << zoom;
  if (x < 0 || x >= n || y < 0 || y >= n)
    return;

  auto key = std::make_tuple(zoom, x, y);
  auto &tile = state.tile_cache[key];
  if (tile.valid || tile.status == "Loading")
    return;

  tile.status = "Loading";
  state.tile_loader->Enqueue(
      TileRequest{zoom, x, y, BuildSatelliteTileUrl(zoom, x, y)});
}

// Applies any tile downloads that finished on the worker pool since the last
// call. Must run on the main thread, since texture creation needs the GL
// context.
static void ApplyTileResults(TrackState &state) {
  for (auto &result : state.tile_loader->DrainResults()) {
    auto key = std::make_tuple(result.zoom, result.x, result.y);
    auto &tile = state.tile_cache[key];
    bool is_active_tile = result.zoom == state.map_tile_zoom &&
                          result.x == state.map_tile_x &&
                          result.y == state.map_tile_y;

    if (!result.ok) {
      tile.status = "Error: " + result.error;
    } else {
      std::string error;
      if (UpdateMapTileTexture(result.image_data, result.url, tile, error)) {
        if (is_active_tile)
          state.map_tile_status = "Loaded " + std::to_string(tile.width) + "x" +
                                  std::to_string(tile.height);
      } else {
        tile.status = "Error: " + error;
      }
    }

    if (is_active_tile && !tile.valid)
      state.map_tile_status = tile.status;
  }
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

  // RequestTile is a cheap no-op for tiles that are already cached or in
  // flight, so it's fine to ask for the whole visible range every frame;
  // actual concurrency is bounded by the worker pool in TileLoader.
  for (int ty = min_ty; ty <= max_ty; ++ty) {
    for (int tx = min_tx; tx <= max_tx; ++tx) {
      RequestTile(state, zoom, tx, ty);
    }
  }
}

static void RenderVisibleTiles(const TrackState &state, ImDrawList *draw_list,
                               const ImVec2 &canvas_min,
                               const ImVec2 &canvas_size) {
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
      ImVec2 tile_min = ImVec2(origin.x + (tx - center_tx) * tile_screen_size,
                               origin.y + (ty - center_ty) * tile_screen_size);
      ImVec2 tile_max =
          ImVec2(tile_min.x + tile_screen_size, tile_min.y + tile_screen_size);
      auto key = std::make_tuple(zoom, tx, ty);
      auto it = state.tile_cache.find(key);

      if (it != state.tile_cache.end() && it->second.valid) {
        draw_list->AddImage((ImTextureID)(intptr_t)it->second.texture, tile_min,
                            tile_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
      } else {
        draw_list->AddRectFilled(tile_min, tile_max, IM_COL32(18, 22, 35, 255));
        draw_list->AddRect(tile_min, tile_max, IM_COL32(80, 92, 120, 150), 0.0f,
                           0, 1.0f);
        if (it != state.tile_cache.end()) {
          draw_list->AddText(ImVec2(tile_min.x + 4.0f, tile_min.y + 4.0f),
                             IM_COL32(200, 200, 220, 180),
                             it->second.status.c_str());
        }
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

static bool SaveState(const TrackState &state, const std::string &filename) {
  std::vector<TrackSegment> complete_segments;
  for (const auto &seg : state.segments) {
    if (seg.complete) {
      complete_segments.push_back(seg);
    }
  }

  pacer::ReferenceTrack track;
  if (!complete_segments.empty()) {
    track.cs = pacer::CoordinateSystem(pacer::GPSSample{
        .lat = complete_segments.front().a.lat,
        .lon = complete_segments.front().a.lon,
    });
  }
  for (int i = 0; i < (int)complete_segments.size(); ++i) {
    const auto &seg = complete_segments[i];
    pacer::GPSSample a{.lat = seg.a.lat, .lon = seg.a.lon};
    pacer::GPSSample b{.lat = seg.b.lat, .lon = seg.b.lon};
    track.segments.push_back(pacer::Segment{pacer::ToPoint(track.cs.Local(a)),
                                            pacer::ToPoint(track.cs.Local(b))});
    if (seg.is_sector_split) {
      track.sector_indices.push_back(i);
    }
  }

  try {
    track.SaveToFile(filename);
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

static bool ReadSegmentsFromFile(const std::string &filename,
                                 std::vector<TrackSegment> &out_segments,
                                 std::string &error) {
  try {
    pacer::ReferenceTrack track = pacer::ReferenceTrack::FromFile(filename);
    std::vector<TrackSegment> segments;
    segments.reserve(track.segments.size());
    for (const auto &seg : track.segments) {
      auto a = track.cs.Global(pacer::Vec3f{seg.first.x, seg.first.y, 0});
      auto b = track.cs.Global(pacer::Vec3f{seg.second.x, seg.second.y, 0});
      TrackSegment s;
      s.a.lat = static_cast<float>(a.lat);
      s.a.lon = static_cast<float>(a.lon);
      s.b.lat = static_cast<float>(b.lat);
      s.b.lon = static_cast<float>(b.lon);
      s.complete = true;
      segments.push_back(s);
    }
    for (int index : track.sector_indices) {
      if (index >= 0 && index < (int)segments.size()) {
        segments[index].is_sector_split = true;
      }
    }
    out_segments = std::move(segments);
  } catch (const std::exception &e) {
    error = std::string("Invalid state file: ") + e.what();
    return false;
  }

  return true;
}

// Centers the map on the geometric center of state.segments and resets pan
// offset. Does not touch view_zoom.
static void RecenterMapOnSegments(TrackState &state) {
  state.view_offset = ImVec2(0.0f, 0.0f);
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
}

static bool LoadState(TrackState &state, const std::string &filename) {
  std::vector<TrackSegment> segments;
  std::string error;
  if (!ReadSegmentsFromFile(filename, segments, error))
    return false;

  state.segments = std::move(segments);

  // Do not restore view_offset or view_zoom from file. Instead center the map
  // on the geometric center of the loaded segments and reset pan offset.
  RecenterMapOnSegments(state);

  state.selected_segment = -1;
  state.dragging_point = false;
  state.track_closed = true;

  return true;
}

// Left column: tile/view controls, selected-segment editor, file load/save,
// and the segment table.
static void DrawControlPanel(TrackState &state, float width) {
  // Child window (scrollable) so it doesn't scroll the map.
  ImGui::BeginChild("left_panel", ImVec2(width, 0), true,
                    ImGuiWindowFlags_None);
  ImGui::TextWrapped(
      "Draw a sequence of gates (inner edge to outer edge) around the "
      "circuit, in the direction of travel.");
  ImGui::TextWrapped(
      "Map: left-click to pan, or left-click and drag a point to move it. "
      "Right-click to place a gate point (first click sets the inner "
      "edge, the next sets the outer edge; the following right-click "
      "starts a new gate). Scroll to zoom.");
  ImGui::Spacing();

  // --- Left column: controls and lists ---
  ImGui::TextWrapped(
      "Zoom/Lat/Lon control the satellite tile view. Auto-load visible "
      "tiles fetches tiles as you pan; Show infill toggles the filled "
      "track surface; Fetch Satellite Tile grabs the current tile "
      "manually.");
  ImGui::Text("Zoom");
  ImGui::SameLine();
  ImGui::PushItemWidth(85.0f);
  ImGui::InputInt("##zoom", &state.map_tile_zoom, 1, 1);
  ImGui::PopItemWidth();
  ImGui::SameLine();
  ImGui::Text("Lat");
  ImGui::SameLine();
  ImGui::PushItemWidth(70.0f);
  ImGui::InputFloat("##lat", &state.map_tile_lat, 0.0f, 0.0f, "%.5f");
  ImGui::SameLine();
  ImGui::Text("Lon");
  ImGui::SameLine();
  ImGui::InputFloat("##lon", &state.map_tile_lon, 0.0f, 0.0f, "%.5f");
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
    int x = 0, y = 0;
    std::string url = BuildSatelliteTileUrl(
        state.map_tile_lat, state.map_tile_lon, state.map_tile_zoom, x, y);
    state.map_tile_x = x;
    state.map_tile_y = y;
    RequestTile(state, state.map_tile_zoom, x, y);
    state.map_tile_status = "Fetching...";
    HelloImGui::Log(HelloImGui::LogLevel::Info, "Requested satellite tile %s",
                    url.c_str());
  }
  ImGui::SameLine();
  ImGui::Text("%s", state.map_tile_status.c_str());

  if (ImGui::Button("Reset View")) {
    state.view_offset = {0.0f, 0.0f};
    state.view_zoom = 1.0f;
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset Track")) {
    PushUndo(state);
    state.segments.clear();
    state.selected_segment = -1;
    state.selected_end = 0;
    state.track_closed = false;
  }
  {
    bool old_closed = state.track_closed;
    if (ImGui::Checkbox("Close the track", &state.track_closed)) {
      bool new_closed = state.track_closed;
      state.track_closed = old_closed;
      PushUndo(state);
      state.track_closed = new_closed;
    }
  }

  ImGui::Separator();

  // Selected segment editor, save/load and segment table live in left column.
  if (state.selected_segment >= 0 &&
      state.selected_segment < (int)state.segments.size()) {
    auto &seg = state.segments[state.selected_segment];
    ImGui::PushItemWidth(80.0f);
    ImGui::Text("Point A");
    ImGui::SameLine();
    ImGui::DragFloat("##a_lat", &seg.a.lat, 0.00005f, -85.0f, 85.0f, "%.5f");
    if (ImGui::IsItemActivated())
      PushUndo(state);
    ImGui::SameLine();
    ImGui::DragFloat("##a_lon", &seg.a.lon, 0.00005f, -180.0f, 180.0f, "%.5f");
    if (ImGui::IsItemActivated())
      PushUndo(state);
    ImGui::Text("Point B");
    ImGui::SameLine();
    ImGui::DragFloat("##b_lat", &seg.b.lat, 0.00005f, -85.0f, 85.0f, "%.5f");
    if (ImGui::IsItemActivated())
      PushUndo(state);
    ImGui::SameLine();
    ImGui::DragFloat("##b_lon", &seg.b.lon, 0.00005f, -180.0f, 180.0f, "%.5f");
    if (ImGui::IsItemActivated())
      PushUndo(state);
    ImGui::PopItemWidth();
    if (ImGui::Button("Delete segment") && state.selected_segment >= 0) {
      PushUndo(state);
      auto it = state.segments.begin() + state.selected_segment;
      state.segments.erase(it);
      state.selected_segment = -1;
    }

    // Insert a gate interpolated from the selected one and its neighbor, for
    // refining corners without hand-placing a whole new point.
    int i = state.selected_segment;
    bool has_next = i + 1 < (int)state.segments.size();
    bool has_prev = i > 0;
    ImGui::BeginDisabled(!has_next);
    if (ImGui::Button("Add Next")) {
      PushUndo(state);
      int j = i + 1;
      TrackSegment mid;
      mid.a.lat = (state.segments[i].a.lat + state.segments[j].a.lat) / 2.0f;
      mid.a.lon = (state.segments[i].a.lon + state.segments[j].a.lon) / 2.0f;
      mid.b.lat = (state.segments[i].b.lat + state.segments[j].b.lat) / 2.0f;
      mid.b.lon = (state.segments[i].b.lon + state.segments[j].b.lon) / 2.0f;
      mid.complete = true;
      mid.is_sector_split = false;
      state.segments.insert(state.segments.begin() + j, mid);
      state.selected_segment = j;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!has_prev);
    if (ImGui::Button("Add Prev")) {
      PushUndo(state);
      int j = i - 1;
      TrackSegment mid;
      mid.a.lat = (state.segments[j].a.lat + state.segments[i].a.lat) / 2.0f;
      mid.a.lon = (state.segments[j].a.lon + state.segments[i].a.lon) / 2.0f;
      mid.b.lat = (state.segments[j].b.lat + state.segments[i].b.lat) / 2.0f;
      mid.b.lon = (state.segments[j].b.lon + state.segments[i].b.lon) / 2.0f;
      mid.complete = true;
      mid.is_sector_split = false;
      state.segments.insert(state.segments.begin() + i, mid);
      state.selected_segment = i;
    }
    ImGui::EndDisabled();
  }

  ImGui::Spacing();
  ImGui::TextWrapped(
      "Save state / Load state read and write the gates and sector "
      "markings to the state file below.");
  ImGui::SetNextItemWidth(-1);
  ImGui::InputText("##statefile", &state.state_filename);
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
  ImGui::SameLine();
  if (ImGui::Button("Load state##load")) {
    PushUndo(state);
    if (!LoadState(state, state.state_filename)) {
      state.undo_stack.pop_back(); // load failed, nothing actually changed
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

  ImGui::Spacing();
  ImGui::TextWrapped(
      "Segment table: click a row to select that gate for editing above. "
      "Check Sector to mark it as a sector-split boundary; gate 0 is "
      "always the start/finish line.");
  state.hovered_segment = -1;
  if (ImGui::BeginTable("points_table", 3, ImGuiTableFlags_Borders)) {
    ImGui::TableSetupColumn("Index");
    ImGui::TableSetupColumn("Point");
    ImGui::TableSetupColumn("Sector");
    ImGui::TableHeadersRow();
    for (int i = 0; i < (int)state.segments.size(); ++i) {
      ImGui::PushID(i);
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      std::string index_label =
          i == 0 ? std::to_string(i) + " (start/finish)" : std::to_string(i);
      if (ImGui::Selectable(index_label.c_str(), state.selected_segment == i,
                            ImGuiSelectableFlags_SpanAllColumns |
                                ImGuiSelectableFlags_AllowOverlap)) {
        state.selected_segment = i;
      }
      if (ImGui::IsItemHovered()) {
        state.hovered_segment = i;
      }
      ImGui::TableSetColumnIndex(1);
      ImGui::Text("A: %.5f, %.5f | B: %.5f, %.5f", state.segments[i].a.lat,
                  state.segments[i].a.lon, state.segments[i].b.lat,
                  state.segments[i].b.lon);
      ImGui::TableSetColumnIndex(2);
      if (i > 0) {
        bool old_value = state.segments[i].is_sector_split;
        if (ImGui::Checkbox("##sector", &state.segments[i].is_sector_split)) {
          bool new_value = state.segments[i].is_sector_split;
          state.segments[i].is_sector_split = old_value;
          PushUndo(state);
          state.segments[i].is_sector_split = new_value;
        }
      } else {
        // Disabled placeholder so row 0 has the same height as every other
        // row (which all have a real checkbox here) -- otherwise this row
        // renders shorter and every checkbox below it visually creeps up by
        // about a row.
        bool not_applicable = false;
        ImGui::BeginDisabled();
        ImGui::Checkbox("##sector", &not_applicable);
        ImGui::EndDisabled();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (!state.last_message.empty()) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.9f, 0.5f, 1.0f));
    ImGui::TextWrapped("%s", state.last_message.c_str());
    ImGui::PopStyleColor();
  }

  ImGui::EndChild();
}

// Applies pan/zoom/click/drag input for the map canvas. hovered indicates
// whether the mouse is over the canvas item drawn just before this call.
static void HandleMapInput(TrackState &state, const ImVec2 &canvas_min,
                           const ImVec2 &canvas_max, bool hovered) {
  ImVec2 mouse_pos = ImGui::GetIO().MousePos;
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

  if (right_clicked && !state.track_closed) {
    PushUndo(state);
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
      PushUndo(state);
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
}

// Draws the track quads (infill + outline) and segment endpoint handles.
static void DrawTrackOverlay(const TrackState &state, ImDrawList *draw_list,
                             const ImVec2 &canvas_min,
                             const ImVec2 &canvas_max) {
  // Cycled per sector (a run of gates between sector-split boundaries) so
  // adjacent sectors are visually distinguishable.
  static const ImU32 kSectorColors[] = {
      IM_COL32(180, 220, 255, 160), IM_COL32(255, 200, 140, 160),
      IM_COL32(180, 255, 180, 160), IM_COL32(255, 180, 220, 160),
      IM_COL32(220, 220, 140, 160), IM_COL32(200, 180, 255, 160),
  };
  constexpr int kSectorColorCount =
      sizeof(kSectorColors) / sizeof(kSectorColors[0]);

  ImU32 selected_color = IM_COL32(255, 120, 120, 220);
  ImU32 hover_color = IM_COL32(255, 220, 80, 220);

  // Draw quads between consecutive segments (a_i, b_i, b_{i+1}, a_{i+1}),
  // colored per sector; a gate marked as a sector split ends its sector, so
  // the quad reaching it still uses the old color and the next one advances.
  if (state.segments.size() >= 2) {
    int sector = 0;
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
        draw_list->AddConvexPolyFilled(
            poly, 4, kSectorColors[sector % kSectorColorCount]);
      }
      draw_list->AddPolyline(poly, 4, IM_COL32(40, 60, 80, 200), true, 2.0f);
      if (state.segments[i + 1].is_sector_split) {
        ++sector;
      }
    }

    if (state.track_closed) {
      // Wraparound quad joining the last gate back to the start/finish gate.
      int last = static_cast<int>(state.segments.size() - 1);
      ImVec2 a0 =
          WorldToScreen(state.segments[last].a, canvas_min, canvas_max, state);
      ImVec2 b0 =
          WorldToScreen(state.segments[last].b, canvas_min, canvas_max, state);
      ImVec2 a1 =
          WorldToScreen(state.segments[0].a, canvas_min, canvas_max, state);
      ImVec2 b1 =
          WorldToScreen(state.segments[0].b, canvas_min, canvas_max, state);
      ImVec2 poly[4] = {a0, b0, b1, a1};
      if (state.show_infill) {
        draw_list->AddConvexPolyFilled(
            poly, 4, kSectorColors[sector % kSectorColorCount]);
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
    bool hovered = state.hovered_segment == i;
    ImU32 colorA = hovered ? hover_color
                   : (state.selected_segment == i && state.selected_end == 0)
                       ? selected_color
                       : IM_COL32(220, 240, 255, 220);
    ImU32 colorB = hovered ? hover_color
                   : (state.selected_segment == i && state.selected_end == 1)
                       ? selected_color
                       : IM_COL32(200, 220, 200, 220);
    bool bold = state.segments[i].is_sector_split;
    float radius = bold ? 8.0f : 6.0f;
    float outline_thickness = bold ? 3.0f : 2.0f;
    float line_thickness = bold ? 4.0f : 2.0f;
    draw_list->AddLine(sa, sb, IM_COL32(180, 200, 220, 160), line_thickness);
    draw_list->AddCircleFilled(sa, radius, colorA);
    draw_list->AddCircle(sa, radius + 0.5f, IM_COL32(60, 80, 100, 180), 12,
                         outline_thickness);
    draw_list->AddCircleFilled(sb, radius, colorB);
    draw_list->AddCircle(sb, radius + 0.5f, IM_COL32(60, 80, 100, 180), 12,
                         outline_thickness);
  }

  draw_list->AddRect(canvas_min, canvas_max, IM_COL32(255, 255, 255, 80), 0.0f,
                     0, 2.0f);
}

// Right column: satellite map, track overlay, and pan/zoom/edit input.
static void DrawMapCanvas(TrackState &state, const ImVec2 &canvas_size) {
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
  bool hovered = ImGui::IsItemHovered();

  EnsureVisibleTilesLoaded(state, canvas_min, canvas_size);
  RenderVisibleTiles(state, draw_list, canvas_min, canvas_size);

  HandleMapInput(state, canvas_min, canvas_max, hovered);
  DrawTrackOverlay(state, draw_list, canvas_min, canvas_max);

  ImGui::EndChild();
  ImGui::PopStyleColor();
}

static void ShowAnnotatorGui(TrackState &state) {
  ApplyTileResults(state);

  // Platform-native undo shortcut: Cmd+Z / Cmd+Shift+Z on macOS,
  // Ctrl+Z / Ctrl+Shift+Z everywhere else.
#ifdef __APPLE__
  constexpr ImGuiKeyChord kUndoMod = ImGuiMod_Super;
#else
  constexpr ImGuiKeyChord kUndoMod = ImGuiMod_Ctrl;
#endif
  if (!ImGui::GetIO().WantTextInput) {
    if (ImGui::IsKeyChordPressed(kUndoMod | ImGuiMod_Shift | ImGuiKey_Z)) {
      Redo(state);
    } else if (ImGui::IsKeyChordPressed(kUndoMod | ImGuiKey_Z)) {
      Undo(state);
    }
  }

  ImGui::Begin("Track Annotator");

  // Two-column layout: left = controls (~30%), right = map (~70%)
  float avail_w = ImGui::GetContentRegionAvail().x;
  float left_w = avail_w * 0.30f;
  ImGui::Columns(2, "main_columns", false);
  ImGui::SetColumnWidth(0, left_w);

  DrawControlPanel(state, left_w);
  ImGui::NextColumn();

  ImVec2 avail = ImGui::GetContentRegionAvail();
  float canvas_h = std::max(200.0f, avail.y);
  DrawMapCanvas(state, ImVec2(avail.x, canvas_h));

  ImGui::Columns(1);
  ImGui::End();
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::printf(
          "Usage: track_annotator [--lat LAT] [--lon LON] [--file PATH]\n"
          "  --lat LAT    initial map center latitude (default 51.376)\n"
          "  --lon LON    initial map center longitude (default -0.361)\n"
          "  --file PATH  state file to load on startup (if it exists) and\n"
          "               use as the default Save/Load state path\n"
          "               (default track_annotation.json)\n");
      return 0;
    }
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);
  TrackState state;
  state.tile_loader = std::make_unique<TileLoader>(4);

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--lat" && i + 1 < argc) {
      state.map_tile_lat = std::stof(argv[++i]);
    } else if (arg == "--lon" && i + 1 < argc) {
      state.map_tile_lon = std::stof(argv[++i]);
    } else if (arg == "--file" && i + 1 < argc) {
      state.state_filename = argv[++i];
    }
  }

  if (std::filesystem::exists(state.state_filename)) {
    if (LoadState(state, state.state_filename)) {
      state.last_message = "Loaded state from '" + state.state_filename + "'";
    } else {
      state.last_message =
          "Unable to load state from '" + state.state_filename + "'";
    }
  }

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
  // Stop and join worker threads before tearing down curl globally, since
  // curl_global_cleanup must not race with an in-flight curl_easy_perform.
  state.tile_loader.reset();
  curl_global_cleanup();
  return 0;
}
