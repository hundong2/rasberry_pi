#include "camera_client/config.hpp"
#include "camera_client/latest_frame_queue.hpp"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

/** @throws std::runtime_error when condition is false. */
void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

/** Build a small test frame with an identifiable payload byte. */
camera_client::Frame frame_with_value(std::uint8_t value) {
  return {.timestamp_ms = value, .jpeg = {0xFFU, 0xD8U, value, 0xFFU, 0xD9U}};
}

/** Verify that an unread old frame is replaced by the newest frame. */
void test_latest_frame_replacement() {
  camera_client::LatestFrameQueue queue;
  require(!queue.push(frame_with_value(1U)), "first push must not replace");
  require(queue.push(frame_with_value(2U)), "second push must replace first");
  const auto frame = queue.pop_for(std::chrono::milliseconds(1));
  require(frame.has_value(), "latest frame must be available");
  require(frame->timestamp_ms == 2, "queue must preserve only the latest frame");
}

/** Verify environment defaults and CLI precedence without exposing token values. */
void test_config_precedence() {
  // setenv() returns 0 on success and -1 on invalid input/allocation failure.
  require(::setenv("CAMERA_SERVER_URL", "http://environment:3000", 1) == 0,
          "setenv server failed");
  require(::setenv("CAMERA_TOKEN", "unit-test-token", 1) == 0,
          "setenv token failed");
  require(::setenv("CAMERA_FPS", "7", 1) == 0, "setenv fps failed");

  char executable[] = "camera-client-tests";
  char server_option[] = "--server";
  char server_value[] = "http://cli:3000";
  char fps_option[] = "--fps";
  char fps_value[] = "9";
  char* argv[] = {executable, server_option, server_value, fps_option, fps_value};
  const camera_client::Config config = camera_client::parse_config(5, argv);
  require(config.server_url == "http://cli:3000", "CLI server must win");
  require(config.fps == 9, "CLI FPS must win");
  require(config.token == "unit-test-token", "environment token must be read");
}

/** Verify server-compatible camera ID validation. */
void test_invalid_camera_id() {
  char executable[] = "camera-client-tests";
  char id_option[] = "--camera-id";
  char id_value[] = "invalid id";
  char* argv[] = {executable, id_option, id_value};
  bool rejected = false;
  try {
    static_cast<void>(camera_client::parse_config(3, argv));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "camera ID with a space must be rejected");
}

}  // namespace

/** @return 0 when every unit test passes, otherwise 1. */
int main() {
  try {
    test_latest_frame_replacement();
    test_config_precedence();
    test_invalid_camera_id();
    std::cout << "All camera client tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Test failure: " << error.what() << '\n';
    return 1;
  }
}
