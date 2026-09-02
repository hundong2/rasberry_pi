#include "camera_client/config.hpp"
#include "camera_client/latest_frame_queue.hpp"

// 테스트에서 시간 단위, 환경 변수, 예외, 입출력, 문자열과 이동 기능을 사용한다.
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

// 이 테스트는 외부 테스트 프레임워크 없이 예외를 실패 신호로 사용하는 작은
// test runner다. 카메라 하드웨어와 서버 없이 순수 설정/큐 규칙만 확인한다.

/** 조건이 false이면 테스트 실패를 나타내는 std::runtime_error를 던진다. */
void require(bool condition, const std::string& message) {
  // 별도 테스트 프레임워크 없이 assertion과 같은 역할을 한다.
  if (!condition) {
    // throw는 현재 흐름을 중단하고 가장 가까운 호환 catch로 제어를 이동한다.
    throw std::runtime_error(message);
  }
}

/** @return 구분 가능한 payload byte를 포함한 작은 테스트 Frame을 반환한다. */
camera_client::Frame frame_with_value(std::uint8_t value) {
  // 지정 초기화자는 C++20 문법으로 멤버 이름을 명시해 Frame 값을 만든다.
  // FF D8과 FF D9는 각각 JPEG 시작과 끝 marker다.
  return {.timestamp_ms = value, .jpeg = {0xFFU, 0xD8U, value, 0xFFU, 0xD9U}};
}

/** 읽지 않은 이전 Frame이 최신 Frame으로 교체되는지 검사한다. */
void test_latest_frame_replacement() {
  // 지역 queue는 함수 종료 시 소멸하며 mutex 등 멤버도 자동 정리된다.
  camera_client::LatestFrameQueue queue;
  // 논리 부정 !는 첫 push의 false 반환을 true 조건으로 바꿔 검사한다.
  require(!queue.push(frame_with_value(1U)), "first push must not replace");
  require(queue.push(frame_with_value(2U)), "second push must replace first");
  // auto는 pop_for 반환형 std::optional<Frame>을 컴파일러가 추론하게 한다.
  const auto frame = queue.pop_for(std::chrono::milliseconds(1));
  // has_value는 optional이 Frame을 포함하면 true를 반환한다.
  require(frame.has_value(), "latest frame must be available");
  // optional의 operator->는 내부 Frame 멤버에 접근할 포인터처럼 동작한다.
  require(frame->timestamp_ms == 2, "queue must preserve only the latest frame");
  // 위 세 검사는 큐 크기가 1이고 지연된 1번 대신 최신 2번이 전달됨을 함께 증명한다.
}

/** 토큰을 출력하지 않고 환경 변수와 CLI 우선순위를 검사한다. */
void test_config_precedence() {
  // setenv(name,value,overwrite)는 성공 시 0, 실패 시 -1을 반환한다.
  // 세 번째 인자 1은 이미 같은 변수가 있어도 새 값으로 덮어쓰라는 뜻이다.
  require(::setenv("CAMERA_SERVER_URL", "http://environment:3000", 1) == 0,
          "setenv server failed");
  require(::setenv("CAMERA_TOKEN", "unit-test-token", 1) == 0,
          "setenv token failed");
  require(::setenv("CAMERA_FPS", "7", 1) == 0, "setenv fps failed");

  // 수정 가능한 char 배열은 실제 main의 char* argv[] 형식을 그대로 흉내 낸다.
  char executable[] = "camera-client-tests";
  char server_option[] = "--server";
  char server_value[] = "http://cli:3000";
  char fps_option[] = "--fps";
  char fps_value[] = "9";
  // argv 배열의 각 원소는 NUL로 끝나는 명령행 인자 첫 문자를 가리킨다.
  char* argv[] = {executable, server_option, server_value, fps_option, fps_value};
  // argc=5는 위 배열의 원소 수이며 반환 Config를 const로 두어 테스트 중 변경을 막는다.
  const camera_client::Config config = camera_client::parse_config(5, argv);
  require(config.server_url == "http://cli:3000", "CLI server must win");
  require(config.fps == 9, "CLI FPS must win");
  require(config.token == "unit-test-token", "environment token must be read");
  // server와 FPS는 CLI가 환경 변수를 이기고, CLI에 없는 token은 환경에서 유지된다.
}

/** 서버 규칙과 같은 camera ID 유효성 검사가 적용되는지 확인한다. */
void test_invalid_camera_id() {
  char executable[] = "camera-client-tests";
  char id_option[] = "--camera-id";
  char id_value[] = "invalid id";
  char* argv[] = {executable, id_option, id_value};
  // false로 시작해 기대한 invalid_argument를 잡았을 때만 true로 변경한다.
  bool rejected = false;
  try {
    // 반환값 검사가 목적이 아니므로 static_cast<void>로 명시적으로 버린다.
    static_cast<void>(camera_client::parse_config(3, argv));
  // const 참조 catch는 예외 객체를 복사하지 않으며 여기서는 메시지가 필요 없다.
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "camera ID with a space must be rejected");
  // 예외가 없었다면 rejected가 false여서 require가 이 테스트를 실패시킨다.
}

}  // namespace

/**
 * 모든 단위 테스트를 정해진 순서로 실행한다.
 *
 * @return 모두 통과하면 shell 성공 코드 0, 하나라도 예외면 실패 코드 1이다.
 */
int main() {
  try {
    // 앞 테스트가 예외를 던지면 이후 호출은 실행되지 않고 catch로 이동한다.
    test_latest_frame_replacement();
    test_config_precedence();
    test_invalid_camera_id();
    // 모든 테스트가 반환됐으므로 표준 출력에 성공 메시지를 기록한다.
    std::cout << "All camera client tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    // runtime_error와 invalid_argument는 std::exception에서 파생되어 여기서 잡힌다.
    std::cerr << "Test failure: " << error.what() << '\n';
    return 1;
  }
}
