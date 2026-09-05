#include <catch2/catch_test_macros.hpp>

#include <pacer/map-tiles/tile-queue.hpp>

using pacer::TileRequestQueue;
using Clock = TileRequestQueue::Clock;
using Key = TileRequestQueue::Key;

namespace {

Key Tile(int x) { return Key{19, x, 0}; }

// A frame's worth of wanting: what a caller does every frame for the tiles
// currently on screen.
std::vector<Key> Frame(TileRequestQueue &queue, Clock::time_point now,
                       int first, int count) {
  for (int i = 0; i < count; ++i)
    queue.Want(Tile(first + i), now);
  return queue.TakeReady(now);
}

} // namespace

TEST_CASE("no more than kMaxInFlight downloads run at once") {
  TileRequestQueue queue;
  auto now = Clock::time_point{};

  auto ready = Frame(queue, now, 0, 50);
  REQUIRE(ready.size() == TileRequestQueue::kMaxInFlight);
  REQUIRE(queue.InFlightCount() == TileRequestQueue::kMaxInFlight);
  REQUIRE(queue.PendingCount() == 50);

  // Asking again while the slots are full starts nothing new.
  REQUIRE(Frame(queue, now, 0, 50).empty());

  // A slot frees up, and exactly one tile takes it.
  queue.Finish(ready[0], true, now);
  REQUIRE(queue.TakeReady(now).size() == 1);
  REQUIRE(queue.InFlightCount() == TileRequestQueue::kMaxInFlight);
}

TEST_CASE("tiles wanted most recently are downloaded first") {
  TileRequestQueue queue;
  auto now = Clock::time_point{};

  // Fill every slot with tiles nobody will ask about again, then queue an
  // old batch and a newer one.
  auto in_flight = Frame(queue, now, 100, 4);
  REQUIRE(in_flight.size() == 4);

  for (int i = 0; i < 4; ++i)
    queue.Want(Tile(i), now); // older batch
  for (int i = 10; i < 14; ++i)
    queue.Want(Tile(i), now); // the view the user just moved to

  for (const auto &key : in_flight)
    queue.Finish(key, true, now);

  auto ready = queue.TakeReady(now);
  REQUIRE(ready.size() == 4);
  for (const auto &key : ready)
    REQUIRE(std::get<1>(key) >= 10);
}

TEST_CASE("queued tiles are dropped once the view moves off them") {
  TileRequestQueue queue;
  auto now = Clock::time_point{};

  auto in_flight = Frame(queue, now, 100, 4);
  for (int i = 0; i < 20; ++i)
    queue.Want(Tile(i), now);
  REQUIRE(queue.QueuedCount() == 20);

  // Later frames want a different tile: the panned-away 20 go stale and are
  // forgotten rather than downloaded.
  auto later =
      now + TileRequestQueue::kStaleAfter + std::chrono::milliseconds(1);
  queue.Want(Tile(500), later);
  for (const auto &key : in_flight)
    queue.Finish(key, true, later);

  auto ready = queue.TakeReady(later);
  REQUIRE(ready.size() == 1);
  REQUIRE(ready[0] == Tile(500));
  REQUIRE(queue.PendingCount() == 1);
}

TEST_CASE("a tile still being asked for survives the stale sweep") {
  TileRequestQueue queue;
  auto now = Clock::time_point{};

  auto in_flight = Frame(queue, now, 100, 4);
  queue.Want(Tile(0), now);

  // Kept alive across many frames, well past the stale window.
  auto later = now;
  for (int frame = 0; frame < 100; ++frame) {
    later += std::chrono::milliseconds(33);
    queue.Want(Tile(0), later);
    REQUIRE(queue.TakeReady(later).empty()); // slots still busy
  }
  REQUIRE(queue.QueuedCount() == 1);

  for (const auto &key : in_flight)
    queue.Finish(key, true, later);
  auto ready = queue.TakeReady(later);
  REQUIRE(ready.size() == 1);
  REQUIRE(ready[0] == Tile(0));
}

TEST_CASE("a failed tile backs off instead of retrying every frame") {
  TileRequestQueue queue;
  auto now = Clock::time_point{};

  auto ready = Frame(queue, now, 0, 1);
  REQUIRE(ready.size() == 1);
  queue.Finish(Tile(0), false, now);

  // The caller keeps the tile on screen: 60 frames of wanting, no retries.
  auto later = now;
  for (int frame = 0; frame < 60; ++frame) {
    later += std::chrono::milliseconds(33);
    queue.Want(Tile(0), later);
    REQUIRE(queue.TakeReady(later).empty());
  }
  REQUIRE(queue.PendingCount() == 0);

  // First backoff is 2s; after it the tile is retried once.
  auto retry = now + std::chrono::milliseconds(2001);
  queue.Want(Tile(0), retry);
  REQUIRE(queue.TakeReady(retry).size() == 1);

  // Second failure backs off further, so the 2s mark is too early now.
  queue.Finish(Tile(0), false, retry);
  auto too_soon = retry + std::chrono::milliseconds(2001);
  queue.Want(Tile(0), too_soon);
  REQUIRE(queue.TakeReady(too_soon).empty());

  auto after_second = retry + std::chrono::milliseconds(4001);
  queue.Want(Tile(0), after_second);
  REQUIRE(queue.TakeReady(after_second).size() == 1);

  // A success clears the history, so a later failure starts from 2s again.
  queue.Finish(Tile(0), true, after_second);
  queue.Finish(Tile(0), false, after_second);
  auto after_reset = after_second + std::chrono::milliseconds(2001);
  queue.Want(Tile(0), after_reset);
  REQUIRE(queue.TakeReady(after_reset).size() == 1);
}
