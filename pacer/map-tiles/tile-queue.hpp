#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace pacer {

/// Decides which tile downloads to start, which to drop, and when to retry
/// a failed one. Pure bookkeeping -- no network, no GL -- so the policy is
/// testable on its own; TileStore owns one of these and does the I/O.
///
/// The model is "what does the caller want on screen right now": callers
/// re-state their whole visible range every frame via Want(), and anything
/// that stops being re-stated is forgotten before it costs a request. That
/// way a long scroll or pan spends a bounded number of downloads on the
/// view the user ends up at, instead of one per tile crossed on the way.
class TileRequestQueue {
public:
  using Clock = std::chrono::steady_clock;
  using Key = std::tuple<int, int, int>; // zoom, x, y

  /// Downloads allowed to run at once.
  static constexpr size_t kMaxInFlight = 4;

  /// A queued tile is dropped once it has gone this long without being
  /// wanted again -- the view moved off it before it ever got a slot.
  /// Comfortably longer than a frame at the rate the apps render while
  /// tiles are outstanding.
  static constexpr std::chrono::milliseconds kStaleAfter{250};

  /// Records that the caller wants this tile as of `now`. Repeating the
  /// call every frame is what keeps a queued tile alive. Ignored while the
  /// tile is downloading or still backing off from a failure.
  void Want(const Key &key, Clock::time_point now);

  /// Drops queued tiles nobody has wanted recently, then returns the tiles
  /// whose downloads should start now -- most recently wanted first, and
  /// never more than the free in-flight slots. Every returned key counts as
  /// in flight until Finish() reports it. Cheap to call as often as tiles
  /// are wanted.
  std::vector<Key> TakeReady(Clock::time_point now);

  /// Reports a finished download. A failure backs the tile off (2s, 4s,
  /// 8s ... capped at a minute) so a tile the server refuses -- or one
  /// outside the imagery coverage, which 404s forever -- is not re-requested
  /// on every frame it stays on screen.
  void Finish(const Key &key, bool ok, Clock::time_point now);

  bool IsInFlight(const Key &key) const { return in_flight_.count(key) > 0; }
  size_t InFlightCount() const { return in_flight_.size(); }
  size_t QueuedCount() const { return queue_.size(); }
  size_t PendingCount() const { return InFlightCount() + QueuedCount(); }

private:
  struct Failure {
    int count = 0;
    Clock::time_point retry_at{};
  };

  /// Keys waiting for a slot, oldest first; taken from the back, so the
  /// tiles wanted most recently -- the ones under the viewport now -- go
  /// first.
  std::deque<Key> queue_;
  std::map<Key, Clock::time_point> wanted_;
  std::set<Key> in_flight_;
  std::map<Key, Failure> failures_;
};

} // namespace pacer
