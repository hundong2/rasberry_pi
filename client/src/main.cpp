#include "camera_client/application.hpp"
#include "camera_client/config.hpp"

// <atomic>: signal handler와 일반 실행 흐름이 공유할 원자 변수를 제공한다.
#include <atomic>
// <csignal>: std::signal, SIGINT, SIGTERM, SIG_ERR 같은 signal API를 제공한다.
#include <csignal>
// <exception>: 모든 표준 예외의 공통 기반인 std::exception을 제공한다.
#include <exception>
// <iostream>: 터미널 표준 출력 cout과 표준 오류 cerr를 제공한다.
#include <iostream>
// <string_view>: 문자열을 복사하지 않고 읽기만 하는 std::string_view를 제공한다.
#include <string_view>
// std::move로 Config의 문자열 소유권을 Application에 전달한다.
#include <utility>

// 이름 없는 namespace는 내부 이름을 이 main.cpp 파일에서만 보이게 한다.
namespace {

// Signal은 무엇인가?
// - 운영체제가 프로세스에 "특정 사건이 발생했다"고 비동기적으로 알리는 작은 정수다.
// - 일반 함수 호출처럼 코드가 직접 호출 시점을 정하지 않는다. 프로그램 어느 줄을
//   실행하던 중에도 운영체제가 흐름을 잠시 중단하고 handler를 실행할 수 있다.
// - SIGINT는 보통 터미널 Ctrl+C가 보내는 "중단 요청"이다.
// - SIGTERM은 kill PID, systemctl stop 등이 보내는 "정상 종료 요청"이다.
// - 숫자 자체는 운영체제마다 다를 수 있으므로 2, 15 대신 상수 이름을 사용한다.
//
// signal handler와 Application::run이 함께 읽고 쓰므로 일반 bool은 data race를
// 만들 수 있다. atomic_bool의 load/store로 하나의 값을 쪼개지지 않게 접근한다.
// 이 구현은 Raspberry Pi/Linux에서 atomic_bool이 lock-free라는 전제다. 가장 엄격한
// 이식성이 필요하면 std::sig_atomic_t 기반 설계를 별도로 검토해야 한다.
std::atomic_bool stop_requested{false};

/**
 * Signal 문맥에서는 실제 종료 작업을 하지 않고 종료 요청만 기록한다.
 *
 * Signal handler 안에서는 iostream 출력, mutex 잠금, 동적 메모리 할당, 일반 객체
 * 파괴처럼 async-signal-safe가 보장되지 않는 작업을 피해야 한다. handler가 평소
 * 실행 중인 코드가 잡은 mutex를 다시 기다리면 영원히 멈출 수도 있기 때문이다.
 * 따라서 여기서는 플래그만 바꾸고 실제 socket/camera 정리는 run()이 수행한다.
 *
 * @param signal_number 전달된 POSIX signal 번호다. SIGINT와 SIGTERM을 같은 종료
 *                      흐름으로 처리하므로 현재 값은 구분하지 않고 사용하지 않는다.
 * @return signal handler의 형식은 void이므로 반환값이 없다.
 */
// extern "C"는 함수 이름에 C linkage를 적용해 C 계열 signal API가 호출하기 쉬운
// 단순한 함수 형태로 노출한다. 예외를 handler 밖으로 던져서는 안 된다.
extern "C" void handle_signal(int signal_number) {
  // 반환값이 없는 캐스팅으로 의도적으로 사용하지 않는 매개변수 경고를 없앤다.
  static_cast<void>(signal_number);
  // store는 원자적으로 true를 기록한다. relaxed는 다른 메모리와 순서 동기화가
  // 필요 없고 이 플래그 자체의 데이터 경쟁만 막으면 충분하다는 뜻이다.
  stop_requested.store(true, std::memory_order_relaxed);
}

/**
 * 명령행에 도움말 옵션이 있는지 검사한다.
 *
 * @param argc argv 배열에 들어 있는 문자열 포인터 개수다.
 * @param argv 운영체제가 전달한 NUL 종료 문자열 포인터 배열이다.
 * @return -h 또는 --help가 하나라도 있으면 true, 없으면 false다.
 */
bool wants_help(int argc, char* argv[]) {
  // argc는 argv 원소 수이며 argv[0]을 제외한 사용자의 인자만 검사한다.
  for (int index = 1; index < argc; ++index) {
    // string_view는 메모리 할당 없이 argv[index]를 비교한다.
    const std::string_view argument(argv[index]);
    // ==는 문자열 내용을 비교한다. ||는 둘 중 하나가 true이면 true다.
    if (argument == "-h" || argument == "--help") {
      return true;
    }
  }
  return false;
}

}  // namespace: 이 파일 내부 보조 이름 영역 종료

