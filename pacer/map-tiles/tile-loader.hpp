#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace pacer {

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

/// Fetches tile bytes without blocking the render thread. Enqueue() starts a
/// download and returns immediately; DrainResults() hands back whatever has
/// finished since the last call, so the caller polls once per frame rather
/// than waiting on anything.
///
/// Two implementations exist behind this interface -- a curl worker pool on
/// desktop, and the browser's own fetch under emscripten -- because a wasm
/// build has neither libcurl nor (in a single-threaded build) worker threads
/// to run it on. TileStore is written against this interface alone and does
/// not care which one it got.
class TileLoader {
public:
  /// `concurrency` is a hint for backends that own their own workers; the
  /// browser backend ignores it, since TileRequestQueue already caps how
  /// many requests are in flight.
  explicit TileLoader(size_t concurrency);
  ~TileLoader();

  TileLoader(const TileLoader &) = delete;
  TileLoader &operator=(const TileLoader &) = delete;

  void Enqueue(TileRequest request);
  std::vector<TileResult> DrainResults();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace pacer
