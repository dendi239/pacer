#include "session.hpp"

#include <algorithm>
#include <format>

namespace pacer {

std::string LapRef::Label() const {
  if (!Valid())
    return "--";
  return std::format("F{}L{}", source_id, lap_index);
}

//------------------------------- SourceFile --------------------------------//

size_t SourceFile::BeginIndex() const {
  return std::min(trim_begin, samples.size());
}

size_t SourceFile::EndIndex() const {
  size_t dropped = std::min(trim_end, samples.size());
  return std::max(BeginIndex(), samples.size() - dropped);
}

size_t SourceFile::UsedCount() const { return EndIndex() - BeginIndex(); }

int64_t SourceFile::FirstTimestampMs() const {
  return UsedCount() ? samples[BeginIndex()].timestamp_ms : 0;
}

int64_t SourceFile::LastTimestampMs() const {
  return UsedCount() ? samples[EndIndex() - 1].timestamp_ms : 0;
}

bool SourceFile::ReportsAccuracy() const {
  for (const GPSSample &sample : samples) {
    if (sample.h_acc > 0)
      return true;
  }
  return false;
}

size_t SourceFile::IndexAtTimestamp(int64_t timestamp_ms) const {
  auto it = std::lower_bound(samples.begin(), samples.end(), timestamp_ms,
                             [](const GPSSample &sample, int64_t target) {
                               return sample.timestamp_ms < target;
                             });
  return (size_t)(it - samples.begin());
}

//---------------------------------- Source ---------------------------------//

bool Source::AddFile(const std::string &path, std::string *error) {
  files.push_back(SourceFile{.path = path});
  dirty_ = true;
  return ReloadFile(files.size() - 1, error);
}

bool Source::ReloadFile(size_t index, std::string *error) {
  if (index >= files.size())
    return false;

  SourceFile &file = files[index];
  file.samples.clear();
  file.error.clear();
  dirty_ = true;

  GPSFileInfo info;
  std::string message;
  bool ok = LoadGPSFile(
      file.path, [&](GPSSample sample) { file.samples.push_back(sample); },
      &info, &message);

  if (!ok) {
    file.samples.clear();
    file.error = message;
    if (error)
      *error = message;
    return false;
  }

  file.fallback_span_s = info.fallback_span_s;
  file.uses_fallback_clock = info.uses_fallback_clock;
  return true;
}

void Source::RemoveFile(size_t index) {
  if (index >= files.size())
    return;
  files.erase(files.begin() + index);
  dirty_ = true;
}

void Source::MoveFile(size_t index, int delta) {
  if (index >= files.size() || delta == 0)
    return;
  int target = static_cast<int>(index) + delta;
  if (target < 0 || target >= static_cast<int>(files.size()))
    return;
  std::swap(files[index], files[target]);
  dirty_ = true;
}

bool Source::LoadTrack(const std::string &path, std::string *error) {
  try {
    ReferenceTrack loaded = ReferenceTrack::FromFile(path);
    // Keep the runtime-only gate extension across reloads; FromFile resets
    // it to the schema default because the track file does not carry it.
    loaded.gate_extension_m = track.gate_extension_m;
    track = std::move(loaded);
    track_path = path;
    TrackChanged();
    return true;
  } catch (const std::exception &e) {
    if (error)
      *error = path + ": " + e.what();
    return false;
  }
}

void Source::TrackChanged() {
  ++track_generation;
  dirty_ = true;
}

bool Source::Update() {
  if (!dirty_)
    return false;
  dirty_ = false;

  laps.ClearPoints();

  // The frame has to be in place before the first AddPoint, which
  // accumulates distances through it. With no track there is no frame to
  // adopt yet; the display derives one from the data bounds and pushes it
  // back in, and loading a track later re-runs this whole rebuild.
  if (HasTrack()) {
    laps.SetCoordinateSystem(track.cs);
  }

  double offset_ms = 0;
  for (const SourceFile &file : files) {
    if (!file.enabled)
      continue;
    const int64_t shift = file.uses_fallback_clock
                              ? static_cast<int64_t>(offset_ms)
                              : static_cast<int64_t>(0);
    for (size_t i = file.BeginIndex(); i < file.EndIndex(); ++i) {
      GPSSample sample = file.samples[i];
      sample.timestamp_ms += shift;
      laps.AddPoint(sample);
    }
    offset_ms += file.fallback_span_s * 1000.0;
  }

  laps.sectors = HasTrack() ? track.BuildSectors(track.cs) : Sectors{};
  laps.Update();
  return true;
}

size_t Source::UsedSampleCount() const {
  size_t total = 0;
  for (const SourceFile &file : files) {
    if (file.enabled)
      total += file.UsedCount();
  }
  return total;
}

std::pair<int64_t, int64_t> Source::TimestampSpanMs() const {
  bool any = false;
  int64_t first = 0, last = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    const SourceFile &file = files[i];
    if (!file.enabled || file.UsedCount() == 0)
      continue;
    int64_t offset = FileOffsetMs(i);
    int64_t file_first = file.FirstTimestampMs() + offset;
    int64_t file_last = file.LastTimestampMs() + offset;
    if (!any) {
      first = file_first;
      last = file_last;
      any = true;
    } else {
      first = std::min(first, file_first);
      last = std::max(last, file_last);
    }
  }
  return {first, last};
}

