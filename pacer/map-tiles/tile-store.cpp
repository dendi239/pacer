#include "tile-store.hpp"

// Resolves to glad on desktop and to GLES3 under emscripten, matching
// whichever loader hello_imgui was configured with.
#include <hello_imgui/hello_imgui_include_opengl.h>

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

void TileStore::RequestTile(int zoom, int x, int y) {
  if (zoom < 0 || zoom > kMaxSatelliteZoom)
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
  for (const auto &key : queue_.TakeReady(now)) {
    auto [zoom, x, y] = key;
    cache_[key].status = "Loading";
    loader_->Enqueue(TileRequest{zoom, x, y, SatelliteTileUrl(zoom, x, y)});
  }
}

void TileStore::ApplyResults() {
  auto now = TileRequestQueue::Clock::now();

  for (auto &result : loader_->DrainResults()) {
    TileRequestQueue::Key key{result.zoom, result.x, result.y};
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
