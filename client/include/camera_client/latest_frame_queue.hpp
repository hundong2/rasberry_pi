#pragma once

#include "camera_client/frame.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

namespace camera_client {

/**
 * Thread-safe queue that stores at most one frame.
 *
 * A new push replaces an unconsumed frame. This bounds memory and prioritizes
 * low latency over delivery of stale camera data.
 */
class LatestFrameQueue {
 public:
  /**
   * Store the newest frame and wake one waiting consumer.
   *
   * @param frame JPEG frame whose vector ownership is moved into the queue.
   * @return true if an older unconsumed frame was replaced; false otherwise.
   */
  bool push(Frame frame) {
    std::lock_guard lock(mutex_);  // lock_guard releases mutex_ on scope exit.
    if (closed_) {
      return false;  // A closed queue rejects data without changing its state.
    }

    const bool replaced = latest_.has_value();
    latest_ = std::move(frame);  // Move avoids copying the potentially large JPEG.
    ready_.notify_one();         // notify_one() has no return value.
    return replaced;
  }

  /**
   * Wait for and consume the current newest frame.
   *
   * @param timeout Maximum steady-clock duration to block the calling thread.
   * @return The newest frame, or std::nullopt after timeout/queue closure.
   */
  std::optional<Frame> pop_for(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);  // unique_lock is required by condition_variable.
    const bool ready = ready_.wait_for(lock, timeout, [this] {
      return closed_ || latest_.has_value();
    });
    if (!ready || !latest_) {
      return std::nullopt;  // wait_for() returns false when its timeout expires.
    }

    Frame result = std::move(*latest_);
    latest_.reset();  // reset() destroys the moved-from optional value.
    return result;
  }

  /** Close the queue and wake all waiting consumers. This operation is idempotent. */
  void close() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    latest_.reset();
    ready_.notify_all();  // notify_all() has no return value.
  }

 private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::optional<Frame> latest_;
  bool closed_{false};
};

}  // namespace camera_client
