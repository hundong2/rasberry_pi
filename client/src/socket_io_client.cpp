#include "camera_client/socket_io_client.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace camera_client {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using Json = nlohmann::json;

namespace {

/** Host and port extracted from the configured HTTP URL. */
struct ServerEndpoint {
  std::string host;
  std::string port;
};

/**
 * Parse the MVP's supported http://host[:port] URL form.
 *
 * @param url Validated server URL.
 * @return Host and explicit/default port used by TCP resolution.
 * @throws std::runtime_error For an empty host or unsupported URL path.
 */
ServerEndpoint parse_server_url(std::string_view url) {
  constexpr std::string_view prefix = "http://";
  if (!url.starts_with(prefix)) {
    throw std::runtime_error("only http:// server URLs are supported");
  }

  std::string_view authority = url.substr(prefix.size());
  const std::size_t slash = authority.find('/');
  if (slash != std::string_view::npos) {
    const std::string_view path = authority.substr(slash);
    if (path != "/") {
      throw std::runtime_error("server URL must not contain a path");
    }
    authority = authority.substr(0U, slash);
  }
  if (authority.empty()) {
    throw std::runtime_error("server URL host is empty");
  }

  const std::size_t colon = authority.rfind(':');
  if (colon == std::string_view::npos) {
    return {std::string(authority), "80"};
  }
  if (colon == 0U || colon + 1U >= authority.size()) {
    throw std::runtime_error("server URL has an invalid host or port");
  }
  return {std::string(authority.substr(0U, colon)),
          std::string(authority.substr(colon + 1U))};
}

/** @return Socket.IO auth JSON. nlohmann::json escapes all string values. */
std::string auth_json(const Config& config) {
  const Json auth = {{"role", "camera"},
                     {"cameraId", config.camera_id},
                     {"token", config.token}};
  return auth.dump();  // dump() returns a compact UTF-8 JSON string.
}

/** @return Socket.IO binary-event JSON with one attachment placeholder. */
std::string frame_event_json(const Frame& frame) {
  Json placeholder = {{"_placeholder", true}, {"num", 0}};
  Json payload = {{"timestamp", frame.timestamp_ms},
                  {"frame", std::move(placeholder)}};
  Json event = Json::array({"camera:frame", std::move(payload)});
  return event.dump();
}

}  // namespace

class SocketIoClient::Impl {
 public:
  /** Open the TCP/WebSocket layers and complete Engine.IO + namespace handshakes. */
  void connect(const Config& config) {
    disconnect();
    const ServerEndpoint endpoint = parse_server_url(config.server_url);

    tcp::resolver resolver(io_context_);
    // resolve() returns all matching TCP endpoints or throws boost::system::system_error.
    const auto endpoints = resolver.resolve(endpoint.host, endpoint.port);

    socket_ = std::make_unique<websocket::stream<beast::tcp_stream>>(io_context_);
    beast::get_lowest_layer(*socket_).expires_after(std::chrono::seconds(8));
    // connect() returns the selected endpoint or throws on DNS/network failure.
    beast::get_lowest_layer(*socket_).connect(endpoints);

    socket_->set_option(websocket::stream_base::timeout::suggested(
        beast::role_type::client));
    socket_->read_message_max(4U * 1024U * 1024U);
    socket_->set_option(websocket::stream_base::decorator(
        [](websocket::request_type& request) {
          request.set(boost::beast::http::field::user_agent,
                      "rasberry-pi-camera-client/1.0");
        }));

    const std::string host_header = endpoint.host + ":" + endpoint.port;
    // handshake() returns void and throws unless HTTP Upgrade succeeds.
    socket_->handshake(host_header, "/socket.io/?EIO=4&transport=websocket");

    const Message open_packet = read_message();
    if (!open_packet.text || !open_packet.data.starts_with('0')) {
      throw std::runtime_error("invalid Engine.IO open packet");
    }

    write_text("40/stream," + auth_json(config));
    for (;;) {
      const Message packet = read_message();
      if (!packet.text) {
        continue;
      }
      if (packet.data == "2") {
        write_text("3");  // Engine.IO ping type 2 is answered by pong type 3.
        continue;
      }
      if (packet.data.starts_with("40/stream,")) {
        authenticated_ = true;
        next_ack_id_ = 0U;
        return;
      }
      if (packet.data.starts_with("44/stream,")) {
        throw std::runtime_error("Socket.IO namespace authentication failed");
      }
    }
  }

