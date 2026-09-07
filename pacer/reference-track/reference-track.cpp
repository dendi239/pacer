#include "reference-track.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace {

// Converts a Segment stored in local coordinates (relative to `cs`) to raw
// lon/lat Points, matching what pacer::Split() expects when intersecting
// against a driven lap's raw GPSSample points.
pacer::Segment ToGlobalSegment(const pacer::Segment &local,
                               const pacer::CoordinateSystem &cs) {
  return pacer::Segment{
      pacer::ToPoint(cs.Global(pacer::Vec3f{local.first.x, local.first.y, 0})),
      pacer::ToPoint(
          cs.Global(pacer::Vec3f{local.second.x, local.second.y, 0})),
  };
}

// Extends a local-coordinate segment by `meters` beyond each endpoint, along
// its own direction.
pacer::Segment ExtendSegment(const pacer::Segment &s, double meters) {
  pacer::Point dir = s.second - s.first;
  double length = std::sqrt(dir.Norm());
  if (length < 1e-9) {
    return s;
  }
  dir = dir / length;
  return pacer::Segment{s.first - dir * meters, s.second + dir * meters};
}

// Inserts synthetic gates between each consecutive pair so there's roughly
// one gate per meter of track, linearly interpolating each pair's endpoints.
// Without this, a delta calculated against widely-spaced hand-drawn gates
// (e.g. down a straight) is jittery: two laps only get compared where a
// gate actually is, so long gaps between gates show up as noise.
//
// The pair from the last annotated gate back round to the first is included:
// a track is a loop, and leaving that stretch bare left the last tens of
// meters of every lap — the run down to the line, on most circuits — with no
// gates at all, so the live delta had nothing to update against there.
std::vector<pacer::Segment>
DensifyGates(const std::vector<pacer::Segment> &gates) {
  if (gates.size() < 2) {
    return gates;
  }

  // Roughly one gate per meter between each annotated pair.
  auto steps_between = [](const pacer::Segment &g1, const pacer::Segment &g2) {
    pacer::Point mid1 = (g1.first + g1.second) / 2.0;
    pacer::Point mid2 = (g2.first + g2.second) / 2.0;
    double distance = std::sqrt((mid2 - mid1).Norm());
    return std::max<size_t>(1, static_cast<size_t>(std::ceil(distance)));
  };

  // Counting first costs one cheap pass and saves the growth overshoot: a
  // 1.1 km circuit densifies to ~1100 segments, and letting the vector
  // double its way there wastes ~30 KB of heap the ESP32 doesn't have.
  size_t n = gates.size();
  size_t total = 0;
  for (size_t i = 0; i < n; ++i) {
    total += steps_between(gates[i], gates[(i + 1) % n]);
  }

  // Each pair contributes its own gate plus the synthetic ones up to (but not
  // including) the next annotated gate, so gates[0] — the start/finish line —
  // stays index 0 and appears exactly once.
  std::vector<pacer::Segment> dense;
  dense.reserve(total);
  for (size_t i = 0; i < n; ++i) {
    const pacer::Segment &g1 = gates[i];
    const pacer::Segment &g2 = gates[(i + 1) % n];
    size_t steps = steps_between(g1, g2);

    for (size_t k = 0; k < steps; ++k) {
      double t = static_cast<double>(k) / static_cast<double>(steps);
      dense.push_back(pacer::Segment{
          pacer::Interpolate(g1.first, g2.first, t),
          pacer::Interpolate(g1.second, g2.second, t),
      });
    }
  }
  return dense;
}

// How many gates ahead of the next expected one a single sample interval may
// reach, and equivalently the widest run of gates Resample() will bridge.
//
// Two things eat into this. A kart covers a couple of meters between fixes,
// so one interval crosses a handful of the ~1 m gates. And densifying by
// interpolating endpoints makes consecutive gates converge on the inside of
// a bend — tightly enough that they cross over each other, so a lap on the
// inside line meets them out of index order and leaves a run of them
// untouched. Measured over a Daytona Milton Keynes session, those runs reach
// 23 gates at the tighter corners, and nothing improves past a window of 24.
constexpr size_t kGateWindow = 32;

} // namespace

