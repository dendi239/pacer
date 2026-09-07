// Browser TileLoader: emscripten_fetch, i.e. the page's own fetch(). See
// tile-loader-curl.cpp for the desktop counterpart.
//
// Nothing here is threaded. The browser already performs the transfer off
// the main thread and calls back into wasm once the response has landed, so
// this backend is a thin adapter that parks finished responses until the
// render loop drains them. That also means the tile server must send
// permissive CORS headers -- ArcGIS World_Imagery does
// (Access-Control-Allow-Origin: *).

#include "tile-loader.hpp"

#include <cstdio>

#include <emscripten/fetch.h>

namespace pacer {

struct TileLoader::Impl {
  std::vector<TileResult> completed;
};

namespace {

// Handed to the fetch callbacks through emscripten_fetch_t::userData. Owned
// by the in-flight request and destroyed when it settles, either way.
// Points at the loader's result vector rather than the loader itself: this
// is the only thing the callbacks touch, and the Impl holding it is private.
struct FetchContext {
  std::vector<TileResult> *completed;
  int zoom;
  int x;
  int y;
  std::string url;
};

// Fills in the parts of the result that don't depend on success, then hands
// the context back for deletion.
TileResult ResultFor(const FetchContext &context) {
  TileResult result;
  result.zoom = context.zoom;
  result.x = context.x;
  result.y = context.y;
  result.url = context.url;
  return result;
}

void OnSuccess(emscripten_fetch_t *fetch) {
  auto *context = static_cast<FetchContext *>(fetch->userData);
  TileResult result = ResultFor(*context);

  const auto *bytes = reinterpret_cast<const unsigned char *>(fetch->data);
  if (fetch->numBytes > 0) {
    result.image_data.assign(bytes, bytes + fetch->numBytes);
    result.ok = true;
  } else {
    result.error = "Downloaded image data is empty";
  }

  context->completed->push_back(std::move(result));
  delete context;
  emscripten_fetch_close(fetch);
}

void OnError(emscripten_fetch_t *fetch) {
  auto *context = static_cast<FetchContext *>(fetch->userData);
  TileResult result = ResultFor(*context);
  // status 0 is what a CORS rejection or a dropped connection looks like
  // from here; the browser deliberately withholds the real reason.
  result.error = fetch->status == 0
                     ? std::string("Download failed (network or CORS)")
                     : "Download failed: HTTP " + std::to_string(fetch->status);

  context->completed->push_back(std::move(result));
  delete context;
  emscripten_fetch_close(fetch);
}

} // namespace

TileLoader::TileLoader(size_t /*concurrency*/)
    : impl_(std::make_unique<Impl>()) {}

TileLoader::~TileLoader() = default;

void TileLoader::Enqueue(TileRequest request) {
  emscripten_fetch_attr_t attr;
  emscripten_fetch_attr_init(&attr);
  std::snprintf(attr.requestMethod, sizeof(attr.requestMethod), "GET");
  attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
  attr.onsuccess = OnSuccess;
  attr.onerror = OnError;
  attr.timeoutMSecs = 30000;
  attr.userData = new FetchContext{&impl_->completed, request.zoom, request.x,
                                   request.y, request.url};
  emscripten_fetch(&attr, request.url.c_str());
}

std::vector<TileResult> TileLoader::DrainResults() {
  std::vector<TileResult> results;
  results.swap(impl_->completed);
  return results;
}

} // namespace pacer
