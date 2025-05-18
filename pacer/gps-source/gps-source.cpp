#include "gps-source.hpp"

#include <stdexcept>
#include <string>

#include "GPMF_common.h"
#include "GPMF_parser.h"
#include "demo/GPMF_mp4reader.h"

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

    if (key != STR2FOURCC("GPS5")) {
      continue;
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
            // printf("  %c%c%c%c ", PRINTF_4CC(key));

            for (j = 0; j < elements; j++) {
              if (type == GPMF_TYPE_STRING_ASCII) {

                // printf("%c", rawdata[pos]);
                pos++;
                ptr++;
              } else if (type_samples == 0) // no TYPE structure
              {

                // printf("%.3f%s, ", *ptr++, units[j % unit_samples]);
                ptr++;
              } else if (complextype[j] != 'F') {
                r.values[j % unit_samples] = *ptr;
                if ((j + 1) % unit_samples == 0) {
                  on_sample(data, r.gps, i, samples);
                }

                // printf("%.3f%s, ", *ptr++, units[j % unit_samples]);
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
} // namespace pacer
