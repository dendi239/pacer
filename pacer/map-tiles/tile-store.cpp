#include "tile-store.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <glad/glad.h>

#include "stb_image.h"

#include "tile-math.hpp"

namespace pacer {

static size_t CurlWriteMemoryCallback(void *contents, size_t size, size_t nmemb,
                                      void *userp) {
  size_t total_size = size * nmemb;
  auto *buffer = static_cast<std::vector<unsigned char> *>(userp);
  auto *data = static_cast<unsigned char *>(contents);
  buffer->insert(buffer->end(), data, data + total_size);
  return total_size;
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
                   "pacer-map-tiles/1.0 (+https://github.com/)");
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

TileStore::TileStore() {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  loader_ = std::make_unique<TileLoader>(4);
}

TileStore::~TileStore() {
  // Join workers before tearing down curl globally: curl_global_cleanup
  // must not race with an in-flight curl_easy_perform.
  loader_.reset();
  curl_global_cleanup();
}

void TileStore::RequestTile(int zoom, int x, int y) {
  if (zoom < 0 || zoom > kMaxSatelliteZoom)
    return;
  int n = 1 << zoom;
  if (x < 0 || x >= n || y < 0 || y >= n)
    return;

  auto &tile = cache_[std::make_tuple(zoom, x, y)];
  if (tile.valid || tile.status == "Loading")
    return;

  tile.status = "Loading";
  loader_->Enqueue(TileRequest{zoom, x, y, SatelliteTileUrl(zoom, x, y)});
}

void TileStore::ApplyResults() {
  for (auto &result : loader_->DrainResults()) {
    auto &tile = cache_[std::make_tuple(result.zoom, result.x, result.y)];
    if (!result.ok) {
      tile.status = "Error: " + result.error;
      continue;
    }
    std::string error;
    if (!UpdateMapTileTexture(result.image_data, result.url, tile, error)) {
      tile.status = "Error: " + error;
    }
  }
}

const MapTileImage *TileStore::Find(int zoom, int x, int y) const {
  auto it = cache_.find(std::make_tuple(zoom, x, y));
  return it == cache_.end() ? nullptr : &it->second;
}

} // namespace pacer
