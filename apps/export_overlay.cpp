// Frame-table exporter: turns a recorded session into one row per video
// frame, which is what an overlay renderer consumes.
//
//   export_overlay --track <track.json> --out <frames.csv> [options] <input>...
//
// The overlay pipeline splits in two: this works out what the numbers are at
// every video frame, and a renderer downstream decides what they look like.
// Keeping the split means restyling never re-runs the timing engine, and the
// numbers burned into a video are the ones the dashboard and the firmware
// produce -- they come out of the same LiveTiming instance, not out of a
// second implementation written for the video.
//
// Two deltas are emitted, because they answer different questions:
//
//   delta_live_s  what the driver saw: LiveTiming's running delta to the best
//                 lap *so far*, stepping at each gate, and empty whenever the
//                 tracker was not confident in it (see delta_valid, lost).
//   delta_best_s  what the session turned out to be: the same measurement
//                 against the best lap of the whole session, interpolated
//                 between gates so it moves every frame rather than every
//                 metre. Only defined inside a lap the splitter timed.
//
// A row is emitted for every frame in the requested range even where the
// recording does not cover it, because a renderer needs one row per video
// frame to stay in step with the picture. Uncovered fields are left empty
// rather than zeroed: an overlay has to be able to tell "no reading" from a
// kart sitting on the start line at a standstill.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <pacer/geometry/geometry.hpp>
#include <pacer/gps-source/gps-source.hpp>
#include <pacer/laps/laps.hpp>
#include <pacer/live-timing/live-timing.hpp>
#include <pacer/reference-track/reference-track.hpp>

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Below this a heading derived from the velocity vector is noise, so the last
// one that meant something is carried instead: a marker that spins on the
// spot while the kart sits in the pits reads as a bug in the overlay.
constexpr double kHeadingMinSpeedMps = 0.5;

//---------------------------------- FPS -----------------------------------//

struct Fps {
  long num = 30;
  long den = 1;

  double Value() const {
    return static_cast<double>(num) / static_cast<double>(den);
  }
};

// Accepts "60000/1001", "59.94" and "60". The NTSC rates are why this is not
// just a double: 59.94 taken literally drifts a frame against real 59.94 fps
// footage every seven minutes, which over a race stint is seconds of lag
// between the marker and the picture.
bool ParseFps(const char *text, Fps *out) {
  if (const char *slash = std::strchr(text, '/')) {
    char *end = nullptr;
    long num = std::strtol(text, &end, 10);
    if (end != slash || num <= 0) {
      return false;
    }
    long den = std::strtol(slash + 1, &end, 10);
    if (*end != '\0' || den <= 0) {
      return false;
    }
    *out = Fps{num, den};
    return true;
  }

  char *end = nullptr;
  double value = std::strtod(text, &end);
  if (*end != '\0' || !(value > 0)) {
    return false;
  }
  for (long thousands : {24000L, 30000L, 60000L, 120000L}) {
    if (std::abs(value - static_cast<double>(thousands) / 1001.0) < 0.01) {
      *out = Fps{thousands, 1001};
      return true;
    }
  }
  if (value == std::floor(value)) {
    *out = Fps{static_cast<long>(value), 1};
    return true;
  }
  *out = Fps{std::lround(value * 1000.0), 1000};
  return true;
}

//--------------------------------- OUTPUT ---------------------------------//

enum class Format { kCsv, kNdjson };

// Every reading is a double carrying NaN for "absent", and the two flags an
// int carrying -1 for the same, so one writer handles a covered frame and an
// uncovered one without either of them growing a special case.
struct Row {
  int64_t frame = 0;
  double t_s = kNaN;
  double lat = kNaN, lon = kNaN;
  double x_m = kNaN, y_m = kNaN;
  double heading_rad = kNaN;
  double speed_mps = kNaN;
  double lap_number = kNaN;
  double current_lap_s = kNaN;
  double last_lap_s = kNaN;
  double best_lap_s = kNaN;
  double session_remaining_s = kNaN;
  double delta_live_s = kNaN;
  double delta_best_s = kNaN;
  int delta_valid = -1;
  int lost = -1;
  double gate = kNaN;
  double gate_count = kNaN;
};

