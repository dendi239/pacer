#include "gps-source.hpp"

#include "ubx-nav-pvt.hpp"

void pacer::ReadDatFile(const char *filename, void *data,
                        void (*on_sample)(GPSSample sample, double time,
                                          void *data),
                        DatVersion version) {

  FILE *f = fopen(filename, "rb");
  if (f == nullptr) {
    return;
  }

  int64_t timestamp;
  uGnssDecUbxNavPvt_t gps;

  // Reads are what end the loop, not feof: feof only reports a read that has
  // already failed, so testing it first lets one more iteration through on a
  // whole-record file, and that iteration emits `gps` unchanged -- a verbatim
  // duplicate of the last sample, one that a lap splitter then sees as a
  // zero-length interval sitting on the final fix.
  while (true) {
    if (version == DatVersion::WITH_TIMESTAMP &&
        fread(&timestamp, sizeof(timestamp), 1, f) != 1) {
      break;
    }
    if (fread(&gps, sizeof(gps), 1, f) != 1) {
      break;
    }

    GPSSample s{
        .lat = static_cast<double>(gps.lat) / 1e7,
        .lon = static_cast<double>(gps.lon) / 1e7,
        .altitude = gps.height / 1000.0,     // mm to m
        .full_speed = gps.gSpeed / 1000.0,   // mm/s to m/s
        .ground_speed = gps.gSpeed / 1000.0, // mm/s to m/s
        .vel_n = gps.velN / 1000.0,          // mm/s to m/s
        .vel_e = gps.velE / 1000.0,
        .vel_d = gps.velD / 1000.0,
        .h_acc = gps.hAcc / 1000.0, // mm to m
        .s_acc = gps.sAcc / 1000.0,
        .timestamp_ms = gps.iTOW,
    };

    if (on_sample) {
      if (version == DatVersion::WITH_TIMESTAMP) {
        on_sample(s, gps.iTOW / 1e3, data);
      } else {
        double timestamp = gps.iTOW / 1e3;
        on_sample(s, timestamp, data);
      }
    }
  }

  fclose(f);
}
