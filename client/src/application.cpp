#include "camera_client/application.hpp"

#include "camera_client/camera_process.hpp"
#include "camera_client/latest_frame_queue.hpp"
#include "camera_client/socket_io_client.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>

namespace camera_client {
namespace {

std::mutex log_mutex;

/** Serialize a complete log line written by capture and main threads. */
void log_line(const char* level, const std::string& message) {
  std::lock_guard lock(log_mutex);
  std::cerr << '[' << level << "] " << message << '\n';
}

/**
 * Sleep interruptibly using short steady-clock intervals.
 *
 * @param duration Total requested wait.
 * @param stop_requested Signal-controlled application stop flag.
 */
void interruptible_sleep(std::chrono::milliseconds duration,
                         const std::atomic_bool& stop_requested) {
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (!stop_requested.load(std::memory_order_relaxed) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

}  // namespace

Application::Application(Config config) : config_(std::move(config)) {}

int Application::run(const std::atomic_bool& stop_requested) {
  LatestFrameQueue frames;
  CameraProcess camera;
  SocketIoClient socket;
  std::atomic_bool camera_failed{false};
  std::atomic_uint64_t captured{0U};
  std::atomic_uint64_t replaced{0U};
  std::uint64_t accepted = 0U;
  std::uint64_t rejected = 0U;

  try {
    camera.start(
        config_,
        [&](Frame frame) {
          captured.fetch_add(1U, std::memory_order_relaxed);
          if (frames.push(std::move(frame))) {
            replaced.fetch_add(1U, std::memory_order_relaxed);
          }
        },
        [&](const std::string& error) {
          if (stop_requested.load(std::memory_order_relaxed)) {
            return;  // systemd/timeout may terminate the child with the main process.
          }
          log_line("ERROR", error);
          camera_failed.store(true, std::memory_order_relaxed);
          frames.close();
        });
  } catch (const std::exception& error) {
    log_line("ERROR", std::string("camera startup failed: ") + error.what());
    return 2;
  }

  log_line("INFO", "camera capture started for " + config_.camera_id);
  std::chrono::seconds backoff{1};
  std::mt19937 random_engine(std::random_device{}());
  std::uniform_int_distribution<int> jitter_ms(0, 250);

  while (!stop_requested.load(std::memory_order_relaxed) &&
         !camera_failed.load(std::memory_order_relaxed)) {
    try {
      if (!socket.connected()) {
        log_line("INFO", "connecting to " + config_.server_url);
        socket.connect(config_);
        log_line("INFO", "Socket.IO /stream authentication succeeded");
        backoff = std::chrono::seconds(1);
      }

      auto frame = frames.pop_for(std::chrono::milliseconds(500));
      if (!frame) {
        continue;
      }

      const FrameAck ack = socket.send_frame(*frame);
      if (ack.accepted) {
        ++accepted;
      } else {
        ++rejected;
        if (ack.reason != "backpressure") {
          log_line("WARN", "server rejected frame: " + ack.reason);
        }
      }

      const std::uint64_t completed = accepted + rejected;
      if (completed > 0U && completed % 100U == 0U) {
        log_line("INFO", "stats captured=" +
                             std::to_string(captured.load(std::memory_order_relaxed)) +
                             " replaced=" +
                             std::to_string(replaced.load(std::memory_order_relaxed)) +
                             " accepted=" + std::to_string(accepted) +
                             " rejected=" + std::to_string(rejected));
      }
    } catch (const std::exception& error) {
      socket.disconnect();
      log_line("WARN", std::string("connection lost: ") + error.what());
      const auto delay = backoff + std::chrono::milliseconds(jitter_ms(random_engine));
      interruptible_sleep(delay, stop_requested);
      backoff = std::min(backoff * 2, std::chrono::seconds(30));
    }
  }

  frames.close();
  socket.disconnect();
  camera.stop();
  log_line("INFO", "stopped: captured=" +
                       std::to_string(captured.load(std::memory_order_relaxed)) +
                       " replaced=" +
                       std::to_string(replaced.load(std::memory_order_relaxed)) +
                       " accepted=" + std::to_string(accepted) +
                       " rejected=" + std::to_string(rejected));
  return camera_failed.load(std::memory_order_relaxed) ? 2 : 0;
}

}  // namespace camera_client
