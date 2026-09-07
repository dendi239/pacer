#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <pacer/datatypes/datatypes.hpp>
#include <pacer/gps-source/gps-source.hpp>
#include <pacer/laps/laps.hpp>
#include <pacer/reference-track/reference-track.hpp>

namespace pacer {

/// Handle to one lap of one source. Names the source by id rather than by
/// position, so it survives reordering and removal of other sources.
struct LapRef {
  int source_id = -1;
  int lap_index = -1;

  friend bool operator==(const LapRef &, const LapRef &) = default;

  bool Valid() const { return source_id >= 0 && lap_index >= 0; }

  /// "F1L2" -- source 1, lap 2. Laps are split over a source's whole
  /// concatenated sample stream (a GoPro clip boundary can fall mid-lap),
  /// so a lap belongs to a source, not to a file within it.
  std::string Label() const;
};

/// ImGui drag-and-drop payload type for a lap; the payload itself is a
/// LapRef. Dragged from a source's lap chart or lap table, dropped on a
/// comparison.
inline constexpr const char *kLapDragPayload = "PACER_LAP";

/// Formats a lap time the way a timing screen does: "1:07.104". Sector
/// times are short enough to read as plain seconds, so they don't use this.
std::string FormatLapTime(double seconds);

/// One recording file inside a source. `samples` is the file exactly as it
/// was loaded and is never mutated; trimming only moves the
/// [BeginIndex(), EndIndex()) window that Source::Rebuild() feeds into the
/// lap splitter, so a trim is always undoable without re-reading the file.
struct SourceFile {
  std::string path;
  std::vector<GPSSample> samples;

  /// Samples dropped from the head and the tail. Clamped by BeginIndex()/
  /// EndIndex() rather than validated on assignment, so a UI can move them
  /// freely.
  size_t trim_begin = 0;
  size_t trim_end = 0;

  bool enabled = true;

  /// Non-empty when the file failed to load; `samples` is then empty and the
  /// entry is kept so the user can see (and fix, or remove) the path.
  std::string error;

  /// Clock chaining metadata from the loader; see GPSFileInfo.
  double fallback_span_s = 0;
  bool uses_fallback_clock = false;

  size_t BeginIndex() const;
  size_t EndIndex() const;
  size_t UsedCount() const;

  /// True when the receiver reported a horizontal accuracy for any sample.
  /// GPMF carries none, so a zero h_acc means "not reported" there and
  /// "impossibly good" nowhere.
  bool ReportsAccuracy() const;

  /// Index of the first sample at or after `timestamp_ms` (in the file's own
  /// clock), clamped to [0, samples.size()]. Samples are in time order, so
  /// this is what maps a dragged handle back onto a trim window.
  size_t IndexAtTimestamp(int64_t timestamp_ms) const;

  /// Timestamps of the first/last untrimmed sample, in ms. Zero when the
  /// trimmed window is empty. These are the file's own timestamps, before
  /// Rebuild() applies any cross-file offset.
  int64_t FirstTimestampMs() const;
  int64_t LastTimestampMs() const;
};

/// One recording session's worth of data: an ordered list of files that are
/// concatenated into a single sample stream, plus the reference track that
/// splits that stream into laps and sectors.
struct Source {
  int id = 0;
  std::string name;

  std::vector<SourceFile> files;

  /// Defines both the lap/sector timing lines and the coordinate frame the
  /// laps are measured in. Empty until a track is loaded, in which case the
  /// source has points but no laps.
  ReferenceTrack track;
  std::string track_path;

  /// Bumped whenever `track` changes -- a load, or a gate-extension tweak.
  /// Views and comparisons holding a copy of the track watch this to know
  /// when to re-adopt it.
  int track_generation = 0;

  Laps laps;

  /// Reads `path` and appends it as a new file. Returns false and fills
  /// `error` (if non-null) on failure; the entry is still appended, carrying
  /// the error message. Marks the source dirty either way.
  bool AddFile(const std::string &path, std::string *error = nullptr);

  /// Re-reads an already-listed file from disk, keeping its trim window.
  bool ReloadFile(size_t index, std::string *error = nullptr);

  void RemoveFile(size_t index);
  void MoveFile(size_t index, int delta);

  /// Loads a reference track and adopts its coordinate system as this
  /// source's frame. Returns false and fills `error` (if non-null) on
  /// failure, leaving the previous track in place.
  bool LoadTrack(const std::string &path, std::string *error = nullptr);

  /// Records an in-place edit of `track` (e.g. gate_extension_m): bumps the
  /// generation and schedules the rebuild that re-splits the laps.
  void TrackChanged();

  /// Schedules a Rebuild() on the next Update(). Call after changing a trim
  /// window, toggling a file, or editing track.gate_extension_m.
  void MarkDirty() { dirty_ = true; }
  bool Dirty() const { return dirty_; }

  /// Rebuilds the lap splitter's input from the enabled files' trimmed
  /// windows if anything changed since the last call. Returns true when a
  /// rebuild actually happened, so callers can refit views.
  bool Update();

  //------------------------------- QUERIES ---------------------------------//

