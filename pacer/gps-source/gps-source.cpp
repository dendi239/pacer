#include "gps-source.hpp"

#include <__chrono/calendar.h>
#include <chrono>
#include <cstdint>
#include <format>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <sys/types.h>

#include "GPMF_common.h"
#include "GPMF_parser.h"
#include "demo/GPMF_mp4reader.h"
#include "pacer/datatypes/datatypes.hpp"

namespace pacer {

GPMFSource::GPMFSource(const char *filename)
    : mp4handle_(OpenMP4Source(const_cast<char *>(filename), MOV_GPMF_TRAK_TYPE,
                               MOV_GPMF_TRAK_SUBTYPE, 0)) {
  if (mp4handle_ == 0) {
    throw std::runtime_error(
        (std::string("Failed to open file: ") + std::string(filename)).c_str());
  }
}

GPMFSource::~GPMFSource() noexcept {
  if (mp4handle_) {
    CloseSource(mp4handle_);
  }
}

GPMFSource::GPMFSource(size_t mp4handle) : mp4handle_(mp4handle) {}

uint32_t GPMFSource::Seek(double target) {
  double in, out;
  uint32_t ret = GPMF_OK;
  do {
    ret = GetPayloadTime(mp4handle_, index_, &in, &out);
    if (ret == GPMF_OK) {
      if (out <= target)
        ++index_;
      if (target < in)
        --index_;
    }
  } while (ret == GPMF_OK && in < out && ((target > out) || (target < in)));
  if (!(in < out)) {
    --index_;
  }

  return ret;
}

void GPMFSource::Next() { ++index_; }

bool GPMFSource::IsEnd() {
  double in, out;
  if (GetPayloadTime(mp4handle_, index_, &in, &out) != GPMF_OK) {
    return true;
  }
  return in + 1e-9 >= out;
}

std::pair<double, double> GPMFSource::CurrentTimeSpan() const {
  double in, out;
  GetPayloadTime(mp4handle_, index_, &in, &out);
  return {in, out};
}

double GPMFSource::GetTotalDuration() const { return GetDuration(mp4handle_); }

union GPSUData {
  struct {
    char y[2], m[2], d[2], h[2], min[2], s[2], _, ms[3];
  } str;
  char data[16];

