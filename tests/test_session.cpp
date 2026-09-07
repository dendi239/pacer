#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <format>

#include <pacer/session/session.hpp>

namespace {

// A file of `count` samples, one every 40 ms starting at `first_ms`, walking
// east one meter at a time from (51, 0).
pacer::SourceFile MakeFile(const std::string &path, size_t count,
                           int64_t first_ms) {
  pacer::SourceFile file{.path = path};
  for (size_t i = 0; i < count; ++i) {
    file.samples.push_back(pacer::GPSSample{
        .lat = 51.0,
        .lon = 0.0 + 1e-5 * (double)i,
        .timestamp_ms = first_ms + (int64_t)i * 40,
    });
  }
  return file;
}

// Timestamps of every point the source fed into the lap splitter, i.e. what
// Rebuild() produced.
std::vector<int64_t> PointTimestamps(const pacer::Source &source) {
  std::vector<int64_t> out;
  for (size_t i = 0; i < source.laps.PointCount(); ++i) {
    out.push_back(source.laps.GetPoint(i).timestamp_ms);
  }
  return out;
}

} // namespace

TEST_CASE("Trim window clamps instead of underflowing", "[session][trim]") {
  pacer::SourceFile file = MakeFile("a.dat", 10, 0);

  SECTION("untrimmed") {
    REQUIRE(file.BeginIndex() == 0);
    REQUIRE(file.EndIndex() == 10);
    REQUIRE(file.UsedCount() == 10);
  }

  SECTION("both ends") {
    file.trim_begin = 2;
    file.trim_end = 3;
    REQUIRE(file.BeginIndex() == 2);
    REQUIRE(file.EndIndex() == 7);
    REQUIRE(file.FirstTimestampMs() == 80);
    REQUIRE(file.LastTimestampMs() == 240);
  }

  SECTION("overlapping trims leave nothing rather than wrapping") {
    file.trim_begin = 8;
    file.trim_end = 8;
    REQUIRE(file.BeginIndex() == 8);
    REQUIRE(file.EndIndex() == 8);
    REQUIRE(file.UsedCount() == 0);
    REQUIRE(file.FirstTimestampMs() == 0);
  }

  SECTION("trims past the end are harmless") {
    file.trim_begin = 99;
    file.trim_end = 99;
    REQUIRE(file.UsedCount() == 0);
  }
}

TEST_CASE("Rebuild concatenates the enabled files' trimmed windows",
          "[session][rebuild]") {
  pacer::Source source;
  source.files.push_back(MakeFile("a.dat", 5, 1000));
  source.files.push_back(MakeFile("b.dat", 5, 2000));
  source.MarkDirty();

  REQUIRE(source.Update());
  REQUIRE(source.UsedSampleCount() == 10);
  REQUIRE(source.laps.PointCount() == 10);

  SECTION("a second Update is a no-op until something changes") {
    REQUIRE_FALSE(source.Update());
    source.MarkDirty();
    REQUIRE(source.Update());
  }

  SECTION("disabling a file drops its samples") {
    source.files[0].enabled = false;
    source.MarkDirty();
    source.Update();
    REQUIRE(source.UsedSampleCount() == 5);
    REQUIRE(PointTimestamps(source).front() == 2000);
  }

  SECTION("trimming drops samples from the ends of that file only") {
    source.files[0].trim_begin = 2;
    source.files[1].trim_end = 1;
    source.MarkDirty();
    source.Update();

    auto timestamps = PointTimestamps(source);
    REQUIRE(timestamps.size() == 7);
    REQUIRE(timestamps.front() == 1080);
    REQUIRE(timestamps[3] == 2000);
    REQUIRE(timestamps.back() == 2120);
  }

  SECTION("reordering reorders the stream") {
    source.MoveFile(1, -1);
    source.Update();
    REQUIRE(PointTimestamps(source).front() == 2000);
  }
}

