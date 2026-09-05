#pragma once

#include <map>
#include <memory>
#include <string>
#include <tuple>

#include <pacer/map-tiles/tile-queue.hpp>

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
//
// Requests are throttled rather than fired off as they arrive; see
// TileRequestQueue for the policy.
class TileStore {
public:
  TileStore();
  ~TileStore();

  TileStore(const TileStore &) = delete;
  TileStore &operator=(const TileStore &) = delete;

  /// Requests a tile unless it is already cached, downloading, or backing
  /// off after a failure. Cheap no-op otherwise, so it is fine -- and
  /// expected -- to call for the whole visible range every frame: that
  /// repetition is what keeps a queued tile from being dropped.
  void RequestTile(int zoom, int x, int y);

  /// Uploads downloads that finished since the last call, forgets tiles the
  /// caller has stopped asking for, and starts as many queued downloads as
  /// there are free slots. Call once per frame on the thread owning the GL
  /// context.
  void ApplyResults();

  /// Cached tile lookup; returns nullptr for tiles never requested.
  const MapTileImage *Find(int zoom, int x, int y) const;

  /// Number of tiles queued or downloading but not yet applied. Lets the
  /// app raise its idle frame rate while downloads are outstanding (the
  /// render loop is not woken by worker threads), and idle hard once
  /// everything has landed.
  size_t PendingCount() const { return queue_.PendingCount(); }

private:
  /// Starts whatever the queue says is ready, and marks it as loading.
  void Dispatch(TileRequestQueue::Clock::time_point now);

  std::map<TileRequestQueue::Key, MapTileImage> cache_;
  TileRequestQueue queue_;
  std::unique_ptr<TileLoader> loader_;
};

} // namespace pacer