  int64_t Timestamp() const {
    int64_t y = 2000 + (str.y[0] - '0') * 10 + (str.y[1] - '0');
    int64_t m = (str.m[0] - '0') * 10 + (str.m[1] - '0');
    int64_t d = (str.d[0] - '0') * 10 + (str.d[1] - '0');
    int64_t hour = (str.h[0] - '0') * 10 + (str.h[1] - '0');
    int64_t minute = (str.min[0] - '0') * 10 + (str.min[1] - '0');
    int64_t second = (str.s[0] - '0') * 10 + (str.s[1] - '0');
    int64_t milliseconds =
        (str.ms[0] - '0') * 100 + (str.ms[1] - '0') * 10 + (str.ms[2] - '0');

    // https://howardhinnant.github.io/date_algorithms.html
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400); // [0, 399]
    const unsigned doy =
        (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;        // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; // [0, 146096]
    int64_t days_since_1970 = era * 146097 + static_cast<int64_t>(doe) - 719468;

    return days_since_1970 * 24 * 60 * 60 * 1000 +
           1000 * (hour * 60 * 60 + minute * 60 + second) + milliseconds;
  }
};

uint32_t GPMFSource::Samples(void *data,
                             void (*on_sample)(void * /*data*/,
                                               GPSSample /*sample*/,
                                               size_t /*current_index*/,
                                               size_t /*total_records*/)) {
  uint32_t payloadsize = GetPayloadSize(mp4handle_, index_);
  size_t payloadres = 0;
  payloadres = GetPayloadResource(mp4handle_, payloadres, payloadsize);
  uint32_t *payload = GetPayload(mp4handle_, payloadres, index_);

  if (payload == nullptr) {
    printf("No payload\n");
    return 1;
  }

  GPMF_stream metadata_stream, *ms = &metadata_stream;
  auto ret = GPMF_Init(ms, payload, payloadsize);
  if (ret != GPMF_OK) {
    printf("No init\n");
    return ret;
  }

  while (GPMF_OK ==
         GPMF_FindNext(ms, STR2FOURCC("STRM"),
                       GPMF_LEVELS(GPMF_RECURSE_LEVELS | GPMF_TOLERANT))) {

    if (ret != GPMF_OK) {
      printf("No FindNext gps data\n");
      return ret;
    }

    ret = GPMF_SeekToSamples(ms);
    if (ret != GPMF_OK) {
      printf("No Seek to Samples\n");
      return ret;
    }

    char *rawdata = (char *)GPMF_RawData(ms);
    uint32_t key = GPMF_Key(ms);
    GPMF_SampleType type = GPMF_Type(ms);
    uint32_t samples = GPMF_Repeat(ms);
    uint32_t elements = GPMF_ElementsInStruct(ms);

    // printf("Key: %c%c%c%c, Samples: %d, Elements: %d\n", PRINTF_4CC(key),
    //        samples, elements);
    // Extract GPSU data from GPS5 stream
    // To do this, you need to search for the GPSU key at the same level as
    // GPS5. Typically, GPSU contains a timestamp for each GPS5 sample. You can
    // use GPMF_FindPrev or GPMF_FindNext to locate GPSU.

    // Example: Find GPSU at the same level as GPS5
    GPMF_stream gpsu_stream;

    if (key == STR2FOURCC("GPS9")) {
      // printf("Found GPS9 stream, skipping...\n");
      // lat, long, alt, 2D speed, 3D speed, days since 2000, secs since
      // midnight (ms precision), DOP, fix (0, 2D or 3D)
      //  Parse GPS9 data: lat, long, alt, 2D speed, 3D speed, days since 2000,
      //  secs since midnight (ms precision), DOP, fix
      if (samples) {
        uint32_t buffersize = samples * elements * sizeof(double);
        double *tmpbuffer = (double *)malloc(buffersize);
        if (tmpbuffer) {
          if (GPMF_OK == GPMF_ScaledData(ms, tmpbuffer, buffersize, 0, samples,
                                         GPMF_TYPE_DOUBLE)) {
            for (uint32_t i = 0; i < samples; ++i) {
              GPSSample gps{};
              // GPS9: [lat, lon, alt, 2D speed, 3D speed, days since 2000, secs
              // since midnight, DOP, fix]
              gps.lat = tmpbuffer[i * elements + 0];
              gps.lon = tmpbuffer[i * elements + 1];
              gps.altitude = tmpbuffer[i * elements + 2];
              gps.ground_speed = tmpbuffer[i * elements + 3];
              gps.full_speed = tmpbuffer[i * elements + 4];
              // Timestamp calculation from days since 2000 and seconds since
              // midnight
              double days_since_2000 = tmpbuffer[i * elements + 5];
              double secs_since_midnight = tmpbuffer[i * elements + 6];
              // Convert days since 2000-01-01 to ms since epoch
              constexpr int64_t epoch_2000 =
                  946684800000LL; // ms since epoch for 2000-01-01T00:00:00Z
              int64_t ms_since_2000 = static_cast<int64_t>(
                  days_since_2000 * 86400000.0 + secs_since_midnight * 1000.0);
              gps.timestamp_ms = epoch_2000 + ms_since_2000;
              // gps.dop = tmpbuffer[i * elements + 7];
              // gps.fix = static_cast<int>(tmpbuffer[i * elements + 8]);
              on_sample(data, gps, i, samples);
            }
          }
          free(tmpbuffer);
        }
      }
    }

    if (key == STR2FOURCC("GPS5")) {
      int64_t timestamp = 0;
      GPMF_CopyState(ms, &gpsu_stream);
      if (GPMF_OK ==
          GPMF_FindPrev(&gpsu_stream, STR2FOURCC("GPSU"),
                        GPMF_LEVELS(GPMF_CURRENT_LEVEL | GPMF_TOLERANT))) {
        char *gpsu_data = (char *)GPMF_RawData(&gpsu_stream);
        uint32_t gpsu_size =
            GPMF_Repeat(&gpsu_stream) * GPMF_ElementsInStruct(&gpsu_stream);

        // GPSU is usually a 16-byte ASCII timestamp per sample, e.g.
        // "2017-01-01 12:34:56.000" If there are multiple samples, gpsu_data
        // contains concatenated timestamps.
        for (uint32_t k = 0; k < gpsu_size; k += 16) {
          GPSUData t;
          memcpy(t.data, gpsu_data + k, 16);
          timestamp = t.Timestamp();
        }
      }

      if (samples) {
        uint32_t buffersize = samples * elements * sizeof(double);
        GPMF_stream find_stream;
        double *ptr, *tmpbuffer = (double *)malloc(buffersize);

#define MAX_UNITS 64
#define MAX_UNITLEN 8
        char units[MAX_UNITS][MAX_UNITLEN] = {""};
        uint32_t unit_samples = 1;

        char complextype[MAX_UNITS] = {""};
        uint32_t type_samples = 1;

        if (tmpbuffer) {
          uint32_t i, j;

          // Search for any units to display
          GPMF_CopyState(ms, &find_stream);
          if (GPMF_OK == GPMF_FindPrev(
                             &find_stream, GPMF_KEY_SI_UNITS,
                             GPMF_LEVELS(GPMF_CURRENT_LEVEL | GPMF_TOLERANT)) ||
              GPMF_OK == GPMF_FindPrev(
                             &find_stream, GPMF_KEY_UNITS,
                             GPMF_LEVELS(GPMF_CURRENT_LEVEL | GPMF_TOLERANT))) {
            char *data = (char *)GPMF_RawData(&find_stream);
            uint32_t ssize = GPMF_StructSize(&find_stream);
            if (ssize > MAX_UNITLEN - 1)
              ssize = MAX_UNITLEN - 1;
            unit_samples = GPMF_Repeat(&find_stream);

            for (i = 0; i < unit_samples && i < MAX_UNITS; i++) {
              memcpy(units[i], data, ssize);
              units[i][ssize] = 0;
              data += ssize;
            }
          }

          // GPMF_FormattedData(ms, tmpbuffer, buffersize, 0, samples); //
          // Output data in LittleEnd, but no scale
          if (GPMF_OK ==
              GPMF_ScaledData(ms, tmpbuffer, buffersize, 0, samples,
                              GPMF_TYPE_DOUBLE)) // Output scaled data as floats
          {
            ptr = tmpbuffer;
            int pos = 0;
            union {
              GPSSample gps;
              double values[5];
            } r;

            for (i = 0; i < samples; i++) {
              r.gps.timestamp_ms = timestamp;

              // printf("  %c%c%c%c (%d,%d) ", PRINTF_4CC(key), samples,
              // elements);

              for (j = 0; j < elements; j++) {
                if (type == GPMF_TYPE_STRING_ASCII) {
                  // printf("%c", rawdata[pos]);
                  pos++;
                  ++ptr;
                } else if (type_samples == 0) // no TYPE structure
                {
                  // printf("%.3f%s, ", *ptr, units[j % unit_samples]);
                  ++ptr;
                } else if (complextype[j] != 'F') {
                  r.values[j % unit_samples] = *ptr;
                  if ((j + 1) % unit_samples == 0) {
                    on_sample(data, r.gps, i, samples);
                  }

                  // printf("%.3f%s, ", *ptr, units[j % unit_samples]);
                  ++ptr;
                  pos += GPMF_SizeofType((GPMF_SampleType)complextype[j]);
                } else if (type_samples && complextype[j] == GPMF_TYPE_FOURCC) {
                  ptr++;

                  // printf("%c%c%c%c, ", rawdata[pos], rawdata[pos + 1],
                  //        rawdata[pos + 2], rawdata[pos + 3]);
                  pos += GPMF_SizeofType((GPMF_SampleType)complextype[j]);
                }
              }

              // printf("\n");
            }
          }
          free(tmpbuffer);
        }
      }
    }
  }

  return ret;
}

uint32_t SequentialGPSSource::Samples(
    void *data,
    void (*on_sample)(void * /*data*/, GPSSample /*sample*/,
                      size_t /*current_index*/, size_t /*total_records*/)) {
  return current_->Samples(data, on_sample);
}
bool SequentialGPSSource::IsEnd() {
  return current_ == right_ && current_->IsEnd();
}
double SequentialGPSSource::GetTotalDuration() const {
  return left_->GetTotalDuration() + right_->GetTotalDuration();
}
uint32_t SequentialGPSSource::Seek(double target) {
  if (auto left_duration = left_->GetTotalDuration(); target < left_duration) {
    if (current_ == right_)
      right_->Seek(0);
    return (current_ = left_)->Seek(target);
  } else {
    target -= left_->GetTotalDuration();
    return (current_ = right_)->Seek(target);
  }
}
void SequentialGPSSource::Next() {
  if (current_ == left_ && current_->IsEnd())
    current_ = right_;
  return current_->Next();
}
auto SequentialGPSSource::CurrentTimeSpan() const -> std::pair<double, double> {
  auto [start, end] = current_->CurrentTimeSpan();
  if (current_ == right_) {
    auto left_len = left_->GetTotalDuration();
    return {start + left_len, end + left_len};
  }
  return {start, end};
}
uint32_t RawGPSSource::Samples(void *data,
                               void (*on_sample)(void * /*data*/,
                                                 GPSSample /*sample*/,
                                                 size_t /*current_index*/,
                                                 size_t /*total_records*/)) {
  return 0;
};

uint32_t RawGPSSource::ReadSamples(
    std::function<void(GPSSample, uint32_t, uint32_t)> on_sample) {
  return Samples(&on_sample, [](void *data, GPSSample sample,
                                size_t current_index, size_t total_records) {
    auto &f =
        *reinterpret_cast<std::function<void(GPSSample, uint32_t, uint32_t)> *>(
            data);
    f(sample, current_index, total_records);
  });
}
} // namespace pacer
