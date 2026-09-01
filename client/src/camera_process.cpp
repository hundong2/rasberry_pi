#include "camera_client/camera_process.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace camera_client {
namespace {

constexpr std::size_t kReadBufferBytes = 64U * 1024U;
constexpr std::size_t kMaximumBufferedBytes = 4U * 1024U * 1024U;

/** @return Current Unix epoch time in milliseconds. */
std::int64_t epoch_milliseconds() {
  const auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             now.time_since_epoch())
      .count();
}

/**
 * Find a two-byte marker in a byte vector.
 *
 * @param bytes Buffer to scan.
 * @param first First marker byte.
 * @param second Second marker byte.
 * @param offset Initial scan offset.
 * @return Marker index, or bytes.size() when not found.
 */
std::size_t find_marker(const std::vector<std::uint8_t>& bytes,
                        std::uint8_t first, std::uint8_t second,
                        std::size_t offset = 0U) {
  if (bytes.size() < 2U || offset >= bytes.size() - 1U) {
    return bytes.size();
  }
  for (std::size_t index = offset; index + 1U < bytes.size(); ++index) {
    if (bytes[index] == first && bytes[index + 1U] == second) {
      return index;
    }
  }
  return bytes.size();
}

}  // namespace

CameraProcess::~CameraProcess() { stop(); }

void CameraProcess::start(const Config& config, FrameHandler on_frame,
                          ErrorHandler on_error) {
  if (thread_.joinable()) {
    throw std::logic_error("camera capture is already running");
  }
  // jthread passes its stop_token as the first callable argument and joins on destruction.
  thread_ = std::jthread(
      [this, config, on_frame = std::move(on_frame),
       on_error = std::move(on_error)](std::stop_token stop_token) mutable {
        capture_loop(config, std::move(on_frame), std::move(on_error), stop_token);
      });
}

void CameraProcess::stop() {
  if (!thread_.joinable()) {
    return;
  }

  thread_.request_stop();  // request_stop() returns false only if already requested.
  {
    std::lock_guard lock(child_mutex_);
    if (child_pid_ > 0) {
      // kill() returns 0 on signal delivery; ESRCH means the child already exited.
      if (::kill(child_pid_, SIGTERM) != 0 && errno != ESRCH) {
        // Shutdown must continue even if the child cannot receive SIGTERM.
      }
    }
  }
  thread_.join();  // join() blocks until capture_loop has closed and reaped the child.
}

void CameraProcess::capture_loop(Config config, FrameHandler on_frame,
                                 ErrorHandler on_error,
                                 std::stop_token stop_token) {
  int pipe_fds[2] = {-1, -1};
  if (::pipe(pipe_fds) != 0) {  // pipe() returns 0 and two FDs on success.
    on_error("pipe() failed: " + std::string(std::strerror(errno)));
    return;
  }

  const pid_t pid = ::fork();  // fork() returns child PID to parent, 0 to child, -1 on error.
  if (pid < 0) {
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    on_error("fork() failed: " + std::string(std::strerror(errno)));
    return;
  }

  if (pid == 0) {
    ::close(pipe_fds[0]);
    // dup2() returns the destination FD on success and -1 on failure.
    if (::dup2(pipe_fds[1], STDOUT_FILENO) < 0) {
      _exit(126);  // _exit() avoids flushing buffers copied from the parent.
    }
    ::close(pipe_fds[1]);

    const std::string width = std::to_string(config.width);
    const std::string height = std::to_string(config.height);
    const std::string fps = std::to_string(config.fps);
    const std::string quality = std::to_string(config.jpeg_quality);
    ::execlp("rpicam-vid", "rpicam-vid", "--nopreview", "--timeout", "0",
             "--codec", "mjpeg", "--width", width.c_str(), "--height",
             height.c_str(), "--framerate", fps.c_str(), "--quality",
             quality.c_str(), "--output", "-", static_cast<char*>(nullptr));
    _exit(127);  // execlp() returns only when executable lookup/start fails.
  }

  ::close(pipe_fds[1]);
  {
    std::lock_guard lock(child_mutex_);
    child_pid_ = pid;
  }

  std::array<std::uint8_t, kReadBufferBytes> read_buffer{};
  std::vector<std::uint8_t> pending;
  pending.reserve(512U * 1024U);  // reserve() changes capacity, not logical size.

  while (!stop_token.stop_requested()) {
    const ssize_t count = ::read(pipe_fds[0], read_buffer.data(), read_buffer.size());
    if (count == 0) {
      if (!stop_token.stop_requested()) {
        on_error("rpicam-vid closed its output stream");
      }
      break;
    }
    if (count < 0) {
      if (errno == EINTR) {
        continue;  // A signal interrupted read(); retry unless stop was requested.
      }
      on_error("read() failed: " + std::string(std::strerror(errno)));
      break;
    }

    const auto valid_bytes = static_cast<std::size_t>(count);
    pending.insert(pending.end(), read_buffer.begin(),
                   read_buffer.begin() + static_cast<std::ptrdiff_t>(valid_bytes));

    while (true) {
      const std::size_t start = find_marker(pending, 0xFFU, 0xD8U);
      if (start == pending.size()) {
        if (pending.size() > 1U) {
          pending.erase(pending.begin(), pending.end() - 1);
        }
        break;
      }

      const std::size_t end = find_marker(pending, 0xFFU, 0xD9U, start + 2U);
      if (end == pending.size()) {
        if (start > 0U) {
          pending.erase(pending.begin(),
                        pending.begin() + static_cast<std::ptrdiff_t>(start));
        }
        break;
      }

      const std::size_t frame_end = end + 2U;
      Frame frame;
      frame.timestamp_ms = epoch_milliseconds();
      frame.jpeg.assign(
          pending.begin() + static_cast<std::ptrdiff_t>(start),
          pending.begin() + static_cast<std::ptrdiff_t>(frame_end));
      on_frame(std::move(frame));
      pending.erase(pending.begin(),
                    pending.begin() + static_cast<std::ptrdiff_t>(frame_end));
    }

    if (pending.size() > kMaximumBufferedBytes) {
      pending.clear();  // A malformed stream cannot grow memory without bound.
      on_error("MJPEG parser discarded an oversized incomplete frame");
    }
  }

  ::close(pipe_fds[0]);  // close() returns 0 on success; no recovery is needed here.
  if (!stop_token.stop_requested()) {
    ::kill(pid, SIGTERM);
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    // waitpid() returns the child PID when reaped and -1 when interrupted/error.
  }
  {
    std::lock_guard lock(child_mutex_);
    child_pid_ = -1;
  }
}

}  // namespace camera_client