TEST_CASE("Files on the synthetic clock are chained, timestamped ones are not",
          "[session][clock]") {
  pacer::Source source;

  // Two clips whose samples were stamped from the MP4 chunk spans, so both
  // start near zero and only the concatenation order tells them apart.
  source.files.push_back(MakeFile("clip1.MP4", 5, 0));
  source.files.back().uses_fallback_clock = true;
  source.files.back().fallback_span_s = 10;

  source.files.push_back(MakeFile("clip2.MP4", 5, 0));
  source.files.back().uses_fallback_clock = true;
  source.files.back().fallback_span_s = 10;

  source.MarkDirty();
  source.Update();

  auto timestamps = PointTimestamps(source);
  REQUIRE(timestamps.size() == 10);
  REQUIRE(timestamps[0] == 0);
  REQUIRE(timestamps[4] == 160);
  // Second clip shifted past the first, so the stream stays ordered.
  REQUIRE(timestamps[5] == 10'000);
  REQUIRE(timestamps[9] == 10'160);
  REQUIRE(source.FileOffsetMs(1) == 10'000);

  SECTION("disabling the first clip closes the gap it left") {
    source.files[0].enabled = false;
    source.MarkDirty();
    source.Update();
    REQUIRE(source.FileOffsetMs(1) == 0);
    REQUIRE(PointTimestamps(source).front() == 0);
  }

  SECTION("a file carrying real timestamps is left where it is") {
    source.files.push_back(MakeFile("logger.dat", 5, 500'000));
    source.MarkDirty();
    source.Update();
    REQUIRE(source.FileOffsetMs(2) == 0);
    REQUIRE(PointTimestamps(source).back() == 500'160);
  }
}

TEST_CASE("Auto-trim drops the ends the receiver hadn't settled on",
          "[session][trim]") {
  pacer::Source source;

  SECTION("bad accuracy at both ends") {
    source.files.push_back(MakeFile("a.dat", 10, 0));
    pacer::SourceFile &file = source.files[0];
    for (auto &sample : file.samples)
      sample.h_acc = 1.0;
    file.samples[0].h_acc = 20.0;
    file.samples[1].h_acc = 20.0;
    file.samples[9].h_acc = 0.0; // reported nothing

    REQUIRE(source.AutoTrim(0) == 3);
    REQUIRE(file.trim_begin == 2);
    REQUIRE(file.trim_end == 1);
    REQUIRE(file.UsedCount() == 7);
  }

  SECTION("a stale fix, held while the receiver re-acquires") {
    // No h_acc anywhere, as GPMF gives -- so only the repeated position
    // gives the stale head away.
    source.files.push_back(MakeFile("clip.MP4", 10, 0));
    pacer::SourceFile &file = source.files[0];
    for (size_t i = 1; i < 4; ++i)
      file.samples[i].lon = file.samples[0].lon;

    REQUIRE(source.AutoTrim(0) == 4);
    REQUIRE(file.trim_begin == 4);
    REQUIRE(file.trim_end == 0);
  }

  SECTION("a file that is bad throughout is left for the user to judge") {
    source.files.push_back(MakeFile("bad.dat", 10, 0));
    for (auto &sample : source.files[0].samples)
      sample.h_acc = 50.0;

    REQUIRE(source.AutoTrim(0) == 0);
    REQUIRE(source.files[0].UsedCount() == 10);
  }

  SECTION("never widens a trim the user already made") {
    source.files.push_back(MakeFile("a.dat", 10, 0));
    pacer::SourceFile &file = source.files[0];
    for (auto &sample : file.samples)
      sample.h_acc = 1.0;
    file.trim_begin = 5;

    REQUIRE(source.AutoTrim(0) == 0);
    REQUIRE(file.trim_begin == 5);
  }
}

TEST_CASE("The plot domain ignores trims so handles stay put",
          "[session][trim]") {
  pacer::Source source;
  source.files.push_back(MakeFile("a.dat", 10, 1000));
  source.MarkDirty();
  source.Update();

  auto [full_begin, full_end] = source.FullTimestampSpanMs();
  REQUIRE(full_begin == 1000);
  REQUIRE(full_end == 1360);

  source.files[0].trim_begin = 4;
  source.MarkDirty();
  source.Update();

  REQUIRE(source.TimestampSpanMs().first == 1160);
  // The domain a trimming view draws is unchanged by the trim itself.
  REQUIRE(source.FullTimestampSpanMs() ==
          std::pair<int64_t, int64_t>{1000, 1360});
}

TEST_CASE("Sources are addressed by id, not position", "[session][lapref]") {
  pacer::Session session;
  pacer::Source *first = session.NewSource();
  pacer::Source *second = session.NewSource();

  REQUIRE(first->id != second->id);
  REQUIRE(session.IndexOf(second->id) == 1);

  session.Remove(first->id);
  REQUIRE(session.Find(first->id) == nullptr);
  // Still the same source, now at a different position.
  REQUIRE(session.Find(second->id) == second);
  REQUIRE(session.IndexOf(second->id) == 0);

  REQUIRE(pacer::LapRef{.source_id = 1, .lap_index = 2}.Label() == "F1L2");
  REQUIRE_FALSE(session.ResolveLap({.source_id = 99, .lap_index = 0}));
  // No track, so no laps to resolve even for a live source.
  REQUIRE_FALSE(session.ResolveLap({.source_id = second->id, .lap_index = 0}));
}

TEST_CASE("A comparison holds laps from any source on its track",
          "[session][comparison]") {
  pacer::Session session;
  pacer::Source *first = session.NewSource();
  REQUIRE(first->LoadTrack("tracks/daytona-milton-keynes.json"));
  first->files.push_back(MakeFile("a.dat", 5, 0));
  first->MarkDirty();
  first->Update();

  // A second source on the same track, and a third on another one.
  pacer::Source *same_track = session.NewSource(); // inherits the track
  pacer::Source *other_track = session.NewSource();
  REQUIRE(other_track->LoadTrack("tracks/llandow.json"));

  // Laps only exist once data crosses the start line, which the synthetic
  // files above never do -- so reach past AddLap's lap-index check by
  // asking WhyNotAddable about the track instead.
  pacer::Comparison *comparison = session.NewComparison();
  REQUIRE(comparison->laps.empty());
  REQUIRE_FALSE(comparison->HasTrack());

  SECTION("an empty comparison adopts the first lap's track") {
    // Nothing to adopt while the source has no laps.
    REQUIRE(session.WhyNotAddable(*comparison, {first->id, 0}) ==
            "that lap no longer exists");
  }

  SECTION("a source with no track cannot contribute") {
    pacer::Source *untracked = session.NewSource();
    untracked->track = pacer::ReferenceTrack{};
    untracked->track_path.clear();
    REQUIRE(session.WhyNotAddable(*comparison, {untracked->id, 0}) ==
            std::format("{} has no reference track", untracked->name));
  }

  SECTION("laps from a different track are refused once one is adopted") {
    // Adopt a track directly, as the first successful drop would.
    comparison->track = first->track;
    comparison->track_path = first->track_path;
    comparison->laps.push_back({first->id, 0});

    REQUIRE(session.WhyNotAddable(*comparison, {same_track->id, 0}) ==
            "that lap no longer exists");
    REQUIRE(session.WhyNotAddable(*comparison, {other_track->id, 0}) ==
            std::format("{} is on a different track", other_track->name));
  }

  SECTION("emptying a comparison frees it to adopt another track") {
    comparison->track = first->track;
    comparison->track_path = first->track_path;
    comparison->laps.push_back({first->id, 0});

    session.RemoveLap(comparison, {first->id, 0});
    REQUIRE_FALSE(comparison->HasTrack());
    REQUIRE(comparison->track_path.empty());
  }

  SECTION("removing a source drops its laps from every comparison") {
    comparison->track = first->track;
    comparison->track_path = first->track_path;
    comparison->laps = {{first->id, 0}, {same_track->id, 3}};

    session.Remove(first->id);
    REQUIRE(comparison->laps.size() == 1);
    REQUIRE(comparison->laps[0] == pacer::LapRef{same_track->id, 3});
  }
}

TEST_CASE("Lap times read like a timing screen", "[session]") {
  REQUIRE(pacer::FormatLapTime(67.104) == "1:07.104");
  REQUIRE(pacer::FormatLapTime(9.5) == "0:09.500");
  REQUIRE(pacer::FormatLapTime(0) == "--");
}

TEST_CASE("A new source inherits the previous one's track", "[session]") {
  pacer::Session session;
  pacer::Source *first = session.NewSource();
  REQUIRE(first->LoadTrack("tracks/daytona-milton-keynes.json"));

  pacer::Source *second = session.NewSource();
  REQUIRE(second->track_path == first->track_path);
  REQUIRE(second->track.segments.size() == first->track.segments.size());
  REQUIRE(second->HasTrack());
}
