#include "reference-track.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace {

// Hand-drawn gates are only as wide as the annotated track edges; extending
// them past each edge makes delta calculation robust to a driven lap
// straying slightly outside the annotated boundary (noise, imprecise
// annotation, running wide, etc).
constexpr double kGateExtensionMeters = 2.0;

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
std::vector<pacer::Segment>
DensifyGates(const std::vector<pacer::Segment> &gates) {
  if (gates.size() < 2) {
    return gates;
  }

  std::vector<pacer::Segment> dense;
  for (size_t i = 0; i + 1 < gates.size(); ++i) {
    const pacer::Segment &g1 = gates[i];
    const pacer::Segment &g2 = gates[i + 1];
    pacer::Point mid1 = (g1.first + g1.second) / 2.0;
    pacer::Point mid2 = (g2.first + g2.second) / 2.0;
    double distance = std::sqrt((mid2 - mid1).Norm());
    size_t steps =
        std::max<size_t>(1, static_cast<size_t>(std::ceil(distance)));

    for (size_t k = 0; k < steps; ++k) {
      double t = static_cast<double>(k) / static_cast<double>(steps);
      dense.push_back(pacer::Segment{
          pacer::Interpolate(g1.first, g2.first, t),
          pacer::Interpolate(g1.second, g2.second, t),
      });
    }
  }
  dense.push_back(gates.back());
  return dense;
}

} // namespace

size_t pacer::ReferenceTrack::Count() const { return segments.size(); }

size_t pacer::ReferenceTrack::TimingLinesCount() const {
  return segments.size();
}

pacer::Segment pacer::ReferenceTrack::TimingLine(size_t index) const {
  return ExtendSegment(segments[index], kGateExtensionMeters);
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

  Lap result{.points = {lap.points.front()}};

  for (size_t i_gate = 0, i_lap = 1; i_gate < dense_gates.size(); ++i_gate) {
    if (i_lap >= lap.points.size()) {
      break;
    }
    Segment timing_line = ToGlobalSegment(dense_gates[i_gate], cs);

    while (i_lap < lap.points.size()) {
      auto split_point =
          pacer::Split(timing_line, lap.points[i_lap - 1], lap.points[i_lap]);
      if (split_point) {
        result.points.push_back(*split_point);
        break;
      }
      ++i_lap;
    }
  }

  result.points.push_back(lap.points.back());
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
    Vec3f prev = cs.Local(lap.points[idx - 1].point);
    Vec3f curr = cs.Local(lap.points[idx].point);
    Vec3f next = cs.Local(lap.points[idx + 1].point);

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

void pacer::ReferenceTrack::SaveToFile(const std::string &filename) const {
  nlohmann::json json;
  json["segments"] = nlohmann::json::array();
  for (const auto &seg : segments) {
    Segment global = ToGlobalSegment(seg, cs);
    json["segments"].push_back(
        {{global.first.y, global.first.x}, {global.second.y, global.second.x}});
  }
  json["sector_indices"] = sector_indices;

  std::ofstream file(filename);
  if (!file.is_open()) {
    throw std::runtime_error("Unable to write file: " + filename);
  }
  file << json.dump(2);
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

  result.start_line = convert(segments[0]);
  for (int index : sector_indices) {
    if (index < 0 || static_cast<size_t>(index) >= segments.size()) {
      continue;
    }
    result.sector_lines.push_back(convert(segments[index]));
  }
  return result;
}
