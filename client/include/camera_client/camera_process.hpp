// 선언 중복을 방지하는 컴파일러 지시문이다.
#pragma once

// CameraProcess의 함수 인자와 콜백 값에 필요한 프로젝트 자료형이다.
#include "camera_client/config.hpp"
#include "camera_client/frame.hpp"

// std::function은 람다, 함수 포인터 등 호출 가능한 대상을 같은 형식으로 보관한다.
#include <functional>
// std::mutex는 여러 스레드가 child_pid_를 동시에 바꾸지 못하게 보호한다.
#include <mutex>
// std::string은 오류 메시지의 메모리를 소유한다.
#include <string>
// std::jthread와 std::stop_token을 제공한다.
#include <thread>

// pid_t는 POSIX 프로세스 ID를 표현하는 정수형이다.
#include <sys/types.h>

namespace camera_client {

/** rpicam-vid 자식 프로세스를 소유하고 MJPEG 표준 출력을 Frame으로 분리한다. */
class CameraProcess {
 public:
  // Frame을 값으로 받으므로 호출된 쪽으로 JPEG 메모리 소유권을 이동할 수 있다.
  using FrameHandler = std::function<void(Frame)>;
  // 오류 문자열은 const 참조로 전달하여 불필요한 복사를 피한다.
  using ErrorHandler = std::function<void(const std::string&)>;

  // = default는 컴파일러가 기본 생성자를 만들어도 된다는 뜻이다.
  CameraProcess() = default;
  // 프로세스와 스레드를 두 객체가 동시에 소유하지 않도록 복사를 금지한다.
  CameraProcess(const CameraProcess&) = delete;
  CameraProcess& operator=(const CameraProcess&) = delete;
  // 소멸자는 stop()을 호출하여 자식 프로세스와 스레드를 RAII 방식으로 정리한다.
  ~CameraProcess();

  /**
   * join 가능한 백그라운드 스레드에서 카메라 캡처를 시작한다.
   *
   * @param config 검증된 너비, 높이, FPS와 JPEG 품질이며 읽기만 한다.
   * @param on_frame JPEG 한 장이 완성될 때 호출하며 Frame 소유권을 받는다.
   * @param on_error 시작/read 실패 시 호출하며 비밀값 없는 오류 문자열을 받는다.
   * @return void 함수이므로 반환값이 없다.
   * @throws std::logic_error 이미 캡처 스레드가 실행 중일 때 발생한다.
   */
  void start(const Config& config, FrameHandler on_frame, ErrorHandler on_error);

  /** 자식 종료를 요청하고 캡처 스레드를 join한다. 반환값이 없고 반복 호출해도 안전하다. */
  void stop();

 private:
  // Config와 콜백을 값으로 받아 백그라운드 스레드가 독립적으로 소유한다.
  void capture_loop(Config config, FrameHandler on_frame, ErrorHandler on_error,
                    std::stop_token stop_token);

  // child_mutex_는 child_pid_를 읽고 쓰는 임계 구역을 보호한다.
  std::mutex child_mutex_;
  // -1은 현재 관리 중인 자식 프로세스가 없다는 sentinel 값이다.
  pid_t child_pid_{-1};
  // jthread는 join 가능한 스레드와 협력적 중단 토큰을 함께 관리한다.
  std::jthread thread_;
};

}  // namespace camera_client 종료
