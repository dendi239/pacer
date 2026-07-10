#pragma once

#include <string>
#include <vector>

#include <pacer/geometry/geometry.hpp>
#include <pacer/laps/laps.hpp>

namespace pacer {

// A hand-annotated (or synthesized) sequence of timing-line gates used to
// project driven laps onto a common set of cross-sections for delta
// calculation. Geometry is stored in local coordinates relative to `cs`, so
// it stays internally consistent regardless of any other CoordinateSystem in
// use elsewhere (e.g. for map display).
struct ReferenceTrack {
  CoordinateSystem cs;
  std::vector<Segment> segments;
  std::vector<int> sector_indices; // ordered indices into segments

  size_t Count() const;
  size_t TimingLinesCount() const;

  /// Returns segments[index] extended a couple of meters past each edge, so
  /// the gate still catches a driven lap that strays slightly outside the
  /// annotated track boundary.
  Segment TimingLine(size_t index) const;

  /// All TimingLine()s densified to roughly one synthetic gate per meter
  /// (linearly interpolated between each annotated pair), in this track's
  /// local frame. Gate 0 is the start/finish line. Both Resample() and the
  /// live-timing engine consume laps through this same gate sequence, so
  /// their deltas agree.
  std::vector<Segment> DensifiedGates() const;

  /// Converts a local-frame segment to raw lon/lat Points, i.e. the frame
  /// pacer::Split() expects when intersecting against raw GPSSample points.
  Segment ToGlobal(const Segment &local) const;

  /// Projects `lap` onto this track's timing lines, producing a Lap whose
  /// points align (index-for-index) with any other lap resampled against the
  /// same ReferenceTrack. Internally, consecutive gates are densified to
  /// roughly one synthetic gate per meter (linearly interpolated between
  /// each pair) so widely-spaced hand-drawn gates, e.g. down a straight,
  /// don't produce a jittery delta.
  Lap Resample(const Lap &lap) const;

  /// Builds a ReferenceTrack the old way: a perpendicular offset of `width`
  /// meters at every interior point of `lap`. Useful when there is no
  /// hand-annotated track, only a recorded lap to use as a stand-in.
  static ReferenceTrack FromLap(const Lap &lap, float width,
                               const CoordinateSystem &cs);

  /// Loads a reference track from the JSON schema written by
  /// track_annotator ({"segments": [[[lat,lon],[lat,lon]], ...]}).
  /// Throws std::runtime_error on failure.
  static ReferenceTrack FromFile(const std::string &filename);

  /// Writes this track using the same JSON schema. Throws
  /// std::runtime_error on failure.
  void SaveToFile(const std::string &filename) const;

  /// Builds a pacer::Sectors using segments[0] as the start/finish line and
  /// sector_indices (in order) as sector splits, converting from this
  /// track's local frame into target_cs (the frame the consuming Laps
  /// object uses). Returns a default (empty) Sectors if segments is empty.
  /// Uses the raw annotated segments, not the TimingLine-extended ones —
  /// the gate extension is a delta-calculation robustness hack, not
  /// something that should silently move where a lap/sector splits.
  Sectors BuildSectors(const CoordinateSystem &target_cs) const;
};

} // namespace pacer