  /** Send the binary event's descriptor and attachment, then parse its ACK. */
  FrameAck send_frame(const Frame& frame) {
    if (!authenticated_ || socket_ == nullptr) {
      throw std::runtime_error("Socket.IO client is not connected");
    }
    if (frame.jpeg.size() < 4U || frame.jpeg[0] != 0xFFU ||
        frame.jpeg[1] != 0xD8U || frame.jpeg[frame.jpeg.size() - 2U] != 0xFFU ||
        frame.jpeg.back() != 0xD9U) {
      throw std::runtime_error("capture produced an invalid JPEG boundary");
    }

    const std::uint64_t ack_id = next_ack_id_++;
    // Packet: Engine.IO message(4), Socket.IO binary event(5), one attachment(1-),
    // namespace, ACK id, then the JSON event array.
    const std::string descriptor = "451-/stream," + std::to_string(ack_id) +
                                   frame_event_json(frame);
    write_text(descriptor);
    write_binary(frame.jpeg);

    const std::string expected_prefix =
        "43/stream," + std::to_string(ack_id);
    for (;;) {
      const Message packet = read_message();
      if (!packet.text) {
        continue;
      }
      if (packet.data == "2") {
        write_text("3");
        continue;
      }
      if (!packet.data.starts_with(expected_prefix)) {
        continue;
      }

      const std::size_t json_offset = packet.data.find('[', expected_prefix.size());
      if (json_offset == std::string::npos) {
        throw std::runtime_error("Socket.IO ACK has no JSON argument array");
      }
      // parse() returns a JSON value or throws parse_error for malformed data.
      const Json arguments = Json::parse(packet.data.substr(json_offset));
      if (!arguments.is_array() || arguments.empty() ||
          !arguments.front().is_object()) {
        throw std::runtime_error("Socket.IO ACK has an unexpected shape");
      }

      const Json& result = arguments.front();
      FrameAck ack;
      ack.accepted = result.value("accepted", false);
      ack.reason = result.value("reason", std::string{});
      return ack;
    }
  }

  /** Close protocol and transport layers while swallowing shutdown errors. */
  void disconnect() noexcept {
    authenticated_ = false;
    if (socket_ == nullptr) {
      return;
    }
    try {
      if (socket_->is_open()) {
        write_text("41/stream,");
        // close() sends and waits for a WebSocket close handshake; it may throw.
        socket_->close(websocket::close_code::normal);
      }
    } catch (...) {
      // Destructors and retry cleanup must not hide the original network error.
    }
    socket_.reset();
  }

  /** @return Whether the namespace handshake completed and socket remains open. */
  [[nodiscard]] bool connected() const noexcept {
    return authenticated_ && socket_ != nullptr && socket_->is_open();
  }

 private:
  struct Message {
    bool text{false};
    std::string data;
  };

  /** @return One complete WebSocket message and whether its opcode was text. */
  Message read_message() {
    if (socket_ == nullptr) {
      throw std::runtime_error("cannot read from a closed WebSocket");
    }
    beast::flat_buffer buffer;
    // read() returns transferred bytes and throws on timeout/close/protocol failure.
    socket_->read(buffer);
    return {socket_->got_text(), beast::buffers_to_string(buffer.data())};
  }

  /** @param message Engine.IO/Socket.IO text packet to send atomically. */
  void write_text(const std::string& message) {
    if (socket_ == nullptr) {
      throw std::runtime_error("cannot write to a closed WebSocket");
    }
    socket_->text(true);  // text(true) selects a UTF-8 WebSocket text opcode.
    socket_->write(asio::buffer(message));  // write() returns transferred bytes.
  }

  /**
   * Send an Engine.IO binary message payload.
   *
   * @param bytes Raw Socket.IO attachment. WebSocket binary opcode itself tells
   *              Engine.IO this is a message packet, so no ASCII '4' prefix is used.
   */
  void write_binary(const std::vector<std::uint8_t>& bytes) {
    if (socket_ == nullptr) {
      throw std::runtime_error("cannot write to a closed WebSocket");
    }
    socket_->binary(true);  // binary(true) selects a WebSocket binary opcode.
    socket_->write(asio::buffer(bytes));
  }

  asio::io_context io_context_;
  std::unique_ptr<websocket::stream<beast::tcp_stream>> socket_;
  std::uint64_t next_ack_id_{0U};
  bool authenticated_{false};
};

SocketIoClient::SocketIoClient() : impl_(std::make_unique<Impl>()) {}

SocketIoClient::~SocketIoClient() = default;

void SocketIoClient::connect(const Config& config) { impl_->connect(config); }

FrameAck SocketIoClient::send_frame(const Frame& frame) {
  return impl_->send_frame(frame);
}

void SocketIoClient::disconnect() noexcept { impl_->disconnect(); }

bool SocketIoClient::connected() const noexcept { return impl_->connected(); }

}  // namespace camera_client
