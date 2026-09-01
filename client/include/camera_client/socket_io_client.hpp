#pragma once

#include "camera_client/config.hpp"
#include "camera_client/frame.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace camera_client {

/** Parsed server acknowledgement for a camera:frame event. */
struct FrameAck {
  bool accepted{false};
  std::string reason;
};

/** Synchronous Engine.IO v4 / Socket.IO client over a WebSocket transport. */
class SocketIoClient {
 public:
  SocketIoClient();
  SocketIoClient(const SocketIoClient&) = delete;
  SocketIoClient& operator=(const SocketIoClient&) = delete;
  ~SocketIoClient();

  /**
   * Open a WebSocket and authenticate with the `/stream` namespace.
   *
   * @param config Server URL, camera ID, and secret camera token.
   * @throws std::runtime_error On DNS, TCP, WebSocket, protocol, or auth failure.
   */
  void connect(const Config& config);

  /**
   * Send one binary JPEG event and wait for its matching Socket.IO ACK.
   *
   * @param frame JPEG bytes and Unix epoch millisecond timestamp.
   * @return Server acceptance flag and optional rejection reason.
   * @throws std::runtime_error If disconnected or the protocol/transport fails.
   */
  FrameAck send_frame(const Frame& frame);

  /** Close the WebSocket without throwing. Safe to call repeatedly. */
  void disconnect() noexcept;

  /** @return true only while an authenticated namespace connection is usable. */
  [[nodiscard]] bool connected() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace camera_client
