#pragma once

#include "camera_client/config.hpp"
#include "camera_client/frame.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <sys/types.h>

namespace camera_client {

/** Owns an rpicam-vid child process and converts its MJPEG stdout into frames. */
class CameraProcess {
 public:
  using FrameHandler = std::function<void(Frame)>;
  using ErrorHandler = std::function<void(const std::string&)>;

  CameraProcess() = default;
  CameraProcess(const CameraProcess&) = delete;
  CameraProcess& operator=(const CameraProcess&) = delete;
  ~CameraProcess();

  /**
   * Start camera capture on a joinable background thread.
   *
   * @param config Validated width, height, FPS, and JPEG quality values.
   * @param on_frame Called once for each complete JPEG; receives ownership.
   * @param on_error Called for startup/read failures; contains no secret values.
   * @throws std::logic_error If capture is already running.
   */
  void start(const Config& config, FrameHandler on_frame, ErrorHandler on_error);

  /** Request child termination and join the capture thread. Safe to call repeatedly. */
  void stop();

 private:
  void capture_loop(Config config, FrameHandler on_frame, ErrorHandler on_error,
                    std::stop_token stop_token);

  std::mutex child_mutex_;
  pid_t child_pid_{-1};
  std::jthread thread_;
};

}  // namespace camera_client
