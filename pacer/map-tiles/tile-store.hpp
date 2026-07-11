#pragma once

#include <map>
#include <memory>
#include <string>
#include <tuple>

namespace pacer {

struct MapTileImage {
  unsigned int texture = 0; // GL texture name
  int width = 0;
  int height = 0;
  bool valid = false;
  std::string url;
  std::string status = "Pending";
};

struct TileLoader;

// Cache of satellite tiles backed by a small curl worker pool. Downloads
// happen on worker threads so fetches never block the render thread;
// ApplyResults() turns finished downloads into GL textures and must run on
// the thread owning the GL context. The constructor/destructor own the curl
// global state and the worker lifetime, so teardown order can't go wrong.
class TileStore {
public:
  TileStore();
  ~TileStore();

  TileStore(const TileStore &) = delete;
  TileStore &operator=(const TileStore &) = delete;

  /// Enqueues a download unless the tile is cached or in flight. Cheap
  /// no-op otherwise, so it's fine to call for the whole visible range
  /// every frame.
  void RequestTile(int zoom, int x, int y);

  /// Uploads downloads that finished since the last call. Call once per
  /// frame on the render thread.
  void ApplyResults();

  /// Cached tile lookup; returns nullptr for tiles never requested.
  const MapTileImage *Find(int zoom, int x, int y) const;

private:
  std::map<std::tuple<int, int, int>, MapTileImage> cache_;
  std::unique_ptr<TileLoader> loader_;
};

} // namespace pacer