const char *const kHeader =
    "frame,t_s,lat,lon,x_m,y_m,heading_rad,speed_mps,lap_number,"
    "current_lap_s,last_lap_s,best_lap_s,session_remaining_s,"
    "delta_live_s,delta_best_s,delta_valid,lost,gate,gate_count\n";

void AppendCsv(std::string &out, double value, int precision) {
  if (std::isfinite(value)) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f", precision, value);
    out += buf;
  }
  out += ',';
}

void AppendCsvFlag(std::string &out, int value) {
  if (value >= 0) {
    out += value ? '1' : '0';
  }
  out += ',';
}

void WriteCsvRow(std::FILE *f, const Row &r) {
  std::string line;
  line.reserve(224);
  char head[32];
  std::snprintf(head, sizeof head, "%lld,", static_cast<long long>(r.frame));
  line += head;
  AppendCsv(line, r.t_s, 6);
  AppendCsv(line, r.lat, 7);
  AppendCsv(line, r.lon, 7);
  AppendCsv(line, r.x_m, 3);
  AppendCsv(line, r.y_m, 3);
  AppendCsv(line, r.heading_rad, 5);
  AppendCsv(line, r.speed_mps, 3);
  AppendCsv(line, r.lap_number, 0);
  AppendCsv(line, r.current_lap_s, 3);
  AppendCsv(line, r.last_lap_s, 3);
  AppendCsv(line, r.best_lap_s, 3);
  AppendCsv(line, r.session_remaining_s, 3);
  AppendCsv(line, r.delta_live_s, 4);
  AppendCsv(line, r.delta_best_s, 4);
  AppendCsvFlag(line, r.delta_valid);
  AppendCsvFlag(line, r.lost);
  AppendCsv(line, r.gate, 2);
  AppendCsv(line, r.gate_count, 0);
  line.back() = '\n';
  std::fwrite(line.data(), 1, line.size(), f);
}

void AppendJson(std::string &out, const char *name, double value,
                int precision) {
  out += '"';
  out += name;
  out += "\":";
  if (std::isfinite(value)) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f", precision, value);
    out += buf;
  } else {
    // JSON has no NaN literal, so an absent reading has to be null anyway --
    // which is also what pandas and JavaScript both read back as missing.
    out += "null";
  }
  out += ',';
}

void AppendJsonFlag(std::string &out, const char *name, int value) {
  out += '"';
  out += name;
  out += "\":";
  out += value < 0 ? "null" : (value ? "true" : "false");
  out += ',';
}

void WriteJsonRow(std::FILE *f, const Row &r) {
  std::string line;
  line.reserve(320);
  char head[48];
  std::snprintf(head, sizeof head, "{\"frame\":%lld,",
                static_cast<long long>(r.frame));
  line += head;
  AppendJson(line, "t_s", r.t_s, 6);
  AppendJson(line, "lat", r.lat, 7);
  AppendJson(line, "lon", r.lon, 7);
  AppendJson(line, "x_m", r.x_m, 3);
  AppendJson(line, "y_m", r.y_m, 3);
  AppendJson(line, "heading_rad", r.heading_rad, 5);
  AppendJson(line, "speed_mps", r.speed_mps, 3);
  AppendJson(line, "lap_number", r.lap_number, 0);
  AppendJson(line, "current_lap_s", r.current_lap_s, 3);
  AppendJson(line, "last_lap_s", r.last_lap_s, 3);
  AppendJson(line, "best_lap_s", r.best_lap_s, 3);
  AppendJson(line, "session_remaining_s", r.session_remaining_s, 3);
  AppendJson(line, "delta_live_s", r.delta_live_s, 4);
  AppendJson(line, "delta_best_s", r.delta_best_s, 4);
  AppendJsonFlag(line, "delta_valid", r.delta_valid);
  AppendJsonFlag(line, "lost", r.lost);
  AppendJson(line, "gate", r.gate, 2);
  AppendJson(line, "gate_count", r.gate_count, 0);
  line.back() = '}';
  line += '\n';
  std::fwrite(line.data(), 1, line.size(), f);
}

