#pragma once

#include <cmath>
#include <cstddef>
#include <cstdio>

// Lap times are plain seconds, 75.234 rather than 1:15.234: on a kart
// circuit every lap sits in the same minute, so the minutes carry nothing and
// the seconds are what gets compared at a glance. NaN reads "--.---".
inline void FormatLapTime(char *buf, size_t n, double seconds) {
  if (std::isnan(seconds)) {
    snprintf(buf, n, "--.---");
    return;
  }
  snprintf(buf, n, "%.3f", seconds);
}
