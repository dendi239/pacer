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
struct TileResult;

enum class ImageryProvider { kEsri, kGoogle };

/// Where satellite tiles come from. Esri needs nothing; Google needs a Map
/// Tiles API key (https://developers.google.com/maps/documentation/tile).
struct ImagerySource {
  ImageryProvider provider = ImageryProvider::kEsri;
  std::string google_api_key;

  bool operator==(const ImagerySource &) const = default;
};

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

  /// Switches imagery source. Drops every cached tile and ignores downloads
  /// still in flight from the old source. For Google, starts a session
  /// first; tiles are only requested once it is up (see SourceStatus).
  void SetSource(ImagerySource source);
  const ImagerySource &Source() const { return source_; }

  /// Why tiles aren't loading yet ("Starting Google session...", a key
  /// error, ...), or empty once the source is ready.
  const std::string &SourceStatus() const { return source_status_; }

  /// Credit line the current provider's terms require on the map.
  const char *Attribution() const;

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
  size_t PendingCount() const {
    return queue_.PendingCount() + (session_in_flight_ ? 1 : 0);
  }

private:
  /// Starts whatever the queue says is ready, and marks it as loading.
  void Dispatch(TileRequestQueue::Clock::time_point now);

  /// True once tile URLs can be built (Google needs its session first).
  bool SourceReady() const;
  std::string TileUrl(int zoom, int x, int y) const;
  void ApplySessionResult(const TileResult &result);

  std::map<TileRequestQueue::Key, MapTileImage> cache_;
  TileRequestQueue queue_;
  std::unique_ptr<TileLoader> loader_;

  ImagerySource source_;
  std::string source_status_;
  std::string google_session_;
  bool session_in_flight_ = false;
  // Bumped on every SetSource, and carried by session requests, so a slow
  // session reply for a source the user already moved away from is ignored.
  int source_generation_ = 0;
};

} // namespace pacer