//---------------------------- OFFLINE LAP TIMES ---------------------------//

// One lap as the offline splitter sees it, projected onto the reference
// track's densified gates: gate_ms[k] is when the kart crossed gate k. This
// is the same projection the timeline's comparisons are built on, so a delta
// drawn on the video and one read off the timeline agree.
struct TimedLap {
  int64_t start_ms = 0;
  int64_t end_ms = 0;
  double lap_time_s = 0;
  bool complete = false;
  std::vector<int64_t> gate_ms;
};

std::vector<TimedLap> BuildTimedLaps(const pacer::Laps &laps,
                                     const pacer::ReferenceTrack &track,
                                     size_t gate_count) {
  std::vector<TimedLap> timed_laps;
  timed_laps.reserve(laps.LapsCount());

  for (size_t i = 0; i < laps.LapsCount(); ++i) {
    pacer::Lap lap = laps.GetLap(i);
    if (lap.points.empty()) {
      continue;
    }
    pacer::Lap resampled = track.Resample(lap);

    TimedLap timed;
    timed.start_ms = lap.points.front().timestamp_ms;
    timed.end_ms = lap.points.back().timestamp_ms;
    timed.lap_time_s = laps.LapTime(i);
    // Resample() appends one closing point past the last gate only when the
    // lap covered every one of them, which is exactly the condition for the
    // lap being comparable end to end -- and so for it being allowed to
    // become the reference every other lap is measured against.
    timed.complete = resampled.points.size() > gate_count;

    // Keep the closing point Resample() appends past the last gate: that gate
    // stops a metre short of the start line, so truncating there would leave
    // the final metre of every lap with a reference time pinned to the last
    // gate -- and a delta that ramps by however long that metre took.
    size_t crossed = std::min(resampled.points.size(), gate_count + 1);
    timed.gate_ms.reserve(crossed);
    for (size_t k = 0; k < crossed; ++k) {
      timed.gate_ms.push_back(resampled.points[k].timestamp_ms);
    }
    // Densified gates converge and cross over each other on the inside of a
    // bend, so a lap on the inside line can meet two of them out of order and
    // leave a pair of crossing times a millisecond backwards. Clamping to
    // non-decreasing costs nothing at that scale and is what lets the lookup
    // below stay a binary search.
    for (size_t k = 1; k < timed.gate_ms.size(); ++k) {
      timed.gate_ms[k] = std::max(timed.gate_ms[k], timed.gate_ms[k - 1]);
    }

    timed_laps.push_back(std::move(timed));
  }
  return timed_laps;
}

// The lap covering `t_ms`, or null between laps (the out lap, the pits, and
// whatever the splitter could not time).
const TimedLap *LapAt(const std::vector<TimedLap> &laps, int64_t t_ms) {
  auto it = std::upper_bound(
      laps.begin(), laps.end(), t_ms,
      [](int64_t t, const TimedLap &lap) { return t < lap.start_ms; });
  if (it == laps.begin()) {
    return nullptr;
  }
  --it;
  return t_ms <= it->end_ms ? &*it : nullptr;
}

