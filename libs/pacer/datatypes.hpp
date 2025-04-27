#pragma once

namespace pacer {

struct GPSSample {
  double lat, lon, altitude, full_speed, ground_speed;
};

} // namespace pacer