  size_t LapsCount() const { return laps.LapsCount(); }
  bool HasTrack() const { return !track.segments.empty(); }

  /// Total samples across enabled files' trimmed windows -- i.e. what
  /// Rebuild() feeds into `laps`.
  size_t UsedSampleCount() const;

  /// Wall-clock span of the concatenated stream, in ms. {0, 0} when empty.
  std::pair<int64_t, int64_t> TimestampSpanMs() const;

  /// The same span over the files' whole sample ranges, ignoring trims. This
  /// is the domain a trimming view should draw: it doesn't move under the
  /// handles as they are dragged.
  std::pair<int64_t, int64_t> FullTimestampSpanMs() const;

  /// Trims off the ends of `files[index]` the samples the receiver hadn't
  /// settled on: fixes worse than `max_h_acc` metres, ones it reported no
  /// accuracy for, and repeats of one position (a stale fix held while it
  /// re-acquires). Leaves the file alone if that would trim everything.
  /// Returns the number of samples newly trimmed.
  size_t AutoTrim(size_t index, double max_h_acc = 5.0);

  /// Offset added to `files[index]`'s own timestamps when it is concatenated,
  /// in ms. Zero unless the file relies on the synthetic clock.
  int64_t FileOffsetMs(size_t index) const;

private:
  bool dirty_ = true;
};

/// A set of laps held side by side. The laps can come from any source, but
/// they all have to be on the same track: a delta is only meaningful when
/// every lap is resampled against the same gates.
struct Comparison {
  int id = 0;
  std::string name;

  /// In the order they were added. A lap's position here is what picks its
  /// colour, so the first lap dropped in is always the first colour --
  /// which a hash of the lap id would not be.
  std::vector<LapRef> laps;

  /// Adopted from the first lap added; empty until then.
  ReferenceTrack track;
  std::string track_path;

  bool HasTrack() const { return !track.segments.empty(); }

  /// Position of `ref` in `laps`, or -1.
  int IndexOf(LapRef ref) const;
  bool Contains(LapRef ref) const { return IndexOf(ref) >= 0; }
};

/// Everything the app has open: the sources being set up, and the
/// comparisons built from their laps.
struct Session {
  /// Held by pointer so a Source's address -- and the `Laps *` that views
  /// hold into it -- survives adding and removing other sources.
  std::vector<std::unique_ptr<Source>> sources;
  std::vector<std::unique_ptr<Comparison>> comparisons;

  /// Appends a source named "Source N", inheriting the previous source's
  /// reference track so a session on one circuit costs no extra clicks.
  Source *NewSource();

  Source *Find(int source_id);
  const Source *Find(int source_id) const;

  /// Position of `source_id` in `sources`, or -1.
  int IndexOf(int source_id) const;

  /// Removes the source and every lap of it from every comparison, so no
  /// comparison is left holding a LapRef that resolves to nothing.
  void Remove(int source_id);

  /// The lap `ref` names, or nullopt when the source is gone or the lap
  /// index no longer exists (a retrim can shorten a source).
  std::optional<Lap> ResolveLap(LapRef ref) const;

  /// Runs Update() on every source. Returns true if any rebuilt.
  bool Update();

  //------------------------------ COMPARISONS ------------------------------//

  Comparison *NewComparison();
  Comparison *FindComparison(int comparison_id);
  void RemoveComparison(int comparison_id);

  /// Why `ref` cannot join `comparison`, or empty when it can. An empty
  /// comparison takes any lap and adopts its source's track; after that
  /// only laps from sources on that same track are meaningful, since
  /// resampling them against different gates would produce a plausible
  /// looking delta out of two unrelated circuits.
  std::string WhyNotAddable(const Comparison &comparison, LapRef ref) const;

  /// Adds `ref` to `comparison`, adopting its source's track if this is the
  /// first lap. Returns false (and changes nothing) when WhyNotAddable
  /// gives a reason. Adding a lap that is already there is a no-op.
  bool AddLap(Comparison *comparison, LapRef ref);

  void RemoveLap(Comparison *comparison, LapRef ref);

  //------------------------------ PERSISTENCE ------------------------------//
  //
  // A session file records what was set up, not what was loaded: the file
  // paths and their trim windows, the tracks, and which laps each comparison
  // holds. Reopening re-reads the recordings from disk, so the file stays
  // small and never goes stale against an edited recording.

  /// Serializes this session to the JSON schema LoadFromString reads.
  std::string ToJsonString() const;

  /// Replaces this session's contents with `json`. Sources keep the ids they
  /// were saved with, so a restored session's windows land back where the
  /// layout file remembers them. Files that no longer exist come back as
  /// entries carrying their error, rather than vanishing.
  /// Throws std::runtime_error if the JSON does not parse.
  void LoadFromString(const std::string &json);

  /// Reads/writes a session file. Throw std::runtime_error on failure.
  void SaveToFile(const std::string &path) const;
  void LoadFromFile(const std::string &path);

private:
  int next_source_id_ = 1;
  int next_comparison_id_ = 1;
};

} // namespace pacer