std::pair<int64_t, int64_t> Source::FullTimestampSpanMs() const {
  bool any = false;
  int64_t first = 0, last = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    const SourceFile &file = files[i];
    if (!file.enabled || file.samples.empty())
      continue;
    int64_t offset = FileOffsetMs(i);
    int64_t file_first = file.samples.front().timestamp_ms + offset;
    int64_t file_last = file.samples.back().timestamp_ms + offset;
    if (!any) {
      first = file_first;
      last = file_last;
      any = true;
    } else {
      first = std::min(first, file_first);
      last = std::max(last, file_last);
    }
  }
  return {first, last};
}

size_t Source::AutoTrim(size_t index, double max_h_acc) {
  if (index >= files.size())
    return 0;
  SourceFile &file = files[index];
  const size_t count = file.samples.size();
  if (count == 0)
    return 0;

  const bool reports_accuracy = file.ReportsAccuracy();
  auto settled = [&](size_t i) {
    const GPSSample &sample = file.samples[i];
    if (reports_accuracy && (sample.h_acc <= 0 || sample.h_acc > max_h_acc))
      return false;
    // A receiver that has lost the fix keeps handing back the last one it
    // had, so the position stops moving *exactly* -- not merely slowly, as a
    // stationary car on a noisy fix would. Every sample in such a run is
    // stale, including the one that starts it, so both neighbours count.
    auto same_position = [&](size_t a, size_t b) {
      return file.samples[a].lat == file.samples[b].lat &&
             file.samples[a].lon == file.samples[b].lon;
    };
    if (i > 0 && same_position(i, i - 1))
      return false;
    if (i + 1 < count && same_position(i, i + 1))
      return false;
    return true;
  };

  size_t begin = 0;
  while (begin < count && !settled(begin))
    ++begin;
  if (begin == count) {
    // Nothing in the file passes; that is a judgement for the user to make,
    // not something to silently delete the recording over.
    return 0;
  }

  size_t end = count;
  while (end > begin + 1 && !settled(end - 1))
    --end;

  size_t trimmed = 0;
  if (begin > file.trim_begin) {
    trimmed += begin - file.trim_begin;
    file.trim_begin = begin;
  }
  size_t trim_end = count - end;
  if (trim_end > file.trim_end) {
    trimmed += trim_end - file.trim_end;
    file.trim_end = trim_end;
  }
  if (trimmed > 0)
    dirty_ = true;
  return trimmed;
}

int64_t Source::FileOffsetMs(size_t index) const {
  if (index >= files.size() || !files[index].uses_fallback_clock)
    return 0;
  double offset_ms = 0;
  for (size_t i = 0; i < index; ++i) {
    if (files[i].enabled)
      offset_ms += files[i].fallback_span_s * 1000.0;
  }
  return static_cast<int64_t>(offset_ms);
}

//--------------------------------- Session ---------------------------------//

Source *Session::NewSource() {
  auto source = std::make_unique<Source>();
  source->id = next_source_id_++;
  source->name = std::format("Source {}", source->id);
  if (!sources.empty()) {
    const Source &previous = *sources.back();
    source->track = previous.track;
    source->track_path = previous.track_path;
  }
  sources.push_back(std::move(source));
  return sources.back().get();
}

Source *Session::Find(int source_id) {
  for (auto &source : sources) {
    if (source->id == source_id)
      return source.get();
  }
  return nullptr;
}

const Source *Session::Find(int source_id) const {
  return const_cast<Session *>(this)->Find(source_id);
}

int Session::IndexOf(int source_id) const {
  for (size_t i = 0; i < sources.size(); ++i) {
    if (sources[i]->id == source_id)
      return static_cast<int>(i);
  }
  return -1;
}

void Session::Remove(int source_id) {
  std::erase_if(sources, [&](const std::unique_ptr<Source> &source) {
    return source->id == source_id;
  });
}

std::optional<Lap> Session::ResolveLap(LapRef ref) const {
  const Source *source = Find(ref.source_id);
  if (!source || ref.lap_index < 0 ||
      static_cast<size_t>(ref.lap_index) >= source->laps.LapsCount()) {
    return std::nullopt;
  }
  return source->laps.GetLap(ref.lap_index);
}

bool Session::Update() {
  bool any = false;
  for (auto &source : sources) {
    any |= source->Update();
  }
  return any;
}

} // namespace pacer
