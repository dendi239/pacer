#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "datatypes.hpp"

namespace pacer {

/// Handler for GPMF track inside MP4 container.
///
/// Allows for traversing media file and getting GPS data out of it.
///
/// TODO: Provide even more low-level access to underlying samples,
///       might be useful to keep buffer for data in some sort of iterator
///       with option to iterate over GPSSample-s on top of it.
class MovieHandler {
public:
  explicit MovieHandler(size_t mp4handle);
  explicit MovieHandler(const char *filename);
  ~MovieHandler() noexcept;

  /// Main interface to take samples from current handler.
  ///
  /// Args:
  ///   void *data:  associated data object with callback;
  ///   on_sample:  void (*)(void *, GPSSample, size_t, size_t) callback
  ///   function, takes following arguments:
  ///     - data provided earlier;
  ///     - sampled data;
  ///     - index of current data;
  ///     - total number of records in a batch.
  uint32_t Samples(void *data,
                   void (*on_sample)(void * /*data*/, GPSSample /*sample*/,
                                     size_t /*current_index*/,
                                     size_t /*total_records*/));

  /// Convenient way of invoking Samples function: designed to be used with
  /// functional objects (e.g. lambdas).
  template <class F> uint32_t Samples(F on_sample) {
    return Samples(&on_sample, [](void *data, GPSSample s, size_t i, size_t n) {
      auto &f = *reinterpret_cast<F *>(data);
      return f(s, i, n);
    });
  }

  /// Seeks to data chunk covering target.
  uint32_t Seek(double target);

  /// Proceeds to next piece of data.
  void Next();

  /// Checks whenever we already reachend end of the stream.
  bool IsEnd();

  /// Returns current samples' time span.
  auto CurrentTimeSpan() const -> std::pair<double, double>;

  /// Gets total MP4 duration.
  double GetTotalDuration() const;

private:
  uint32_t index_ = 0;
  size_t mp4handle_;
};

} // namespace pacer
