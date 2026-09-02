// #pragma once는 이 헤더가 한 번의 컴파일에서 중복 포함되는 것을 막는다.
#pragma once

// Config 선언을 사용하기 위해 프로젝트 내부 헤더를 포함한다.
#include "camera_client/config.hpp"

// std::atomic_bool: 여러 스레드가 데이터 경쟁 없이 종료 상태를 공유한다.
#include <atomic>

// namespace는 같은 이름의 충돌을 막는다. 사용할 때 camera_client::Application처럼 쓴다.
namespace camera_client {

/**
 * 카메라 캡처, 최신 프레임 보관, 서버 전송과 종료 순서를 조정하는 최상위 객체다.
 * class의 멤버는 별도 표시가 없으면 private이며 여기서는 public/private를 명시한다.
 */
class Application {
  // public 아래 선언은 객체 밖의 main.cpp 같은 호출자가 접근할 수 있다.
 public:
  /**
   * 검증이 끝난 Config를 받아 Application 내부에 보관한다.
   *
   * @param config 값으로 받으므로 호출자의 값과 분리되며 내부 멤버로 이동된다.
   */
  // explicit는 Config가 Application으로 암시적으로 변환되는 실수를 막는다.
  explicit Application(Config config);

  /**
   * 외부 signal 플래그가 true가 되거나 복구 불가능한 시작 오류가 날 때까지 실행한다.
   *
   * @param stop_requested main의 signal handler가 변경하는 원자 종료 플래그다.
   * @return 정상 종료면 0, 복구 불가능한 카메라 실패면 2다.
   */
  // const 참조(&)이므로 플래그를 복사하지 않고 읽기만 한다.
  int run(const std::atomic_bool& stop_requested);

  // private 아래 선언은 Application 자신의 멤버 함수만 직접 접근할 수 있다.
 private:
  // 멤버 이름의 뒤쪽 _는 객체가 소유하는 내부 상태임을 나타내는 규칙이다.
  Config config_;
};

}  // namespace camera_client 종료
