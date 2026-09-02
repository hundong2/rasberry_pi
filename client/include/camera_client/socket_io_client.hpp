// 이 헤더의 class/struct 선언이 중복 정의되지 않게 한다.
#pragma once

#include "camera_client/config.hpp"
#include "camera_client/frame.hpp"

// 고정 폭 정수, 스마트 포인터, 문자열 자료형을 사용한다.
#include <cstdint>
#include <memory>
#include <string>

namespace camera_client {

/** 서버가 camera:frame 처리 후 보낸 ACK(수신 확인)를 해석한 결과다. */
struct FrameAck {
  // false 기본값은 서버 ACK에 accepted가 없을 때도 안전하게 거절로 처리한다.
  bool accepted{false};
  // 거절 이유가 없으면 빈 문자열이다.
  std::string reason;
};

/** WebSocket 위에서 Engine.IO v4와 Socket.IO를 동기 방식으로 처리하는 클라이언트다. */
class SocketIoClient {
 public:
  // 생성자는 숨겨진 Impl과 네트워크 실행 컨텍스트를 준비하지만 아직 연결하지 않는다.
  SocketIoClient();
  // 네트워크 소켓을 단 하나의 객체만 소유하도록 복사 생성과 대입을 금지한다.
  SocketIoClient(const SocketIoClient&) = delete;
  SocketIoClient& operator=(const SocketIoClient&) = delete;
  ~SocketIoClient();

  /**
   * WebSocket을 열고 `/stream` namespace에 카메라 역할로 인증한다.
   *
   * @param config 서버 URL, 카메라 ID와 비밀 토큰이며 함수가 복사/변경하지 않는다.
   * @return 성공하면 반환값 없이 연결 상태만 변경한다.
   * @throws std::runtime_error DNS, TCP, WebSocket, protocol 또는 인증 실패 시 발생한다.
   */
  void connect(const Config& config);

  /**
   * JPEG binary event 한 개를 보내고 같은 ID의 Socket.IO ACK를 기다린다.
   *
   * @param frame JPEG 바이트와 Unix epoch millisecond 시각이며 읽기만 한다.
   * @return 서버 수락 여부와 선택적인 거절 이유를 담은 FrameAck 값이다.
   * @throws std::runtime_error 미연결 상태이거나 프로토콜/전송이 실패할 때 발생한다.
   */
  FrameAck send_frame(const Frame& frame);

  /** 예외를 밖으로 던지지 않고 WebSocket을 닫는다. 반환값이 없고 반복 호출해도 안전하다. */
  // noexcept는 종료 정리 함수가 호출자에게 예외를 전달하지 않는다는 계약이다.
  void disconnect() noexcept;

  /** @return 인증된 namespace와 실제 socket이 모두 사용 가능할 때만 true다. */
  // [[nodiscard]]는 반환값을 무시하면 컴파일러가 경고할 수 있게 한다.
  [[nodiscard]] bool connected() const noexcept;

 private:
  // Impl의 세부 Boost 형식을 헤더에서 감추는 PImpl 패턴이다.
  class Impl;
  // unique_ptr는 Impl을 단독 소유하며 SocketIoClient 소멸 때 자동 delete한다.
  std::unique_ptr<Impl> impl_;
};

}  // namespace camera_client 종료