/**
 * 운영체제가 프로그램 시작 시 호출하는 진입점이다.
 *
 * @param argc 명령행 문자열 개수다. 실행 파일 이름도 포함하므로 최소 1이다.
 * @param argv 명령행 문자열 배열이다. argv[0]은 보통 실행 파일 이름이며 각 문자열은
 *             프로그램 실행 환경이 소유하므로 이 함수에서는 읽기만 한다.
 * @return 0은 도움말 또는 정상 종료, 1은 설정/signal 등록 실패, 2는 카메라 실행
 *         실패다. shell에서는 `echo $?`로 마지막 종료 코드를 확인할 수 있다.
 */
int main(int argc, char* argv[]) {
  // 도움말 요청은 필수 설정을 검사하기 전에 정상 종료 코드 0으로 처리한다.
  if (wants_help(argc, argv)) {
    // std::cout은 표준 출력이며 operator<<가 문자열을 스트림에 기록한다.
    std::cout << camera_client::usage(argv[0]);
    return 0;
  }

  // std::signal(signal_number, handler)은 앞으로 해당 signal을 받을 때 실행할 함수를
  // 등록한다. 성공하면 이전 handler 주소를, 실패하면 특수 값 SIG_ERR를 반환한다.
  // SIG_ERR는 실제 signal 종류가 아니라 "등록 함수가 실패했다"는 반환 표식이다.
  //
  // SIGINT: 사용자가 Ctrl+C로 요청하는 대화형 종료.
  // SIGTERM: systemd/kill이 서비스에 정리할 기회를 주며 요청하는 일반적인 종료.
  // SIGKILL은 handler를 등록하거나 무시할 수 없으므로 여기서 처리하지 못한다.
  // ||는 앞 조건이 true면 뒤 조건을 평가하지 않는 short-circuit 연산자다.
  if (std::signal(SIGINT, handle_signal) == SIG_ERR ||
      std::signal(SIGTERM, handle_signal) == SIG_ERR) {
    // handler를 설치하지 못한 채 실행하면 정상 정리가 보장되지 않으므로 시작을 막는다.
    std::cerr << "failed to install SIGINT/SIGTERM handlers\n";
    return 1;
  }

  // try 블록에서 던져진 std::exception 계열 예외는 아래 catch가 처리한다.
  try {
    // parse_config는 성공 시 검증된 Config를 반환하고 실패 시 예외를 던진다.
    camera_client::Config config = camera_client::parse_config(argc, argv);
    // std::move로 문자열을 복사하지 않고 config의 소유 자원을 Application에 넘긴다.
    camera_client::Application application(std::move(config));
    // run의 반환값을 그대로 프로세스 종료 코드로 운영체제에 전달한다.
    return application.run(stop_requested);
  // const 참조로 받으면 예외 객체를 복사하거나 파생 타입 정보를 잘라내지 않는다.
  } catch (const std::exception& error) {
    // what()은 예외 객체가 가진 NUL 종료 오류 메시지 포인터를 반환한다.
    std::cerr << "configuration error: " << error.what() << "\n\n"
              << camera_client::usage(argv[0]);
    return 1;
  }
}
