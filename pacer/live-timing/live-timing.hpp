#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include <pacer/datatypes/datatypes.hpp>
#include <pacer/geometry/geometry.hpp>
#include <pacer/reference-track/reference-track.hpp>

namespace pacer {

// Incremental, sample-by-sample counterpart of Laps::Update() +
// ReferenceTrack::Resample(): consumes a live 10-25 Hz GPS stream and keeps
// current/last/best lap times plus a running delta to the session-best lap,
// measured at the same densified gates Resample() uses. Holds no point
// history — memory is two gate-time arrays — so it runs happily on an ESP32.
//
// Single-threaded by design: call OnSample() and Snapshot() from one thread,
// or guard externally (the firmware copies Snapshot() under a mutex).

struct SessionConfig {
  /// Timed-session length; the countdown starts when lap 1 does, i.e. at
  /// the first start-line crossing. The out lap is not on the clock.
  double session_length_s = 15 * 60;

  /// Gate crossings below this speed are ignored — a parked kart's fix
  /// noise wiggles across nearby gates. Kept just above the receiver's
  /// stationary speed noise (~0.2 m/s) so even a slow on-foot track walk
  /// (anything above ~1 km/h) still produces laps.
  double min_crossing_speed_mps = 0.3;

  /// Crossings of the start line earlier than this after lap start are
  /// ignored, so a wide/extended start gate can't double-trigger.
  double min_lap_s = 15.0;

  /// How many gates ahead of the expected one to search each sample; a
  /// glitchy fix can skip a few gates, and skipped gate times are filled by
  /// interpolation.
  size_t gate_lookahead = 12;

  /// Widest hole the skipped-gate interpolation will fill, in gates (~1 m
  /// each). Past this the kart wasn't following the gate sequence at all,
  /// and inventing times across the hole would both fake the delta and let
  /// a lap that was never fully driven become the reference.
  size_t max_interpolated_gates = 25;

  /// How far the kart may travel without crossing the gate it is expected
  /// at before the tracker goes looking for where the kart actually is.
  /// Gates sit ~1 m apart and the forward search covers gate_lookahead of
  /// them, so anything past that and the kart is not where the sequence
  /// says: it has run wide of the annotated edge, taken the pit lane, or
  /// the receiver dropped a burst of fixes.
  double resync_distance_m = 15.0;

  /// While lost, how close to a gate the fix must be for that gate to count
  /// as a re-acquisition. Kart-circuit pit lanes run alongside the start
  /// straight, tens of meters out; this keeps one from re-anchoring the
  /// tracker onto the racing line it is parked next to.
  double resync_max_offset_m = 8.0;

  /// Distance between re-acquisition scans while lost. The scan is over
  /// every gate, so it is the one non-trivial cost here; pacing it by
  /// distance keeps a kart parked in the pits from paying it at all.
  double resync_scan_interval_m = 5.0;
};

struct LiveSnapshot {
  /// 0 while on the out lap (start line not crossed yet), then 1, 2, ...
  int lap_number = 0;

  /// True once lap 1 has started; the out lap runs off the clock.
  bool session_started = false;
  /// NaN until the session clock is armed; negative once time has expired.
  double session_remaining_s = 0;
  /// Time since the session clock was armed; NaN until then.
  double session_elapsed_s = 0;

  /// Elapsed time on the current lap; NaN until the first flying lap starts.
  double current_lap_s = 0;
  double last_lap_s = 0; ///< NaN until a lap is completed.
  double best_lap_s = 0; ///< NaN until a lap is completed.

  /// Current lap time at the last crossed gate minus the session-best lap's
  /// time at that same gate. Only meaningful while delta_valid.
  double delta_s = 0;

  /// False whenever delta_s is not a live reading: before the first
  /// reference lap, and — this is the one that matters in a race — while
  /// the tracker has lost the kart (see SessionConfig::resync_distance_m).
  /// A frozen number on screen is worse than no number, because the driver
  /// can't tell it has stopped moving.
  bool delta_valid = false;

  /// True while the tracker cannot find the kart on the gate sequence at
  /// all: in the pit lane, deep in the run-off, or riding out a fix outage.
  /// A kart that merely ran wide of a gate is re-acquired silently and
  /// never sets this.
  bool lost = false;

