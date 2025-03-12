/*! @file GPMF_demo.c
 *
 *  @brief Demo to extract GPMF from an MP4
 *
 *  @version 2.5.0
 *
 *  (C) Copyright 2017-2020 GoPro Inc (http://gopro.com/).
 *
 *  Licensed under either:
 *  - Apache License, Version 2.0, http://www.apache.org/licenses/LICENSE-2.0
 *  - MIT license, http://opensource.org/licenses/MIT
 *  at your option.
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GPMF_common.h"
#include "GPMF_parser.h"
#include "GPMF_utils.h"
#include "demo/GPMF_mp4reader.h"

#define SHOW_VIDEO_FRAMERATE 1
#define SHOW_PAYLOAD_TIME 1
#define SHOW_ALL_PAYLOADS 0
#define SHOW_GPMF_STRUCTURE 0
#define SHOW_PAYLOAD_INDEX 0
#define SHOW_SCALED_DATA 1
#define SHOW_THIS_FOUR_CC STR2FOURCC("ACCL")
#define SHOW_COMPUTED_SAMPLERATES 1

#ifdef __cplusplus
extern "C" {
#endif

void PrintGPMF(GPMF_stream *);

#ifdef __cplusplus
}
#endif

void printHelp(char *name) {
  printf("usage: %s <file_with_GPMF> <optional features>\n", name);
  printf("       -a - %s all payloads\n",
         SHOW_ALL_PAYLOADS ? "disable" : "show");
  printf("       -g - %s GPMF structure\n",
         SHOW_GPMF_STRUCTURE ? "disable" : "show");
  printf("       -i - %s index of the payload\n",
         SHOW_PAYLOAD_INDEX ? "disable" : "show");
  printf("       -s - %s scaled data\n", SHOW_SCALED_DATA ? "disable" : "show");
  printf("       -c - %s computed sample rates\n",
         SHOW_COMPUTED_SAMPLERATES ? "disable" : "show");
  printf("       -v - %s video framerate\n",
         SHOW_VIDEO_FRAMERATE ? "disable" : "show");
  printf("       -t - %s time of the payload\n",
         SHOW_PAYLOAD_TIME ? "disable" : "show");
  printf("       -fWXYZ - show only this fourCC , e.g. -f%c%c%c%c (default) "
         "just -f for all\n",
         PRINTF_4CC(SHOW_THIS_FOUR_CC));
  printf("       \n");
  printf("       ver 2.0\n");
}

uint32_t show_all_payloads = SHOW_ALL_PAYLOADS;
uint32_t show_gpmf_structure = SHOW_GPMF_STRUCTURE;
uint32_t show_payload_index = SHOW_PAYLOAD_INDEX;
uint32_t show_scaled_data = SHOW_SCALED_DATA;
uint32_t show_computed_samplerates = SHOW_COMPUTED_SAMPLERATES;
uint32_t show_video_framerate = SHOW_VIDEO_FRAMERATE;
uint32_t show_payload_time = SHOW_PAYLOAD_TIME;
uint32_t show_this_four_cc = 0;

GPMF_ERR readMP4File(char *filename);

int main(int argc, char *argv[]) {
  GPMF_ERR ret = GPMF_OK;

  show_this_four_cc = SHOW_THIS_FOUR_CC;

  // get file return data
  if (argc < 2) {
    printHelp(argv[0]);
    return -1;
  }

  for (int i = 2; i < argc; i++) {
    if (argv[i][0] == '-') // feature switches
    {
      switch (argv[i][1]) {
      case 'a':
        show_all_payloads ^= 1;
        break;
      case 'g':
        show_gpmf_structure ^= 1;
        break;
      case 'i':
        show_payload_index ^= 1;
        break;
      case 's':
        show_scaled_data ^= 1;
        break;
      case 'c':
        show_computed_samplerates ^= 1;
        break;
      case 'v':
        show_video_framerate ^= 1;
        break;
      case 't':
        show_payload_time ^= 1;
        break;
      case 'f':
        show_this_four_cc = STR2FOURCC((&(argv[i][2])));
        break;
      case 'h':
        printHelp(argv[0]);
        break;
      }
    }
  }

  ret = readMP4File(argv[1]);

  printf("\n");
  return ret;
}

GPMF_ERR readMP4File(char *filename) {
  GPMF_ERR ret = GPMF_OK;
  GPMF_stream metadata_stream = {0}, *ms = &metadata_stream;
  double metadatalength;
  uint32_t *payload = NULL;
  uint32_t payloadsize = 0;
  size_t payloadres = 0;
#if 1 // Search for GPMF Track
  size_t mp4handle =
      OpenMP4Source(filename, MOV_GPMF_TRAK_TYPE, MOV_GPMF_TRAK_SUBTYPE, 0);
#else // look for a global GPMF payload in the moov header, within 'udta'
  size_t mp4handle =
      OpenMP4SourceUDTA(argv[1], 0); // Search for GPMF payload with MP4's udta
#endif
  if (mp4handle == 0) {
    printf("error: %s is an invalid MP4/MOV or it has no GPMF data\n\n",
           filename);
    return GPMF_ERROR_BAD_STRUCTURE;
  }

  metadatalength = GetDuration(mp4handle);

  if (metadatalength > 0.0) {
    uint32_t index, payloads = GetNumberPayloads(mp4handle);
    //		printf("found %.2fs of metadata, from %d payloads, within %s\n",
    // metadatalength, payloads, argv[1]);

    uint32_t fr_num, fr_dem;
    uint32_t frames = GetVideoFrameRateAndCount(mp4handle, &fr_num, &fr_dem);
    if (show_video_framerate) {
      if (frames) {
        printf("VIDEO FRAMERATE:\n  %.3f with %d frames\n",
               (float)fr_num / (float)fr_dem, frames);
      }
    }

    for (index = 0; index < payloads; index++) {
      double in = 0.0, out = 0.0; // times
      payloadsize = GetPayloadSize(mp4handle, index);
      payloadres = GetPayloadResource(mp4handle, payloadres, payloadsize);
      payload = GetPayload(mp4handle, payloadres, index);
      if (payload == NULL) {
        printf("payload == NULL");
        goto cleanup;
      }

      ret = GetPayloadTime(mp4handle, index, &in, &out);
      if (ret != GPMF_OK) {
        printf("Payload time is not OK: %d\n", ret);
        goto cleanup;
      }

      if (index <= 1) {
        printf("Time: %.2f to %.2f\n", in, out);
      }

      ret = GPMF_Init(ms, payload, payloadsize);
      if (ret != GPMF_OK) {
        printf("Couldn't init stream: %d\n", ret);
        goto cleanup;
      }

      if (show_payload_time && index <= 1)
        if (show_gpmf_structure || show_payload_index || show_scaled_data)
          if (show_all_payloads)
            printf("PAYLOAD TIME:\n  %.3f to %.3f seconds\n", in, out);

      if (show_gpmf_structure && index <= 1) {
        if (show_all_payloads) {

          printf("GPMF STRUCTURE:\n");
          // Output (printf) all the contained GPMF data within this payload
          ret = GPMF_Validate(ms, GPMF_RECURSE_LEVELS); // optional
          if (GPMF_OK != ret) {
            if (GPMF_ERROR_UNKNOWN_TYPE == ret) {
              printf("Unknown GPMF Type within, ignoring\n");
              ret = GPMF_OK;
            } else {

              printf("Invalid GPMF Structure\n");
            }
          }

          GPMF_ResetState(ms);

          GPMF_ERR nextret;
          do {
            {
              printf("  ");
              PrintGPMF(ms); // printf current GPMF KLV
            }

            nextret =
                GPMF_Next(ms, GPMF_LEVELS(GPMF_RECURSE_LEVELS | GPMF_TOLERANT));

            while (
                nextret ==
                GPMF_ERROR_UNKNOWN_TYPE) // or just using GPMF_Next(ms,
                                         // GPMF_RECURSE_LEVELS|GPMF_TOLERANT)
                                         // to ignore and skip unknown types
              nextret = GPMF_Next(ms, GPMF_RECURSE_LEVELS);

          } while (GPMF_OK == nextret);
          GPMF_ResetState(ms);
        }
      }

      if (show_payload_index && index <= 1) {
        if (show_all_payloads) {

          printf("PAYLOAD INDEX:\n");
          ret = GPMF_FindNext(ms, GPMF_KEY_STREAM,
                              GPMF_LEVELS(GPMF_RECURSE_LEVELS | GPMF_TOLERANT));
          if (ret != GPMF_OK) {
            printf("Couldn't read next stream: %d\n", ret);
          }
          while (GPMF_OK == ret) {
            ret = GPMF_SeekToSamples(ms);
            if (ret != GPMF_OK) {
              printf("Couldn't seek to samples: %d\n", ret);
            }
            if (GPMF_OK == ret) // find the last FOURCC within the stream
            {
              uint32_t key = GPMF_Key(ms);
              GPMF_SampleType type = GPMF_Type(ms);
              uint32_t elements = GPMF_ElementsInStruct(ms);
              // uint32_t samples = GPMF_Repeat(ms);
              uint32_t samples = GPMF_PayloadSampleCount(ms);

              if (samples) {

                printf("  STRM of %c%c%c%c ", PRINTF_4CC(key));

                if (type == GPMF_TYPE_COMPLEX) {
                  GPMF_stream find_stream;
                  GPMF_CopyState(ms, &find_stream);

                  if (GPMF_OK == GPMF_FindPrev(&find_stream, GPMF_KEY_TYPE,
                                               GPMF_LEVELS(GPMF_CURRENT_LEVEL |
                                                           GPMF_TOLERANT))) {
                    char tmp[64];
                    char *data = (char *)GPMF_RawData(&find_stream);
                    uint32_t size = GPMF_RawDataSize(&find_stream);

                    if (size < sizeof(tmp)) {
                      memcpy(tmp, data, size);
                      tmp[size] = 0;

                      printf("of type %s ", tmp);
                    }
                  }

                } else {

                  printf("of type %c ", type);
                }

                printf("with %d sample%s ", samples, samples > 1 ? "s" : "");

                if (elements > 1)
                  printf("-- %d elements per sample", elements);

                printf("\n");
              }

              ret = GPMF_FindNext(
                  ms, GPMF_KEY_STREAM,
                  GPMF_LEVELS(GPMF_RECURSE_LEVELS | GPMF_TOLERANT));
            } else {
              if (ret !=
                  GPMF_OK) // some payload element was corrupt, skip to the next
                           // valid GPMF KLV at the previous level.
              {
                printf("Some elements were corrupt: %d\n", ret);
                ret = GPMF_Next(
                    ms, GPMF_CURRENT_LEVEL); // this will be the next stream if
                                             // any more are present.
                if (ret != GPMF_OK) {
                  break; // skip to the next payload as this one is corrupt
                }
              }
            }
          }
          GPMF_ResetState(ms);
        }
      }

      if (show_scaled_data && index <= 1) {
        if (show_all_payloads) {

          printf("SCALED DATA:\n");
          while (
              GPMF_OK ==
              GPMF_FindNext(
                  ms, STR2FOURCC("STRM"),
                  GPMF_LEVELS(GPMF_RECURSE_LEVELS |
                              GPMF_TOLERANT))) // GoPro Hero5/6/7 Accelerometer)
          {
            if (GPMF_VALID_FOURCC(show_this_four_cc)) {
              if (GPMF_OK != GPMF_FindNext(ms, show_this_four_cc,
                                           GPMF_LEVELS(GPMF_RECURSE_LEVELS |
                                                       GPMF_TOLERANT)))
                continue;
            } else {
              ret = GPMF_SeekToSamples(ms);
              if (GPMF_OK != ret)
                continue;
            }

            char *rawdata = (char *)GPMF_RawData(ms);
            uint32_t key = GPMF_Key(ms);
            GPMF_SampleType type = GPMF_Type(ms);
            uint32_t samples = GPMF_Repeat(ms);
            uint32_t elements = GPMF_ElementsInStruct(ms);

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
                if (GPMF_OK == GPMF_FindPrev(&find_stream, GPMF_KEY_SI_UNITS,
                                             GPMF_LEVELS(GPMF_CURRENT_LEVEL |
                                                         GPMF_TOLERANT)) ||
                    GPMF_OK == GPMF_FindPrev(&find_stream, GPMF_KEY_UNITS,
                                             GPMF_LEVELS(GPMF_CURRENT_LEVEL |
                                                         GPMF_TOLERANT))) {
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

                // Search for TYPE if Complex
                GPMF_CopyState(ms, &find_stream);
                type_samples = 0;
                if (GPMF_OK == GPMF_FindPrev(&find_stream, GPMF_KEY_TYPE,
                                             GPMF_LEVELS(GPMF_CURRENT_LEVEL |
                                                         GPMF_TOLERANT))) {
                  char *data = (char *)GPMF_RawData(&find_stream);
                  uint32_t ssize = GPMF_StructSize(&find_stream);
                  if (ssize > MAX_UNITLEN - 1)
                    ssize = MAX_UNITLEN - 1;
                  type_samples = GPMF_Repeat(&find_stream);

                  for (i = 0; i < type_samples && i < MAX_UNITS; i++) {
                    complextype[i] = data[i];
                  }
                }

                // GPMF_FormattedData(ms, tmpbuffer, buffersize, 0, samples); //
                // Output data in LittleEnd, but no scale
                if (GPMF_OK ==
                    GPMF_ScaledData(
                        ms, tmpbuffer, buffersize, 0, samples,
                        GPMF_TYPE_DOUBLE)) // Output scaled data as floats
                {

                  ptr = tmpbuffer;
                  int pos = 0;
                  for (i = 0; i < samples; i++) {

                    printf("  %c%c%c%c ", PRINTF_4CC(key));

                    for (j = 0; j < elements; j++) {
                      if (type == GPMF_TYPE_STRING_ASCII) {

                        printf("%c", rawdata[pos]);
                        pos++;
                        ptr++;
                      } else if (type_samples == 0) // no TYPE structure
                      {

                        printf("%.3f%s, ", *ptr++, units[j % unit_samples]);
                      } else if (complextype[j] != 'F') {

                        printf("%.3f%s, ", *ptr++, units[j % unit_samples]);
                        pos += GPMF_SizeofType((GPMF_SampleType)complextype[j]);
                      } else if (type_samples &&
                                 complextype[j] == GPMF_TYPE_FOURCC) {
                        ptr++;

                        printf("%c%c%c%c, ", rawdata[pos], rawdata[pos + 1],
                               rawdata[pos + 2], rawdata[pos + 3]);
                        pos += GPMF_SizeofType((GPMF_SampleType)complextype[j]);
                      }
                    }

                    printf("\n");
                  }
                }
                free(tmpbuffer);
              }
            }
          }
          GPMF_ResetState(ms);
        }
      }
    }

    if (show_computed_samplerates) {
      mp4callbacks cbobject;
      cbobject.mp4handle = mp4handle;
      cbobject.cbGetNumberPayloads = GetNumberPayloads;
      cbobject.cbGetPayload = GetPayload;
      cbobject.cbGetPayloadSize = GetPayloadSize;
      cbobject.cbGetPayloadResource = GetPayloadResource;
      cbobject.cbGetPayloadTime = GetPayloadTime;
      cbobject.cbFreePayloadResource = FreePayloadResource;
      cbobject.cbGetEditListOffsetRationalTime = GetEditListOffsetRationalTime;

      printf("COMPUTED SAMPLERATES:\n");
      // Find all the available Streams and compute they sample rates
      while (GPMF_OK ==
             GPMF_FindNext(ms, GPMF_KEY_STREAM,
                           GPMF_LEVELS(GPMF_RECURSE_LEVELS | GPMF_TOLERANT))) {
        if (GPMF_OK ==
            GPMF_SeekToSamples(ms)) // find the last FOURCC within the stream
        {
          double start, end;
          uint32_t fourcc = GPMF_Key(ms);

          double rate = GetGPMFSampleRate(cbobject, fourcc, STR2FOURCC("SHUT"),
                                          GPMF_SAMPLE_RATE_PRECISE, &start,
                                          &end); // GPMF_SAMPLE_RATE_FAST);

          printf("  %c%c%c%c sampling rate = %fHz (time %f to %f)\",\n",
                 PRINTF_4CC(fourcc), rate, start, end);
        }
      }
    }

  cleanup:
    if (payloadres)
      FreePayloadResource(mp4handle, payloadres);
    if (ms)
      GPMF_Free(ms);
    CloseSource(mp4handle);
  }

  if (ret != GPMF_OK) {
    if (GPMF_ERROR_UNKNOWN_TYPE == ret)
      printf("Unknown GPMF Type within\n");
    else
      printf("GPMF data has corruption\n");
  } else {
    ret = GPMF_OK; // when fuzzing, errors reported are showing the system is
                   // working.
  }

  return ret;
}
