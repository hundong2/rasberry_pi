#include "camera_client/socket_io_client.hpp"

// Boost.Asio는 비동기 I/O 기반 요소와 TCP를, Beast는 WebSocket 프로토콜을 제공한다.
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
// nlohmann::json은 C++ 값과 JSON 문자열 사이의 변환을 담당한다.
#include <nlohmann/json.hpp>

// 시간, 크기형, 예외, 문자열, 이동 연산에 필요한 표준 헤더다.
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace camera_client {
// 긴 namespace 이름에 짧은 별칭을 붙인다. 새 namespace를 만드는 것은 아니다.
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
// using은 자주 쓰는 타입에 현재 범위의 별칭을 만든다.
using tcp = asio::ip::tcp;
using Json = nlohmann::json;

namespace {

// 이 파일에서 사용하는 네트워크 계층은 다음 순서로 감싸진다.
// TCP: IP 주소와 port 사이에 신뢰성 있는 byte stream을 만든다.
// WebSocket: TCP 위에서 text/binary 메시지 경계를 제공한다.
// Engine.IO: 연결 open, ping/pong과 transport를 관리한다.
// Socket.IO: /stream namespace, camera:frame event와 ACK ID를 관리한다.
// 따라서 WebSocket 연결만 성공했다고 camera 인증까지 성공한 것은 아니다.

/** 설정된 HTTP URL에서 분리한 host와 port 값 객체다. */
struct ServerEndpoint {
  // host와 port는 resolver가 각각 이름/서비스 문자열로 사용한다.
  std::string host;
  std::string port;
};

/**
 * 현재 MVP가 지원하는 http://host[:port] 형태를 분석한다.
 *
 * @param url 분석할 서버 URL 문자열 보기다.
 * @return TCP resolve에 사용할 host와 명시/기본 port를 ServerEndpoint로 반환한다.
 * @throws std::runtime_error host가 비었거나 지원하지 않는 path가 있을 때 발생한다.
 */
ServerEndpoint parse_server_url(std::string_view url) {
  // constexpr string_view는 별도 동적 할당 없이 읽기 전용 문자열을 가리킨다.
  constexpr std::string_view prefix = "http://";
  // starts_with는 C++20 함수이며 접두사가 일치할 때 true다.
  if (!url.starts_with(prefix)) {
    throw std::runtime_error("only http:// server URLs are supported");
  }

  // substr(pos)는 pos부터 끝까지 보는 string_view를 반환하며 문자를 복사하지 않는다.
  std::string_view authority = url.substr(prefix.size());
  // find('/')는 첫 위치를 반환하고 없으면 npos를 반환한다.
  const std::size_t slash = authority.find('/');
  if (slash != std::string_view::npos) {
    // substr(slash)는 slash부터 끝까지의 경로 부분이다.
    const std::string_view path = authority.substr(slash);
    if (path != "/") {
      throw std::runtime_error("server URL must not contain a path");
    }
    // [0, slash)만 남겨 host:port authority로 만든다.
    authority = authority.substr(0U, slash);
  }
  if (authority.empty()) {
    throw std::runtime_error("server URL host is empty");
  }

  // rfind(':')는 마지막 콜론 위치를 찾아 host와 port를 나눈다.
  const std::size_t colon = authority.rfind(':');
  if (colon == std::string_view::npos) {
    // braced return은 ServerEndpoint{host, port}를 만들어 값으로 반환한다.
    return {std::string(authority), "80"};
  }
  if (colon == 0U || colon + 1U >= authority.size()) {
    throw std::runtime_error("server URL has an invalid host or port");
  }
  // 두 substr 결과를 소유하는 std::string으로 복사하여 반환 객체 수명과 분리한다.
  return {std::string(authority.substr(0U, colon)),
          std::string(authority.substr(colon + 1U))};
}

/** @return Socket.IO auth JSON. nlohmann::json escapes all string values. */
std::string auth_json(const Config& config) {
  // initializer_list의 key/value 쌍으로 JSON object를 만든다.
  const Json auth = {{"role", "camera"},
                     {"cameraId", config.camera_id},
                     {"token", config.token}};
  // dump()는 특수 문자를 escaping한 compact UTF-8 JSON std::string을 반환한다.
  return auth.dump();
}

/** @return Socket.IO binary-event JSON with one attachment placeholder. */
std::string frame_event_json(const Frame& frame) {
  // Socket.IO binary attachment가 뒤따름을 나타내는 placeholder object다.
  Json placeholder = {{"_placeholder", true}, {"num", 0}};
  // std::move로 임시 placeholder의 내부 메모리를 payload에 넘긴다.
  Json payload = {{"timestamp", frame.timestamp_ms},
                  {"frame", std::move(placeholder)}};
  // Json::array는 첫 원소가 이벤트명, 둘째가 payload인 JSON 배열을 만든다.
  Json event = Json::array({"camera:frame", std::move(payload)});
  // 직렬화한 descriptor JSON을 호출자에게 반환한다.
  return event.dump();
}

}  // namespace

// 헤더에는 전방 선언만 있던 중첩 Impl의 실제 정의다. 이 패턴 덕분에 공개 헤더를
// 포함하는 파일은 무거운 Boost 타입과 헤더를 알 필요가 없다.
class SocketIoClient::Impl {
 public:
  /** TCP/WebSocket을 열고 Engine.IO 및 namespace handshake를 완료한다. */
  void connect(const Config& config) {
    // 재연결 전에 기존 소켓이 있다면 예외 없이 정리한다.
    disconnect();
    const ServerEndpoint endpoint = parse_server_url(config.server_url);

    // resolver는 io_context를 사용해 host/port를 TCP endpoint 목록으로 변환한다.
    tcp::resolver resolver(io_context_);
    // resolve(host,port)는 endpoint range를 반환하고 DNS 실패 시 system_error를 던진다.
    const auto endpoints = resolver.resolve(endpoint.host, endpoint.port);

    // make_unique는 heap에 WebSocket stream을 만들고 unique_ptr을 반환한다.
    socket_ = std::make_unique<websocket::stream<beast::tcp_stream>>(io_context_);
    // get_lowest_layer는 WebSocket 아래 TCP stream 참조를 반환한다.
    // expires_after는 다음 하위 계층 작업 제한 시간을 8초로 설정한다.
    beast::get_lowest_layer(*socket_).expires_after(std::chrono::seconds(8));
    // connect(endpoints)는 성공한 endpoint를 반환하고 모두 실패하면 예외를 던진다.
    beast::get_lowest_layer(*socket_).connect(endpoints);

    // suggested(client)는 클라이언트 역할에 권장되는 WebSocket timeout 옵션을 만든다.
    socket_->set_option(websocket::stream_base::timeout::suggested(
        beast::role_type::client));
    // 수신 메시지 상한을 4MiB로 설정해 과도한 메모리 사용을 막는다.
    socket_->read_message_max(4U * 1024U * 1024U);
    // decorator는 HTTP Upgrade 요청을 보내기 직전에 request를 수정하는 콜백이다.
    socket_->set_option(websocket::stream_base::decorator(
        [](websocket::request_type& request) {
          // set(field,value)은 User-Agent 헤더를 추가/교체하며 반환값은 사용하지 않는다.
          request.set(boost::beast::http::field::user_agent,
                      "rasberry-pi-camera-client/1.0");
        }));

    // Host 헤더는 host:port 형식이어야 한다.
    const std::string host_header = endpoint.host + ":" + endpoint.port;
    // handshake(host,target)는 HTTP Upgrade가 성공하면 void로 끝나고 실패하면 예외다.
    socket_->handshake(host_header, "/socket.io/?EIO=4&transport=websocket");

    // 서버가 첫 번째로 보내는 Engine.IO open packet 한 개를 읽는다.
    const Message open_packet = read_message();
    // text가 아니거나 첫 문자가 Engine.IO open 타입 '0'이 아니면 프로토콜 오류다.
    if (!open_packet.text || !open_packet.data.starts_with('0')) {
      throw std::runtime_error("invalid Engine.IO open packet");
    }

    // '40'은 Engine.IO message + Socket.IO connect이며 /stream 뒤에 인증 JSON을 붙인다.
    write_text("40/stream," + auth_json(config));
    // for (;;)는 조건이 없는 무한 반복이다. 성공 return 또는 실패 예외로만 끝난다.
    for (;;) {
      const Message packet = read_message();
      // namespace 연결 확인 과정에서는 예상하지 않은 binary packet을 건너뛴다.
      if (!packet.text) {
        continue;
      }
      if (packet.data == "2") {
        // 서버 ping '2'에 client pong '3'을 보내 연결 생존을 알린다.
        write_text("3");  // Engine.IO ping type 2 is answered by pong type 3.
        continue;
      }
      if (packet.data.starts_with("40/stream,")) {
        // 서버가 /stream 연결 승인을 보냈으므로 이후 프레임 전송이 가능하다.
        authenticated_ = true;
        next_ack_id_ = 0U;
        return;
      }
      if (packet.data.starts_with("44/stream,")) {
        // Socket.IO packet type 4는 namespace connect error다.
        throw std::runtime_error("Socket.IO namespace authentication failed");
      }
    }
  }

  /** binary event 설명자와 attachment를 전송한 뒤 서버 ACK를 해석해 반환한다. */
  FrameAck send_frame(const Frame& frame) {
    // nullptr 비교는 동적 소켓 객체가 존재하는지 확인한다.
    if (!authenticated_ || socket_ == nullptr) {
      throw std::runtime_error("Socket.IO client is not connected");
    }
    // JPEG는 최소 길이와 SOI(FF D8), EOI(FF D9) 경계를 모두 만족해야 한다.
    // back()은 vector 마지막 원소 참조를 반환한다.
    if (frame.jpeg.size() < 4U || frame.jpeg[0] != 0xFFU ||
        frame.jpeg[1] != 0xD8U || frame.jpeg[frame.jpeg.size() - 2U] != 0xFFU ||
        frame.jpeg.back() != 0xD9U) {
      throw std::runtime_error("capture produced an invalid JPEG boundary");
    }

    // 후위 ++는 현재 ID를 ack_id에 대입한 뒤 다음 ID를 1 증가시킨다.
    const std::uint64_t ack_id = next_ack_id_++;
    // Packet: Engine.IO message(4), Socket.IO binary event(5), one attachment(1-),
    // namespace, ACK id, then the JSON event array.
    // 즉 "451-"는 문자 4/5/1/구분자이며 정수 451 하나가 아니다.
    const std::string descriptor = "451-/stream," + std::to_string(ack_id) +
                                   frame_event_json(frame);
    // descriptor는 텍스트로 먼저 보내고 JPEG attachment는 바로 이어 binary로 보낸다.
    write_text(descriptor);
    write_binary(frame.jpeg);

    // 서버 ACK는 같은 ack_id를 포함해야 이 프레임의 결과로 인정한다.
    const std::string expected_prefix =
        "43/stream," + std::to_string(ack_id);
    // 다른 ping이나 다른 ID의 메시지가 중간에 올 수 있어 목표 ACK까지 반복한다.
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
        // 현재 프레임 ACK가 아니므로 버리고 다음 WebSocket 메시지를 기다린다.
        continue;
      }

      // find('[',offset)는 ACK 인자 JSON 배열이 시작되는 위치를 반환한다.
      const std::size_t json_offset = packet.data.find('[', expected_prefix.size());
      if (json_offset == std::string::npos) {
        throw std::runtime_error("Socket.IO ACK has no JSON argument array");
      }
      // substr는 JSON 부분을 새 string으로 반환하고 parse는 Json 값 또는 예외를 만든다.
      const Json arguments = Json::parse(packet.data.substr(json_offset));
      if (!arguments.is_array() || arguments.empty() ||
          !arguments.front().is_object()) {
        throw std::runtime_error("Socket.IO ACK has an unexpected shape");
      }

      // front()는 배열 첫 원소 참조다. 서버 계약상 이 object가 ACK 결과다.
      const Json& result = arguments.front();
      // FrameAck{}의 accepted=false와 빈 reason 기본값으로 먼저 생성한다.
      FrameAck ack;
      // value(key,default)는 key가 없거나 형식에 맞지 않을 때 default를 반환한다.
      ack.accepted = result.value("accepted", false);
      ack.reason = result.value("reason", std::string{});
      return ack;
    }
  }

  /** Close protocol and transport layers while swallowing shutdown errors. */
  void disconnect() noexcept {
    // 먼저 false로 바꿔 이후 connected()와 send_frame()이 사용하지 못하게 한다.
    authenticated_ = false;
    if (socket_ == nullptr) {
      return;
    }
    try {
      if (socket_->is_open()) {
        // Socket.IO namespace disconnect packet을 먼저 전송한다.
        write_text("41/stream,");
        // close(normal)는 정상 close frame을 보내고 상대 응답을 기다리며 실패 시 예외다.
        socket_->close(websocket::close_code::normal);
      }
    // catch (...)는 타입과 관계없이 모든 C++ 예외를 잡는다. noexcept 함수 밖으로
    // 예외가 나가면 std::terminate가 호출되므로 종료 정리에서는 의도적으로 삼킨다.
    } catch (...) {
      // 소멸/재시도 정리 중 오류가 원래 네트워크 오류를 가리지 않게 무시한다.
    }
    // unique_ptr::reset은 소켓 객체를 delete하고 포인터를 nullptr로 만든다.
    socket_.reset();
  }

  /** @return Whether the namespace handshake completed and socket remains open. */
  [[nodiscard]] bool connected() const noexcept {
    // &&는 왼쪽부터 평가하므로 nullptr일 때 socket_->is_open()을 호출하지 않는다.
    return authenticated_ && socket_ != nullptr && socket_->is_open();
  }

 private:
  struct Message {
    // text는 WebSocket opcode 종류, data는 한 메시지의 전체 payload다.
    bool text{false};
    std::string data;
  };

  /** @return One complete WebSocket message and whether its opcode was text. */
  Message read_message() {
    if (socket_ == nullptr) {
      throw std::runtime_error("cannot read from a closed WebSocket");
    }
    // flat_buffer는 Beast가 수신 크기에 맞춰 연속/분할 메모리를 자동 관리한다.
    beast::flat_buffer buffer;
    // read(buffer)는 한 WebSocket 메시지를 끝까지 읽고 바이트 수를 반환한다.
    // read의 반환값은 수신 바이트 수지만 현재는 완성된 buffer만 필요해 사용하지 않는다.
    socket_->read(buffer);
    // got_text()는 마지막 read opcode가 text면 true다.
    // buffers_to_string은 buffer.data() 범위를 std::string으로 복사한다.
    return {socket_->got_text(), beast::buffers_to_string(buffer.data())};
  }

  /** @param message Engine.IO/Socket.IO text packet to send atomically. */
  void write_text(const std::string& message) {
    if (socket_ == nullptr) {
      throw std::runtime_error("cannot write to a closed WebSocket");
    }
    // text(true)는 이후 write의 WebSocket opcode를 UTF-8 text로 선택한다.
    socket_->text(true);
    // asio::buffer는 문자열 메모리를 복사하지 않는 buffer view를 반환한다.
    // write는 전체 메시지를 전송하고 바이트 수를 반환하지만 여기서는 사용하지 않는다.
    // 동기 write는 성공 시 전체 buffer를 보내고 전송 바이트 수를 반환한다.
    socket_->write(asio::buffer(message));
  }

  /**
   * Engine.IO binary message payload를 한 개 전송한다.
   *
   * @param bytes 원시 Socket.IO attachment다. WebSocket binary opcode 자체가
   *              Engine.IO message임을 나타내므로 ASCII '4' prefix는 붙이지 않는다.
   */
  void write_binary(const std::vector<std::uint8_t>& bytes) {
    if (socket_ == nullptr) {
      throw std::runtime_error("cannot write to a closed WebSocket");
    }
    // binary(true)는 이후 write의 opcode를 binary로 선택한다.
    socket_->binary(true);
    // vector는 연속 메모리이므로 asio::buffer가 별도 복사 없이 view로 감싼다.
    // 반환값은 전송 바이트 수지만 동기 write가 예외 없이 끝났는지만 확인한다.
    socket_->write(asio::buffer(bytes));
  }

  // io_context는 Boost.Asio I/O 객체가 사용하는 실행 컨텍스트다.
  asio::io_context io_context_;
  // socket_은 TCP stream을 감싼 WebSocket stream을 단독 소유한다.
  std::unique_ptr<websocket::stream<beast::tcp_stream>> socket_;
  // ACK ID는 요청과 응답을 연결하는 증가 번호이며 uint64_t라 매우 오래 사용 가능하다.
  std::uint64_t next_ack_id_{0U};
  // namespace 인증 성공 여부를 socket open 상태와 별도로 저장한다.
  bool authenticated_{false};
};

// 생성할 때 구현 객체를 heap에 만들고 unique_ptr이 단독 소유한다.
SocketIoClient::SocketIoClient() : impl_(std::make_unique<Impl>()) {}

// Impl이 이 .cpp에서 완전한 타입으로 보이므로 default 소멸자를 여기서 정의한다.
SocketIoClient::~SocketIoClient() = default;

// 아래 함수들은 공개 인터페이스 호출을 숨겨진 Impl 객체에 위임한다.
void SocketIoClient::connect(const Config& config) { impl_->connect(config); }

FrameAck SocketIoClient::send_frame(const Frame& frame) {
  // Impl이 반환한 FrameAck 값은 복사 생략 또는 이동으로 호출자에게 전달된다.
  return impl_->send_frame(frame);
}

void SocketIoClient::disconnect() noexcept { impl_->disconnect(); }

bool SocketIoClient::connected() const noexcept { return impl_->connected(); }

}  // namespace camera_client