size_t pacer::ReferenceTrack::Count() const { return segments.size(); }

size_t pacer::ReferenceTrack::TimingLinesCount() const {
  return segments.size();
}

pacer::Segment pacer::ReferenceTrack::TimingLine(size_t index) const {
  return ExtendSegment(segments[index], gate_extension_m);
}

std::vector<pacer::Segment> pacer::ReferenceTrack::DensifiedGates() const {
  std::vector<Segment> gates;
  gates.reserve(TimingLinesCount());
  for (size_t i = 0; i < TimingLinesCount(); ++i) {
    gates.push_back(TimingLine(i));
  }
  return DensifyGates(gates);
}

pacer::Segment pacer::ReferenceTrack::ToGlobal(const Segment &local) const {
  return ToGlobalSegment(local, cs);
}

pacer::Lap pacer::ReferenceTrack::Resample(const Lap &lap) const {
  if (lap.points.empty()) {
    return lap;
  }

  std::vector<Segment> dense_gates = DensifiedGates();
  if (dense_gates.empty()) {
    return lap;
  }

  // Convert once up front: Global() is trigonometry, and the scan below looks
  // at each gate from several different sample intervals.
  std::vector<Segment> gates;
  gates.reserve(dense_gates.size());
  for (const Segment &gate : dense_gates) {
    gates.push_back(ToGlobalSegment(gate, cs));
  }

  // One point per gate, so two laps resampled against the same track line up
  // index for index — which is the whole premise of comparing them.
  std::vector<GPSSample> crossings(gates.size());
  std::vector<bool> crossed(gates.size(), false);

  // Gate 0 is the start/finish line, and Laps::GetLap() begins every lap
  // exactly on it. A point lying on a line does not count as crossing it, so
  // gate 0 never matches by intersection; take it from the lap itself. Left
  // to the scan, it would consume the entire lap looking for a crossing that
  // cannot happen, and every later gate would come up empty.
  crossings[0] = lap.points.front();
  crossed[0] = true;

  size_t next_gate = 1;
  for (size_t i = 1; i < lap.points.size() && next_gate < gates.size(); ++i) {
    // Gates sit about a meter apart, so one interval can cross several of
    // them; keep taking crossings until none of the upcoming ones intersects
    // it. Searching a bounded window rather than the rest of the lap is what
    // keeps a run of gates the racing line stepped over from stopping the
    // scan dead for the remainder of the lap.
    for (bool found = true; found && next_gate < gates.size();) {
      found = false;
      size_t window = std::min(kGateWindow, gates.size() - next_gate);
      for (size_t k = 0; k < window; ++k) {
        size_t gate = next_gate + k;
        auto split_point =
            pacer::Split(gates[gate], lap.points[i - 1], lap.points[i]);
        if (!split_point) {
          continue;
        }
        crossings[gate] = *split_point;
        crossed[gate] = true;
        next_gate = gate + 1;
        found = true;
        break;
      }
    }
  }

  // Gates the lap stepped over keep their slot, interpolated between the
  // crossings either side — the same fill the live-timing engine does. Simply
  // leaving them out would shift every later index by however many this
  // particular lap happened to miss, and quietly compare two laps at
  // different parts of the track.
  for (size_t gate = 1, previous = 0; gate < next_gate; ++gate) {
    if (!crossed[gate]) {
      continue;
    }
    for (size_t skipped = previous + 1; skipped < gate; ++skipped) {
      double ratio = static_cast<double>(skipped - previous) /
                     static_cast<double>(gate - previous);
      crossings[skipped] =
          Interpolate(crossings[previous], crossings[gate], ratio);
    }
    previous = gate;
  }

  // Gates past next_gate were never reached — the lap ran out first — so they
  // have nothing to interpolate from and are dropped.
  Lap result;
  result.points.assign(crossings.begin(), crossings.begin() + next_gate);
  if (next_gate == gates.size()) {
    // The last gate stops a meter short of the start/finish line; close the
    // lap off with its own final point so the trace covers the full lap
    // distance. Skipped when the sequence didn't run to the end, where it
    // would jump across whatever is missing.
    result.points.push_back(lap.points.back());
  }
  result.FillDistances(cs);
  return result;
}

