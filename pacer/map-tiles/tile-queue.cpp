#include "tile-queue.hpp"

#include <algorithm>

namespace pacer {

// 2s, 4s, 8s ... capped at a minute.
static TileRequestQueue::Clock::duration RetryDelay(int failures) {
  int seconds = 2 << std::min(failures - 1, 5);
  return std::chrono::seconds(std::min(seconds, 60));
}

void TileRequestQueue::Want(const Key &key, Clock::time_point now) {
  if (in_flight_.count(key) > 0)
    return;

  auto failure = failures_.find(key);
  if (failure != failures_.end() && now < failure->second.retry_at)
    return;

  // Already queued: refreshing the timestamp is the whole point of being
  // called again, it keeps the tile out of the stale sweep.
  auto [it, inserted] = wanted_.insert({key, now});
  it->second = now;
  if (inserted)
    queue_.push_back(key);
}

std::vector<TileRequestQueue::Key>
TileRequestQueue::TakeReady(Clock::time_point now) {
  // Nothing can start, so skip the sweep: callers poll this per requested
  // tile per frame, and stale entries only ever cost something once a slot
  // frees up -- which is exactly when the sweep below runs.
  if (in_flight_.size() >= kMaxInFlight)
    return {};

  auto stale =
      std::remove_if(queue_.begin(), queue_.end(), [&](const Key &key) {
        auto it = wanted_.find(key);
        if (it == wanted_.end())
          return true;
        if (now - it->second <= kStaleAfter)
          return false;
        wanted_.erase(it);
        return true;
      });
  queue_.erase(stale, queue_.end());

  std::vector<Key> ready;
  while (in_flight_.size() < kMaxInFlight && !queue_.empty()) {
    Key key = queue_.back();
    queue_.pop_back();
    wanted_.erase(key);
    in_flight_.insert(key);
    ready.push_back(key);
  }
  return ready;
}

void TileRequestQueue::Finish(const Key &key, bool ok, Clock::time_point now) {
  in_flight_.erase(key);
  if (ok) {
    failures_.erase(key);
    return;
  }
  Failure &failure = failures_[key];
  ++failure.count;
  failure.retry_at = now + RetryDelay(failure.count);
}

} // namespace pacer