// Fractional gate index at `t_ms` -- how far round the track the kart is,
// on the same scale for every lap, which is what makes two laps comparable
// at a point in space rather than at a point in time.
double GateProgress(const TimedLap &lap, int64_t t_ms) {
  const std::vector<int64_t> &gates = lap.gate_ms;
  if (gates.size() < 2) {
    return kNaN;
  }
  if (t_ms <= gates.front()) {
    return 0.0;
  }
  if (t_ms >= gates.back()) {
    return static_cast<double>(gates.size() - 1);
  }
  auto it = std::upper_bound(gates.begin(), gates.end(), t_ms);
  size_t k = static_cast<size_t>(it - gates.begin()) - 1;
  int64_t span = gates[k + 1] - gates[k];
  double u = span > 0 ? static_cast<double>(t_ms - gates[k]) /
                            static_cast<double>(span)
                      : 0.0;
  return static_cast<double>(k) + u;
}

// How long `lap` took to reach `progress`, in seconds from its own start.
// NaN past where the lap reached, so a reference that ran short blanks the
// delta rather than inventing the rest of it.
double TimeAtProgress(const TimedLap &lap, double progress) {
  const std::vector<int64_t> &gates = lap.gate_ms;
  if (gates.empty() || !std::isfinite(progress) || progress < 0) {
    return kNaN;
  }
  if (progress >= static_cast<double>(gates.size() - 1)) {
    return progress > static_cast<double>(gates.size() - 1)
               ? kNaN
               : static_cast<double>(gates.back() - gates.front()) / 1000.0;
  }
  size_t k = static_cast<size_t>(progress);
  double u = progress - static_cast<double>(k);
  double a = static_cast<double>(gates[k] - gates.front()) / 1000.0;
  double b = static_cast<double>(gates[k + 1] - gates.front()) / 1000.0;
  return a + (b - a) * u;
}

void Usage(const char *argv0) {
  std::fprintf(
      stderr,
      "usage: %s --track <track.json> --out <frames.csv> [options] "
      "<input.dat|input.mp4>...\n\n"
      "  --track <path>        reference track (tracks/*.json); required\n"
      "  --out <path>          frame table; .csv or .ndjson picks the "
      "format\n"
      "  --fps <rate>          60000/1001, 59.94 or 30 (default 30)\n"
      "  --offset-ms <n>       added to every sample timestamp first; this "
      "is\n"
      "                        the offset that syncs a .dat to its video\n"
      "  --t0-ms <n>           timestamp of frame 0 (default: first sample)\n"
      "  --start <s>           frame 0 this long after the first sample\n"
      "  --duration <s>        how much to export (default: to the last "
      "sample)\n"
      "  --session-length <s>  arm the session countdown; without it\n"
      "                        session_remaining_s is left empty\n"
      "  --gate-extension <m>  how far each gate reaches past the annotated\n"
      "                        track edge (default 2)\n",
      argv0);
}

} // namespace

