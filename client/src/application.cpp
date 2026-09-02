#include "camera_client/application.hpp"

#include "camera_client/camera_process.hpp"
#include "camera_client/latest_frame_queue.hpp"
#include "camera_client/socket_io_client.hpp"

// 각 표준 라이브러리 기능은 사용하는 선언을 직접 include하는 것이 원칙이다.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>

namespace camera_client {
// 이름 없는 namespace의 log 함수/변수는 다른 .cpp에서 링크하거나 호출할 수 없다.
namespace {

// 캡처 스레드와 전송 스레드의 로그 한 줄이 서로 섞이지 않게 보호한다.
std::mutex log_mutex;

/** 캡처/메인 스레드가 출력하는 로그 한 줄을 서로 섞이지 않게 기록한다. */
void log_line(const char* level, const std::string& message) {
  // CTAD(class template argument deduction)가 mutex 형식에서 lock_guard 타입을 추론한다.
  // 함수가 끝나면 소멸자가 자동으로 unlock하므로 예외가 발생해도 잠금이 풀린다.
  std::lock_guard lock(log_mutex);
  // std::cerr는 버퍼링이 적은 표준 오류 스트림이고 '\n'은 줄바꿈 문자다.
  std::cerr << '[' << level << "] " << message << '\n';
}

/**
 * 짧은 steady-clock 간격으로 나눠 종료 요청을 확인하며 대기한다.
 *
 * @param duration 전체 대기 목표 시간이다.
 * @param stop_requested signal handler가 변경하는 종료 플래그다.
 * @return void이므로 반환값이 없으며, 시간이 끝나거나 stop이 true면 종료한다.
 */
void interruptible_sleep(std::chrono::milliseconds duration,
                         const std::atomic_bool& stop_requested) {
  // steady_clock은 시스템 시간이 바뀌어도 뒤로 가지 않아 대기 시간 측정에 적합하다.
  const auto deadline = std::chrono::steady_clock::now() + duration;
  // load는 atomic 값을 원자적으로 읽고, deadline 전까지만 짧게 나눠 대기한다.
  while (!stop_requested.load(std::memory_order_relaxed) &&
         std::chrono::steady_clock::now() < deadline) {
    // sleep_for는 현재 스레드를 최소 지정 시간 동안 실행 대기 상태로 둔다.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

}  // namespace

// 멤버 초기화 목록은 생성자 본문 전에 config_를 직접 생성한다.
// std::move 덕분에 Config 내부 문자열 버퍼를 보통 복사하지 않고 넘겨받는다.
Application::Application(Config config) : config_(std::move(config)) {}

int Application::run(const std::atomic_bool& stop_requested) {
  // 세 객체는 스택에 생성되며 함수가 끝날 때 역순으로 자동 소멸한다(RAII).
  LatestFrameQueue frames;
  CameraProcess camera;
  SocketIoClient socket;
  // 캡처 콜백은 별도 스레드에서 실행되므로 공유 카운터에는 atomic을 사용한다.
  std::atomic_bool camera_failed{false};
  std::atomic_uint64_t captured{0U};
  std::atomic_uint64_t replaced{0U};
  // accepted/rejected는 이 run 스레드에서만 접근하므로 일반 정수면 충분하다.
  std::uint64_t accepted = 0U;
  std::uint64_t rejected = 0U;

  // try 안의 camera.start가 던진 예외는 아래 catch에서 시작 실패로 변환한다.
  try {
    camera.start(
        config_,
        // [&]는 주변 지역 변수를 참조로 캡처하고 Frame은 값으로 전달받는다.
        [&](Frame frame) {
          // fetch_add는 이전 값을 반환하며 여기서는 반환값이 필요 없어 무시한다.
          captured.fetch_add(1U, std::memory_order_relaxed);
          // push가 true면 소비되지 않은 이전 프레임을 새 프레임이 교체했다는 뜻이다.
          if (frames.push(std::move(frame))) {
            replaced.fetch_add(1U, std::memory_order_relaxed);
          }
        },
        // 오류 콜백은 CameraProcess의 백그라운드 스레드에서 호출된다.
        [&](const std::string& error) {
          if (stop_requested.load(std::memory_order_relaxed)) {
            // 사용자가 정상 종료를 요청한 뒤 rpicam-vid가 닫힌 것은 장애가 아니다.
            return;  // systemd/timeout may terminate the child with the main process.
          }
          log_line("ERROR", error);
          // store는 다른 스레드가 읽는 실패 상태를 데이터 경쟁 없이 갱신한다.
          camera_failed.store(true, std::memory_order_relaxed);
          // close는 pop_for에서 기다리는 전송 스레드를 즉시 깨운다.
          frames.close();
        });
  } catch (const std::exception& error) {
    // error.what()은 카메라 시작 실패 이유를 const char*로 반환한다.
    log_line("ERROR", std::string("camera startup failed: ") + error.what());
    return 2;
  }

  log_line("INFO", "camera capture started for " + config_.camera_id);
  // seconds{1}은 단위가 명확한 1초 duration 객체를 만든다.
  std::chrono::seconds backoff{1};
  // random_device로 초기 seed를 만들고 mt19937 의사 난수 엔진을 초기화한다.
  std::mt19937 random_engine(std::random_device{}());
  // 호출할 때마다 0~250 범위의 정수를 균등 분포로 반환한다.
  std::uniform_int_distribution<int> jitter_ms(0, 250);

  // &&는 두 조건이 모두 true일 때만 반복한다. Signal 요청이나 카메라 실패 중
  // 어느 하나가 발생하면 반복을 끝내고 아래 공통 정리 단계로 간다.
  while (!stop_requested.load(std::memory_order_relaxed) &&
         !camera_failed.load(std::memory_order_relaxed)) {
    try {
      if (!socket.connected()) {
        log_line("INFO", "connecting to " + config_.server_url);
        // connect는 성공하면 반환값 없이 연결 상태를 바꾸고 실패하면 예외를 던진다.
        socket.connect(config_);
        log_line("INFO", "Socket.IO /stream authentication succeeded");
        backoff = std::chrono::seconds(1);
      }

      // pop_for는 500ms 안에 Frame이 있으면 optional<Frame>, 아니면 nullopt를 반환한다.
      auto frame = frames.pop_for(std::chrono::milliseconds(500));
      // optional은 값이 있으면 true로 평가된다.
      if (!frame) {
        continue;
      }

      // *frame은 optional 내부 Frame의 참조이며 send_frame은 서버 ACK를 반환한다.
      const FrameAck ack = socket.send_frame(*frame);
      if (ack.accepted) {
        // 전위 증가는 값을 1 올린다. 반환되는 새 값은 여기서 사용하지 않는다.
        ++accepted;
      } else {
        ++rejected;
        // backpressure는 서버가 바빠 의도적으로 최신성 우선 드롭을 한 정상 제어다.
        // 그 외 거절만 운영자가 확인할 경고로 출력한다.
        if (ack.reason != "backpressure") {
          log_line("WARN", "server rejected frame: " + ack.reason);
        }
      }

      const std::uint64_t completed = accepted + rejected;
      // 나머지 연산 %가 0이면 정확히 100장 단위다.
      if (completed > 0U && completed % 100U == 0U) {
        // to_string은 정수를 사람이 읽을 수 있는 std::string으로 변환한다.
        log_line("INFO", "stats captured=" +
                             std::to_string(captured.load(std::memory_order_relaxed)) +
                             " replaced=" +
                             std::to_string(replaced.load(std::memory_order_relaxed)) +
                             " accepted=" + std::to_string(accepted) +
                             " rejected=" + std::to_string(rejected));
      }
    // connect/send/read 과정의 표준 예외는 프로세스를 끝내지 않고 재접속으로 복구한다.
    } catch (const std::exception& error) {
      // 네트워크/프로토콜 예외가 발생하면 현재 소켓을 닫고 다음 반복에서 재연결한다.
      socket.disconnect();
      log_line("WARN", std::string("connection lost: ") + error.what());
      // jitter_ms(engine)는 동시 재접속 집중을 줄이는 임의 지연값을 반환한다.
      const auto delay = backoff + std::chrono::milliseconds(jitter_ms(random_engine));
      interruptible_sleep(delay, stop_requested);
      // std::min은 두 duration 중 작은 값을 반환하여 최대 backoff를 30초로 제한한다.
      backoff = std::min(backoff * 2, std::chrono::seconds(30));
    }
  }

  // 생성의 반대 순서로 입력 큐, 네트워크, 카메라를 명시적으로 종료한다.
  frames.close();
  socket.disconnect();
  camera.stop();
  log_line("INFO", "stopped: captured=" +
                       std::to_string(captured.load(std::memory_order_relaxed)) +
                       " replaced=" +
                       std::to_string(replaced.load(std::memory_order_relaxed)) +
                       " accepted=" + std::to_string(accepted) +
                       " rejected=" + std::to_string(rejected));
  // 삼항 연산자는 카메라 실패면 2, 정상 신호 종료면 0을 반환한다.
  return camera_failed.load(std::memory_order_relaxed) ? 2 : 0;
}

}  // namespace camera_client