  double speed_mps = 0;

  /// Progress around the reference track, for a lap-progress bar and for
  /// debugging gate sync.
  size_t gates_crossed = 0;
  size_t gate_count = 0;
};

/// Where a fix sits relative to the reference track, for the on-device
/// debug screen: a systematic GPS bias shows up as a steady lateral_m while
/// driving the racing line (or standing on a known spot on track).
struct TrackOffset {
  size_t gate = 0; ///< nearest densified gate index

  /// Signed offset from that gate's midpoint along the gate direction,
  /// positive toward the gate's second endpoint. |lateral_m| beyond
  /// half_width_m means the fix is outside the annotated track.
  double lateral_m = 0;

  /// Half the gate's length: annotated track half-width plus the couple of
  /// meters TimingLine() extends past each edge.
  double half_width_m = 0;

  double distance_m = 0; ///< ground distance to the gate segment itself
};

class LiveTiming {
public:
  LiveTiming() = default;

  /// Resets all session state and installs the track. Gate 0 of
  /// rt.DensifiedGates() (== segments[0], the start/finish line) both starts
  /// and finishes laps.
  void SetReferenceTrack(const ReferenceTrack &rt, SessionConfig cfg = {});

  /// Starts the session over on the track already installed: lap numbering,
  /// lap/session clocks and the delta reference all go back to where
  /// SetReferenceTrack() left them, without re-parsing the track. The next
  /// start-line crossing begins lap 1 and rearms the session clock.
  void ResetSession();

  /// Feed one GPS fix; `s.timestamp_ms` must be milliseconds on a monotonic
  /// clock consistent across the session (e.g. UBX iTOW).
  void OnSample(GPSSample s);

  LiveSnapshot Snapshot() const { return snapshot_; }

  /// Ground distance in meters from `s` to the next timing line to be
  /// crossed: the start/finish line while not on a lap (including before
  /// the first flying lap), else the next expected gate. NaN when no track
  /// is installed.
  double DistanceToNextLine(const GPSSample &s) const;

  /// Offset of `s` relative to the nearest densified gate (see TrackOffset).
  /// Scans every gate, so call it at UI rate — not per GPS sample — and
  /// only while someone is looking. nullopt when no track is installed.
  std::optional<TrackOffset> OffsetFromTrack(const GPSSample &s) const;

private:
  /// Gate/start-line bookkeeping for one sample; OnSample() wraps it with
  /// the clock updates, which must run even on the samples this skips.
  void TrackCrossings(const GPSSample &s);

  void StartLap(double crossing_time);
  void FinishLap(double crossing_time);
  void RecordGate(size_t gate, double crossing_time);

  /// Full-gate scan for where `s` actually is, called while lost. Resumes
  /// tracking there if the kart is back on the gate sequence ahead of where
  /// it was last seen, drops the lap if it has come round past the start
  /// line without crossing it, and does nothing while still off-track.
  void TryReacquire(const GPSSample &s);

  /// Gives up on the lap in progress without timing it: the lap clock has
  /// stopped meaning anything, so the next start-line crossing begins a
  /// fresh lap. Session clock, lap count and best lap are untouched.
  void AbandonLap();

  SessionConfig cfg_;

  /// Densified gates in the global (lon/lat) frame, ready for Split().
  std::vector<Segment> gates_;

  bool has_prev_ = false;
  GPSSample prev_;

  bool on_lap_ = false;
  size_t next_gate_ = 0; ///< next expected gate index while on a lap
  double lap_start_time_ = 0;
  size_t last_recorded_gate_ = 0;

  /// Ground distance covered since the last gate was recorded, and since
  /// the last re-acquisition scan. Integrated from the receiver's speed
  /// rather than from position deltas, so fix noise doesn't accumulate into
  /// it while the kart sits still.
  double distance_since_gate_m_ = 0;
  double distance_since_scan_m_ = 0;

  /// Per-gate times relative to lap start; NaN where not (yet) crossed.
  std::vector<double> current_gate_times_;
  std::vector<double> best_gate_times_;

  LiveSnapshot snapshot_;
  double session_start_time_ = 0;
};

} // namespace pacer
