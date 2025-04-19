#pragma once

namespace pacer {

struct GPSSample {
  double lat, lon, altitude, ground_speed, full_speed;
};

} // namespace pacer