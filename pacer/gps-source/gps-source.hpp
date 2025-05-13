#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <pacer/datatypes/datatypes.hpp>

namespace pacer {

class GPSSource {
public:
  GPSSource() = default;
  virtual ~GPSSource() = default;

  /// Main interface to take samples from current GPS source.
  ///
  /// Args:
  ///   void *data:  associated data object with callback;
  ///   on_sample:  void (*)(void *, GPSSample, size_t, size_t) callback
  ///   function, takes following arguments:
  ///     - data provided earlier;
  ///     - sampled data;
  ///     - index of current data;
  ///     - total number of records in a batch.
  virtual uint32_t
  Samples(void *data, void (*on_sample)(void * /*data*/, GPSSample /*sample*/,
                                        size_t /*current_index*/,
                                        size_t /*total_records*/)) = 0;

  /// Convenient way of invoking Samples function: designed to be used with
  /// functional objects (e.g. lambdas).
  template <class F> uint32_t Samples(F on_sample) {
    return Samples(&on_sample, [](void *data, GPSSample s, size_t i, size_t n) {
      auto &f = *reinterpret_cast<F *>(data);
      return f(s, i, n);
    });
  }

  /// Seeks to data chunk covering target.
  virtual uint32_t Seek(double target) = 0;

  /// Proceeds to next piece of data.
  virtual void Next() = 0;

  /// Checks whenever we already reachend end of the stream.
  virtual bool IsEnd() = 0;

  /// Returns current samples' time span.
  virtual auto CurrentTimeSpan() const -> std::pair<double, double> = 0;

  /// Gets total MP4 duration.
  virtual double GetTotalDuration() const = 0;
};

/// Handler for GPMF track inside MP4 container.
///
/// Allows for traversing media file and getting GPS data out of it.
///
/// TODO: Provide even more low-level access to underlying samples,
///       might be useful to keep buffer for data in some sort of iterator
///       with option to iterate over GPSSample-s on top of it.
class GPMFSource : public GPSSource {
public:
  explicit GPMFSource(size_t mp4handle);
  explicit GPMFSource(const char *filename);
  ~GPMFSource() noexcept;

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
                                     size_t /*total_records*/)) override;

  /// Seeks to data chunk covering target.
  uint32_t Seek(double target) override;

  /// Proceeds to next piece of data.
  void Next() override;

  /// Checks whenever we already reachend end of the stream.
  bool IsEnd() override;

  /// Returns current samples' time span.
  auto CurrentTimeSpan() const -> std::pair<double, double> override;

  /// Gets total MP4 duration.
  double GetTotalDuration() const override;

private:
  uint32_t index_ = 0;
  size_t mp4handle_;
};

class SequentialGPSSource : public GPSSource {
public:
  SequentialGPSSource(GPSSource *left, GPSSource *right)
      : left_{left}, right_{right}, current_{left_} {}

  virtual ~SequentialGPSSource() override = default;

  double GetTotalDuration() const override;

  bool IsEnd() override;

  uint32_t Samples(void *data,
                   void (*on_sample)(void * /*data*/, GPSSample /*sample*/,
                                     size_t /*current_index*/,
                                     size_t /*total_records*/)) override;

  uint32_t Seek(double target) override;

  void Next() override;

  /// Returns current samples' time span.
  auto CurrentTimeSpan() const -> std::pair<double, double> override;

private:
  GPSSource *left_, *right_, *current_;
};

} // namespace pacer