int main(int argc, char **argv) {
  std::string track_path, out_path;
  std::vector<std::string> inputs;
  Fps fps{30, 1};
  double start_s = 0, duration_s = -1, session_length_s = 0;
  double gate_extension_m = -1;
  int64_t offset_ms = 0, t0_ms_arg = 0;
  bool have_t0 = false;

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    auto next = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (std::strcmp(arg, "--track") == 0) {
      track_path = next(arg);
    } else if (std::strcmp(arg, "--out") == 0) {
      out_path = next(arg);
    } else if (std::strcmp(arg, "--fps") == 0) {
      const char *value = next(arg);
      if (!ParseFps(value, &fps)) {
        std::fprintf(stderr, "bad --fps: %s\n", value);
        return 2;
      }
    } else if (std::strcmp(arg, "--offset-ms") == 0) {
      offset_ms = std::atoll(next(arg));
    } else if (std::strcmp(arg, "--t0-ms") == 0) {
      t0_ms_arg = std::atoll(next(arg));
      have_t0 = true;
    } else if (std::strcmp(arg, "--start") == 0) {
      start_s = std::atof(next(arg));
    } else if (std::strcmp(arg, "--duration") == 0) {
      duration_s = std::atof(next(arg));
    } else if (std::strcmp(arg, "--session-length") == 0) {
      session_length_s = std::atof(next(arg));
    } else if (std::strcmp(arg, "--gate-extension") == 0) {
      gate_extension_m = std::atof(next(arg));
    } else if (arg[0] == '-') {
      std::fprintf(stderr, "unknown argument: %s\n", arg);
      Usage(argv[0]);
      return 2;
    } else {
      inputs.push_back(arg);
    }
  }

  if (track_path.empty() || out_path.empty() || inputs.empty()) {
    Usage(argv[0]);
    return 2;
  }

  Format format = Format::kCsv;
  if (out_path.size() > 7 &&
      out_path.compare(out_path.size() - 7, 7, ".ndjson") == 0) {
    format = Format::kNdjson;
  } else if (out_path.size() > 6 &&
             out_path.compare(out_path.size() - 6, 6, ".jsonl") == 0) {
    format = Format::kNdjson;
  }

  //------------------------------- INPUTS ---------------------------------//

  pacer::ReferenceTrack track;
  try {
    track = pacer::ReferenceTrack::FromFile(track_path);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "track: %s\n", e.what());
    return 1;
  }
  if (gate_extension_m >= 0) {
    track.gate_extension_m = gate_extension_m;
  }
  const size_t gate_count = track.DensifiedGates().size();
  if (gate_count < 2) {
    std::fprintf(stderr, "track %s has no usable gates\n", track_path.c_str());
    return 1;
  }

  std::vector<pacer::GPSSample> samples;
  std::vector<std::string> errors;
  pacer::LoadGPSFiles(
      inputs, [&](pacer::GPSSample s) { samples.push_back(s); }, &errors);
  for (const std::string &error : errors) {
    std::fprintf(stderr, "input: %s\n", error.c_str());
  }
  if (samples.size() < 2) {
    std::fprintf(stderr, "need at least two samples, got %zu\n",
                 samples.size());
    return 1;
  }
  if (offset_ms != 0) {
    for (pacer::GPSSample &s : samples) {
      s.timestamp_ms += offset_ms;
    }
  }

  // Whether the receiver reports a velocity vector is a property of the
  // source, not of a sample: a .dat carries one throughout and GPMF carries
  // none at all, so a kart that happens to be stationary must not be read as
  // a source without velocities.
  bool has_velocity = false;
  for (const pacer::GPSSample &s : samples) {
    if (s.vel_n != 0 || s.vel_e != 0) {
      has_velocity = true;
      break;
    }
  }

  //--------------------------- LIVE TIMING PASS ---------------------------//

  pacer::SessionConfig config;
  // Without an explicit session length the countdown is parked far enough out
  // that it never expires; session_remaining_s is left empty either way, so
  // an arbitrary default here can't leak into the output.
  config.session_length_s = session_length_s > 0 ? session_length_s : 1e9;

  pacer::LiveTiming live;
  live.SetReferenceTrack(track, config);

  std::vector<pacer::LiveSnapshot> snapshots(samples.size());
  for (size_t i = 0; i < samples.size(); ++i) {
    live.OnSample(samples[i]);
    snapshots[i] = live.Snapshot();
  }

  //------------------------ OFFLINE (RETROSPECTIVE) -----------------------//

  pacer::Laps laps;
  laps.SetCoordinateSystem(track.cs);
  for (const pacer::GPSSample &s : samples) {
    laps.AddPoint(s);
  }
  laps.sectors = track.BuildSectors(track.cs);
  laps.Update();

  std::vector<TimedLap> timed_laps = BuildTimedLaps(laps, track, gate_count);

  int best_index = -1;
  for (size_t i = 0; i < timed_laps.size(); ++i) {
    if (!timed_laps[i].complete || !(timed_laps[i].lap_time_s > 0)) {
      continue;
    }
    if (best_index < 0 ||
        timed_laps[i].lap_time_s < timed_laps[best_index].lap_time_s) {
      best_index = static_cast<int>(i);
    }
  }
  const TimedLap *best_lap =
      best_index >= 0 ? &timed_laps[static_cast<size_t>(best_index)] : nullptr;

  //------------------------------ FRAME GRID ------------------------------//

  const double first_ms = static_cast<double>(samples.front().timestamp_ms);
  const double last_ms = static_cast<double>(samples.back().timestamp_ms);

  double t0_ms = have_t0 ? static_cast<double>(t0_ms_arg)
                         : first_ms + start_s * 1000.0;
  double end_ms = duration_s >= 0 ? t0_ms + duration_s * 1000.0 : last_ms;
  const double frame_ms = 1000.0 * static_cast<double>(fps.den) /
                          static_cast<double>(fps.num);
  int64_t frame_count =
      end_ms > t0_ms
          ? static_cast<int64_t>(std::floor((end_ms - t0_ms) / frame_ms)) + 1
          : 0;
  if (frame_count <= 0) {
    std::fprintf(stderr, "empty frame range\n");
    return 1;
  }

  std::FILE *out = std::fopen(out_path.c_str(), "wb");
  if (out == nullptr) {
    std::fprintf(stderr, "cannot write %s\n", out_path.c_str());
    return 1;
  }
  if (format == Format::kCsv) {
    std::fputs(kHeader, out);
  }

  size_t cursor = 1;
  double last_heading = kNaN;
  int64_t covered = 0;

  for (int64_t frame = 0; frame < frame_count; ++frame) {
    // Off the frame index rather than by accumulation, so a long session
    // cannot drift against the picture one rounding error at a time.
    const double t_ms = t0_ms + static_cast<double>(frame) * frame_ms;

    Row row;
    row.frame = frame;
    row.t_s = (t_ms - t0_ms) / 1000.0;
    row.gate_count = static_cast<double>(gate_count);

    if (t_ms < first_ms || t_ms > last_ms) {
      // Outside the recording: the row exists to keep the table in step with
      // the video, and every reading in it stays empty.
      if (format == Format::kCsv) {
        WriteCsvRow(out, row);
      } else {
        WriteJsonRow(out, row);
      }
      continue;
    }
    ++covered;

    while (cursor + 1 < samples.size() &&
           static_cast<double>(samples[cursor].timestamp_ms) < t_ms) {
      ++cursor;
    }
    const size_t hi = cursor, lo = cursor - 1;
    const pacer::GPSSample &s0 = samples[lo];
    const pacer::GPSSample &s1 = samples[hi];
    const double span_ms =
        static_cast<double>(s1.timestamp_ms - s0.timestamp_ms);
    const double u =
        span_ms > 0 ? std::clamp((t_ms - static_cast<double>(s0.timestamp_ms)) /
                                     span_ms,
                                 0.0, 1.0)
                    : 0.0;
    const double dt_s = span_ms / 1000.0;

    //------------------------------ POSITION ------------------------------//

    const pacer::Vec3f p0 = track.cs.Local(s0);
    const pacer::Vec3f p1 = track.cs.Local(s1);
    double x = 0, y = 0;
    if (has_velocity) {
      // Cubic Hermite with the receiver's Doppler velocity as the tangent.
      // Straight linear interpolation between 25 Hz fixes is visibly angular
      // once a 60 fps marker is drawn on it -- the kart turns between fixes,
      // and the velocity vector is what says how.
      const double h00 = (2 * u - 3) * u * u + 1;
      const double h10 = ((u - 2) * u + 1) * u;
      const double h01 = (3 - 2 * u) * u * u;
      const double h11 = (u - 1) * u * u;
      x = h00 * p0.x + h10 * dt_s * s0.vel_e + h01 * p1.x + h11 * dt_s * s1.vel_e;
      y = h00 * p0.y + h10 * dt_s * s0.vel_n + h01 * p1.y + h11 * dt_s * s1.vel_n;
    } else {
      x = p0.x + (p1.x - p0.x) * u;
      y = p0.y + (p1.y - p0.y) * u;
    }
    const double z = p0.z + (p1.z - p0.z) * u;

    row.x_m = x;
    row.y_m = y;
    const pacer::GPSSample global = track.cs.Global(pacer::Vec3f{x, y, z});
    row.lat = global.lat;
    row.lon = global.lon;
    row.speed_mps = s0.full_speed + (s1.full_speed - s0.full_speed) * u;

    double east = 0, north = 0;
    if (has_velocity) {
      east = s0.vel_e + (s1.vel_e - s0.vel_e) * u;
      north = s0.vel_n + (s1.vel_n - s0.vel_n) * u;
    } else if (dt_s > 0) {
      east = (p1.x - p0.x) / dt_s;
      north = (p1.y - p0.y) / dt_s;
    }
    if (std::hypot(east, north) >= kHeadingMinSpeedMps) {
      double heading = std::atan2(east, north);
      last_heading = heading < 0 ? heading + 2 * M_PI : heading;
    }
    // Bearing from north, clockwise, in [0, 2pi) -- the angle a marker drawn
    // over a north-up map is rotated by.
    row.heading_rad = last_heading;

    //------------------------------- TIMING -------------------------------//

    const pacer::LiveSnapshot &prev = snapshots[lo];
    const pacer::LiveSnapshot &next = snapshots[hi];

    const pacer::LiveSnapshot *base = &prev;
    double lap_ref_ms = static_cast<double>(s0.timestamp_ms);
    double lap_base_s = prev.current_lap_s;

    // A lap can start between two samples, and the crossing is exactly where
    // the new lap's own clock says it is. Without this the lap counter and
    // the timer reset late by up to a fix interval -- 40 ms at 25 Hz, which
    // at the start line is a metre and a half of visibly wrong overlay.
    if (next.lap_number != prev.lap_number &&
        std::isfinite(next.current_lap_s)) {
      const double lap_start_ms =
          static_cast<double>(s1.timestamp_ms) - next.current_lap_s * 1000.0;
      if (t_ms >= lap_start_ms) {
        base = &next;
        lap_ref_ms = lap_start_ms;
        lap_base_s = 0.0;
      }
    }

    row.lap_number = base->lap_number;
    // Timers are computed, never interpolated from the sampled snapshots:
    // a clock reconstructed by lerping a 25 Hz reading stutters at 60 fps.
    if (std::isfinite(lap_base_s)) {
      row.current_lap_s = lap_base_s + (t_ms - lap_ref_ms) / 1000.0;
    }
    if (std::isfinite(base->last_lap_s)) {
      row.last_lap_s = base->last_lap_s;
    }
    if (std::isfinite(base->best_lap_s)) {
      row.best_lap_s = base->best_lap_s;
    }
    if (session_length_s > 0 && std::isfinite(prev.session_remaining_s)) {
      row.session_remaining_s =
          prev.session_remaining_s -
          (t_ms - static_cast<double>(s0.timestamp_ms)) / 1000.0;
    }

    row.delta_valid = base->delta_valid ? 1 : 0;
    row.lost = base->lost ? 1 : 0;
    // Blank rather than stale: a delta that has stopped being a reading is
    // worse on screen than no delta, because nothing about a frozen number
    // says it has stopped moving. delta_valid keeps the distinction.
    if (base->delta_valid) {
      row.delta_live_s = base->delta_s;
    }

    //---------------------- RETROSPECTIVE DELTA ---------------------------//

    const int64_t t_round_ms = static_cast<int64_t>(std::llround(t_ms));
    if (const TimedLap *lap = LapAt(timed_laps, t_round_ms)) {
      const double progress = GateProgress(*lap, t_round_ms);
      row.gate = progress;
      if (best_lap != nullptr) {
        const double reference = TimeAtProgress(*best_lap, progress);
        if (std::isfinite(reference)) {
          const double elapsed =
              static_cast<double>(t_round_ms - lap->start_ms) / 1000.0;
          row.delta_best_s = elapsed - reference;
        }
      }
    }

    if (format == Format::kCsv) {
      WriteCsvRow(out, row);
    } else {
      WriteJsonRow(out, row);
    }
  }

  std::fclose(out);

  //------------------------------- MANIFEST -------------------------------//

  // What a re-render six months from now needs to reproduce this table, and
  // what a renderer needs to lay it over the right clip: the exact frame rate
  // and origin, and the command that produced it.
  std::string manifest_path = out_path;
  const size_t dot = manifest_path.find_last_of('.');
  const size_t slash = manifest_path.find_last_of('/');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
    manifest_path.resize(dot);
  }
  manifest_path += ".manifest.json";

  std::string command = argv[0];
  for (int i = 1; i < argc; ++i) {
    command += ' ';
    command += argv[i];
  }

  char created[32] = "";
  const std::time_t now = std::time(nullptr);
  std::strftime(created, sizeof created, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));

  nlohmann::json manifest;
  manifest["schema"] = 1;
  manifest["generator"] = "pacer export_overlay";
  manifest["created_utc"] = created;
  manifest["command"] = command;
  manifest["inputs"] = inputs;
  manifest["track"] = track_path;
  manifest["gate_count"] = gate_count;
  manifest["gate_extension_m"] = track.gate_extension_m;
  manifest["offset_ms"] = offset_ms;
  manifest["fps"] = {{"num", fps.num}, {"den", fps.den}, {"value", fps.Value()}};
  manifest["t0_ms"] = t0_ms;
  manifest["frames"] = frame_count;
  manifest["frames_covered"] = covered;
  manifest["duration_s"] = frame_count * frame_ms / 1000.0;
  manifest["samples"] = samples.size();
  manifest["has_velocity"] = has_velocity;
  manifest["session_length_s"] = session_length_s;
  manifest["format"] = format == Format::kCsv ? "csv" : "ndjson";

  nlohmann::json lap_list = nlohmann::json::array();
  for (const TimedLap &lap : timed_laps) {
    lap_list.push_back({
        {"start_ms", lap.start_ms},
        {"end_ms", lap.end_ms},
        {"time_s", lap.lap_time_s},
        {"complete", lap.complete},
        {"gates", lap.gate_ms.size()},
    });
  }
  manifest["laps"] = lap_list;
  if (best_lap != nullptr) {
    manifest["best_lap"] = {{"index", best_index},
                            {"time_s", best_lap->lap_time_s}};
  } else {
    manifest["best_lap"] = nullptr;
  }

  std::FILE *manifest_file = std::fopen(manifest_path.c_str(), "wb");
  if (manifest_file == nullptr) {
    std::fprintf(stderr, "cannot write %s\n", manifest_path.c_str());
    return 1;
  }
  const std::string manifest_text = manifest.dump(2);
  std::fwrite(manifest_text.data(), 1, manifest_text.size(), manifest_file);
  std::fputc('\n', manifest_file);
  std::fclose(manifest_file);

  //-------------------------------- SUMMARY -------------------------------//

  std::printf("%zu samples over %.1f min, %s velocities\n", samples.size(),
              (last_ms - first_ms) / 60000.0,
              has_velocity ? "with" : "no");
  std::printf("%zu gates, %zu laps split (%d complete)\n", gate_count,
              timed_laps.size(),
              static_cast<int>(std::count_if(
                  timed_laps.begin(), timed_laps.end(),
                  [](const TimedLap &lap) { return lap.complete; })));
  if (best_lap != nullptr) {
    std::printf("best lap: #%d, %.3f s\n", best_index, best_lap->lap_time_s);
  } else {
    std::printf("best lap: none complete -- delta_best_s is empty\n");
  }
  std::printf("%lld frames at %ld/%ld (%.4f fps), %lld covered\n",
              static_cast<long long>(frame_count), fps.num, fps.den,
              fps.Value(), static_cast<long long>(covered));
  std::printf("wrote %s and %s\n", out_path.c_str(), manifest_path.c_str());
  return 0;
}
