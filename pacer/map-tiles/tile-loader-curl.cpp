// Desktop TileLoader: a small pool of threads running blocking curl
// transfers. See tile-loader-fetch.cpp for the browser counterpart.

#include "tile-loader.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include <curl/curl.h>

namespace pacer {

namespace {

size_t CurlWriteMemoryCallback(void *contents, size_t size, size_t nmemb,
                               void *userp) {
  size_t total_size = size * nmemb;
  auto *buffer = static_cast<std::vector<unsigned char> *>(userp);
  auto *data = static_cast<unsigned char *>(contents);
  buffer->insert(buffer->end(), data, data + total_size);
  return total_size;
}

bool DownloadImageToMemory(const std::string &url,
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

} // namespace

struct TileLoader::Impl {
  explicit Impl(size_t thread_count) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    for (size_t i = 0; i < thread_count; ++i)
      workers.emplace_back([this] { WorkerLoop(); });
  }

  ~Impl() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stop = true;
    }
    cv.notify_all();
    for (auto &worker : workers)
      worker.join();
    // Join before tearing down curl globally: curl_global_cleanup must not
    // race with an in-flight curl_easy_perform.
    curl_global_cleanup();
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

TileLoader::TileLoader(size_t concurrency)
    : impl_(std::make_unique<Impl>(concurrency)) {}

TileLoader::~TileLoader() = default;

void TileLoader::Enqueue(TileRequest request) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->pending.push_back(std::move(request));
  }
  impl_->cv.notify_one();
}

std::vector<TileResult> TileLoader::DrainResults() {
  std::vector<TileResult> results;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  results.swap(impl_->completed);
  return results;
}

} // namespace pacer