pacer::ReferenceTrack
pacer::ReferenceTrack::FromLap(const Lap &lap, float width,
                               const CoordinateSystem &cs) {
  ReferenceTrack track;
  track.cs = cs;
  if (lap.points.size() < 3) {
    return track;
  }

  size_t count = lap.points.size() - 2;
  track.segments.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    size_t idx = i + 1;
    Vec3f prev = cs.Local(lap.points[idx - 1]);
    Vec3f curr = cs.Local(lap.points[idx]);
    Vec3f next = cs.Local(lap.points[idx + 1]);

    Vec3f dir = (next - prev);
    dir /= std::sqrt(dir.Norm());
    Vec3f norm = Vec3f{dir[1], -dir[0], 0};

    track.segments.push_back(
        Segment{ToPoint(curr - norm * width), ToPoint(curr + norm * width)});
  }
  return track;
}

pacer::ReferenceTrack
pacer::ReferenceTrack::FromFile(const std::string &filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    throw std::runtime_error("Unable to open file: " + filename);
  }

  std::vector<std::pair<GPSSample, GPSSample>> raw;
  std::vector<int> sector_indices;
  try {
    nlohmann::json json;
    file >> json;
    if (!json.contains("segments") || !json["segments"].is_array()) {
      throw std::runtime_error(
          "Invalid reference track file, missing segments");
    }

    for (const auto &entry : json["segments"]) {
      if (!entry.is_array() || entry.size() < 2)
        continue;
      if (!entry[0].is_array() || entry[0].size() < 2)
        continue;
      if (!entry[1].is_array() || entry[1].size() < 2)
        continue;
      GPSSample a{.lat = entry[0][0].get<double>(),
                  .lon = entry[0][1].get<double>()};
      GPSSample b{.lat = entry[1][0].get<double>(),
                  .lon = entry[1][1].get<double>()};
      raw.emplace_back(a, b);
    }

    if (json.contains("sector_indices") && json["sector_indices"].is_array()) {
      for (const auto &entry : json["sector_indices"]) {
        sector_indices.push_back(entry.get<int>());
      }
    }
  } catch (const nlohmann::json::exception &e) {
    throw std::runtime_error(std::string("Invalid reference track file: ") +
                             e.what());
  }

  ReferenceTrack track;
  track.sector_indices = std::move(sector_indices);
  if (raw.empty()) {
    return track;
  }

  track.cs = CoordinateSystem(raw.front().first);
  track.segments.reserve(raw.size());
  for (const auto &[a, b] : raw) {
    track.segments.push_back(
        Segment{ToPoint(track.cs.Local(a)), ToPoint(track.cs.Local(b))});
  }

  return track;
}

std::string pacer::ReferenceTrack::ToJsonString() const {
  nlohmann::json json;
  json["segments"] = nlohmann::json::array();
  for (const auto &seg : segments) {
    Segment global = ToGlobalSegment(seg, cs);
    json["segments"].push_back(
        {{global.first.y, global.first.x}, {global.second.y, global.second.x}});
  }
  json["sector_indices"] = sector_indices;

  return json.dump(2);
}

void pacer::ReferenceTrack::SaveToFile(const std::string &filename) const {
  std::ofstream file(filename);
  if (!file.is_open()) {
    throw std::runtime_error("Unable to write file: " + filename);
  }
  file << ToJsonString();
}

pacer::Sectors
pacer::ReferenceTrack::BuildSectors(const CoordinateSystem &target_cs) const {
  Sectors result;
  if (segments.empty()) {
    return result;
  }

  auto convert = [&](const Segment &local) {
    auto a = cs.Global(Vec3f{local.first.x, local.first.y, 0});
    auto b = cs.Global(Vec3f{local.second.x, local.second.y, 0});
    return Segment{ToPoint(target_cs.Local(a)), ToPoint(target_cs.Local(b))};
  };

  result.start_line = convert(TimingLine(0));
  for (int index : sector_indices) {
    if (index < 0 || static_cast<size_t>(index) >= segments.size()) {
      continue;
    }
    result.sector_lines.push_back(convert(TimingLine(index)));
  }
  return result;
}
