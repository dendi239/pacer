#include "tile-store.hpp"

// Resolves to glad on desktop and to GLES3 under emscripten, matching
// whichever loader hello_imgui was configured with.
#include <hello_imgui/hello_imgui_include_opengl.h>

#include <nlohmann/json.hpp>

#include "stb_image.h"

#include "tile-loader.hpp"
#include "tile-math.hpp"

namespace pacer {

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

TileStore::TileStore()
    : loader_(std::make_unique<TileLoader>(TileRequestQueue::kMaxInFlight)) {}

TileStore::~TileStore() = default;

// Session requests share the loader with tiles; a negative zoom marks them,
// and x carries the source generation they were made for.
static constexpr int kSessionRequestZoom = -1;

void TileStore::SetSource(ImagerySource source) {
  if (source == source_)
    return;

  for (auto &[key, tile] : cache_) {
    if (tile.texture)
      glDeleteTextures(1, &tile.texture);
  }
  cache_.clear();
  queue_.ClearFailures();

  source_ = std::move(source);
  ++source_generation_;
  google_session_.clear();
  session_in_flight_ = false;
  source_status_.clear();

  if (source_.provider == ImageryProvider::kGoogle) {
    if (source_.google_api_key.empty()) {
      source_status_ = "Enter a Google Maps API key";
      return;
    }
    source_status_ = "Starting Google session...";
    session_in_flight_ = true;
    loader_->Enqueue(TileRequest{kSessionRequestZoom, source_generation_, 0,
                                 GoogleCreateSessionUrl(source_.google_api_key),
                                 GoogleSatelliteSessionJson()});
  }
}

const char *TileStore::Attribution() const {
  switch (source_.provider) {
  case ImageryProvider::kGoogle:
    return "Imagery \xC2\xA9 Google";
  case ImageryProvider::kEsri:
    break;
  }
  return "Esri, Maxar, Earthstar Geographics";
}

bool TileStore::SourceReady() const {
  return source_.provider == ImageryProvider::kEsri ||
         !google_session_.empty();
}

std::string TileStore::TileUrl(int zoom, int x, int y) const {
  if (source_.provider == ImageryProvider::kGoogle)
    return GoogleTileUrl(zoom, x, y, google_session_, source_.google_api_key);
  return SatelliteTileUrl(zoom, x, y);
}

void TileStore::ApplySessionResult(const TileResult &result) {
  if (result.x != source_generation_)
    return; // reply for a source we have since switched away from
  session_in_flight_ = false;

  // Success and failure both answer in JSON: {"session": ...} or
  // {"error": {"message": ...}}.
  auto body = nlohmann::json::parse(result.image_data.begin(),
                                    result.image_data.end(), nullptr,
                                    /*allow_exceptions=*/false);
  if (result.ok && body.is_object() && body.contains("session") &&
      body["session"].is_string()) {
    google_session_ = body["session"].get<std::string>();
    source_status_.clear();
    return;
  }

  std::string reason = result.error;
  if (body.is_object() && body.contains("error") &&
      body["error"].is_object() && body["error"].contains("message") &&
      body["error"]["message"].is_string())
    reason = body["error"]["message"].get<std::string>();
  source_status_ = "Google session failed: " + reason;
}

void TileStore::RequestTile(int zoom, int x, int y) {
  if (zoom < 0 || zoom > kMaxSatelliteZoom)
    return;
  // Nothing to fetch from yet; asking again next frame is free.
  if (!SourceReady())
    return;
  int n = 1 << zoom;
  if (x < 0 || x >= n || y < 0 || y >= n)
    return;

  TileRequestQueue::Key key{zoom, x, y};
  auto &tile = cache_[key];
  if (tile.valid)
    return;

  auto now = TileRequestQueue::Clock::now();
  queue_.Want(key, now);
  Dispatch(now);
}

void TileStore::Dispatch(TileRequestQueue::Clock::time_point now) {
  if (!SourceReady())
    return;
  for (const auto &key : queue_.TakeReady(now)) {
    auto [zoom, x, y] = key;
    cache_[key].status = "Loading";
    loader_->Enqueue(TileRequest{zoom, x, y, TileUrl(zoom, x, y)});
  }
}

void TileStore::ApplyResults() {
  auto now = TileRequestQueue::Clock::now();

  for (auto &result : loader_->DrainResults()) {
    if (result.zoom == kSessionRequestZoom) {
      ApplySessionResult(result);
      continue;
    }

    TileRequestQueue::Key key{result.zoom, result.x, result.y};
    // Fetched for a source (or session) we have since switched away from:
    // free its slot, keep nothing. The tile is re-wanted from the new source
    // on the next frame it is visible.
    if (!SourceReady() || result.url != TileUrl(result.zoom, result.x,
                                                result.y)) {
      queue_.Finish(key, true, now);
      continue;
    }
    auto &tile = cache_[key];

    std::string error;
    bool ok = result.ok;
    if (!ok) {
      error = result.error;
    } else if (!UpdateMapTileTexture(result.image_data, result.url, tile,
                                     error)) {
      ok = false;
    }
    if (!ok)
      tile.status = "Error: " + error;
    queue_.Finish(key, ok, now);
  }

  Dispatch(now);
}

const MapTileImage *TileStore::Find(int zoom, int x, int y) const {
  auto it = cache_.find(TileRequestQueue::Key{zoom, x, y});
  return it == cache_.end() ? nullptr : &it->second;
}

} // namespace pacer
