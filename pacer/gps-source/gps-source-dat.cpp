#include "gps-source.hpp"

#include "ubx-nav-pvt.hpp"

void pacer::ReadDatFile(const char *filename, void *data,
                        void (*on_sample)(GPSSample sample, double time,
                                          void *data),
                        DatVersion version) {

  FILE *f = fopen(filename, "rb");

  int64_t timestamp;
  uGnssDecUbxNavPvt_t gps;

  while (!feof(f)) {
    if (version == DatVersion::WITH_TIMESTAMP)
      fread(&timestamp, sizeof(timestamp), 1, f);

    fread(&gps, sizeof(gps), 1, f);

    GPSSample s{
        .lat = static_cast<double>(gps.lat) / 1e7,
        .lon = static_cast<double>(gps.lon) / 1e7,
        .altitude = gps.height / 1000.0,     // mm to m
        .full_speed = gps.gSpeed / 1000.0,   // mm/s to m/s
        .ground_speed = gps.gSpeed / 1000.0, // mm/s to m/s
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
