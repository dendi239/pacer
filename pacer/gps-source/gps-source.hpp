#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <utility>

#include <pacer/datatypes/datatypes.hpp>

namespace pacer {

// Base class for raw GPS source.
//
// Being raw in this context means that it does not provide any meaningful
// timestamps to work with.
class RawGPSSource {
public:
  RawGPSSource() = default;
  virtual ~RawGPSSource() = default;

  // Main interface to take samples from current GPS source.
  //
  // Args:
  //   void *data:  associated data object with callback;
  //   on_sample:  void (*)(void *, GPSSample, size_t, size_t) callback
  //   function, takes following arguments:
  //     - data provided earlier;
  //     - sampled data;
  //     - index of current data;
  //     - total number of records in a batch.
  virtual uint32_t
  Samples(void *data, void (*on_sample)(void * /*data*/, GPSSample /*sample*/,
                                        size_t /*current_index*/,
                                        size_t /*total_records*/));

  // Convenient way of invoking Samples function: designed to be used with
  // functional objects (e.g. lambdas).
  template <class F> uint32_t Samples(F on_sample) {
    return Samples(&on_sample, [](void *data, GPSSample s, size_t i, size_t n) {
      auto &f = *reinterpret_cast<F *>(data);
      return f(s, i, n);
    });
  }

  uint32_t
  ReadSamples(std::function<void(GPSSample, uint32_t, uint32_t)> on_sample);

  // Seeks to data chunk covering target.
  virtual uint32_t Seek(double target) = 0;

  // Proceeds to next piece of data.
  virtual void Next() = 0;

  // Checks whenever we already reachend end of the stream.
  virtual bool IsEnd() = 0;

  // Returns current samples' time span.
  virtual auto CurrentTimeSpan() const -> std::pair<double, double> = 0;

  // Gets total MP4 duration.
  virtual double GetTotalDuration() const = 0;
};

// Handler for GPMF track inside MP4 container.
//
// Allows for traversing media file and getting GPS data out of it.
//
// TODO: Provide even more low-level access to underlying samples,
//       might be useful to keep buffer for data in some sort of iterator
//       with option to iterate over GPSSample-s on top of it.
class GPMFSource : public RawGPSSource {
public:
  explicit GPMFSource(size_t mp4handle);
  explicit GPMFSource(const char *filename);
  ~GPMFSource() noexcept;

  // Main interface to take samples from current handler.
  //
  // Args:
  //   void *data:  associated data object with callback;
  //   on_sample:  void (*)(void *, GPSSample, size_t, size_t) callback
  //   function, takes following arguments:
  //     - data provided earlier;
  //     - sampled data;
  //     - index of current data;
  //     - total number of records in a batch.
  uint32_t Samples(void *data,
                   void (*on_sample)(void * /*data*/, GPSSample /*sample*/,
                                     size_t /*current_index*/,
                                     size_t /*total_records*/)) override;

  // Seeks to data chunk covering target.
  uint32_t Seek(double target) override;

  // Proceeds to next piece of data.
  void Next() override;

  // Checks whenever we already reachend end of the stream.
  bool IsEnd() override;

  // Returns current samples' time span.
  std::pair<double, double> CurrentTimeSpan() const override;

  // Gets total MP4 duration.
  double GetTotalDuration() const override;

private:
  uint32_t index_ = 0;
  size_t mp4handle_;
};

class SequentialGPSSource : public RawGPSSource {
public:
  SequentialGPSSource(RawGPSSource *left, RawGPSSource *right)
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

  // Returns current samples' time span.
  std::pair<double, double> CurrentTimeSpan() const override;

private:
  RawGPSSource *left_, *right_, *current_;
};

enum class DatVersion {
  JUST_DATA = 0,
  WITH_TIMESTAMP = 1,
};

void ReadDatFile(const char *filename, void *data,
                 void (*on_sample)(GPSSample sample, double time, void *data),
                 DatVersion version);

template <typename F>
void ReadDatFile(const char *filename, F on_sample,
                 DatVersion version = DatVersion::JUST_DATA) {
  ReadDatFile(
      filename, &on_sample,
      [](GPSSample sample, double time, void *data) {
        auto &f = *reinterpret_cast<F *>(data);
        f(sample, time);
      },
      version);
}

} // namespace pacer
