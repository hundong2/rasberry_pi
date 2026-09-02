#include "camera_client/camera_process.hpp"

// 버퍼 검색, 고정 배열, errno, 시간, signal, 정수와 문자열 처리용 표준 헤더다.
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// waitpid와 pipe/fork/read/close 같은 POSIX 시스템 호출 선언을 제공한다.
#include <sys/wait.h>
#include <unistd.h>

namespace camera_client {
// JPEG 검색 보조 함수와 상수는 이 파일 바깥에 공개할 필요가 없다.
namespace {

// constexpr 값은 컴파일 시간에 정해지며 실행 중 변경할 수 없다.
constexpr std::size_t kReadBufferBytes = 64U * 1024U;
constexpr std::size_t kMaximumBufferedBytes = 4U * 1024U * 1024U;

/** @return 현재 Unix epoch 시각을 밀리초 단위 int64_t로 반환한다. */
std::int64_t epoch_milliseconds() {
  // system_clock::now()는 현재 wall-clock 시각을 time_point로 반환한다.
  const auto now = std::chrono::system_clock::now();
  // time_since_epoch()는 epoch 이후 duration, duration_cast는 이를 밀리초로 바꾼다.
  // count()는 duration 안의 정수 tick 개수를 반환한다.
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             now.time_since_epoch())
      .count();
}

/**
 * 바이트 vector 안에서 연속된 두 marker 바이트를 찾는다.
 *
 * @param bytes 읽기만 할 검색 대상 buffer다.
 * @param first marker의 첫 바이트다.
 * @param second marker의 둘째 바이트다.
 * @param offset 검색을 시작할 index이며 생략하면 0이다.
 * @return 찾으면 첫 marker index, 없으면 bytes.size() sentinel을 반환한다.
 */
std::size_t find_marker(const std::vector<std::uint8_t>& bytes,
                        std::uint8_t first, std::uint8_t second,
                        std::size_t offset = 0U) {
  // size()는 저장된 바이트 수다. 두 바이트를 비교할 공간이 없으면 sentinel을 반환한다.
  if (bytes.size() < 2U || offset >= bytes.size() - 1U) {
    return bytes.size();
  }
  // index + 1도 범위 안에 있을 때만 두 바이트를 비교한다.
  for (std::size_t index = offset; index + 1U < bytes.size(); ++index) {
    // vector의 operator[]는 빠르지만 범위 검사를 하지 않아 위 조건이 중요하다.
    if (bytes[index] == first && bytes[index + 1U] == second) {
      return index;
    }
  }
  // 별도 npos가 없으므로 bytes.size()를 '찾지 못함' sentinel로 사용한다.
  return bytes.size();
}

}  // namespace

// 소멸 시 stop을 호출하는 RAII로 조기 return이나 예외에서도 프로세스가 남지 않게 한다.
CameraProcess::~CameraProcess() { stop(); }

void CameraProcess::start(const Config& config, FrameHandler on_frame,
                          ErrorHandler on_error) {
  // joinable()은 jthread가 현재 실행 스레드를 소유하면 true를 반환한다.
  if (thread_.joinable()) {
    throw std::logic_error("camera capture is already running");
  }
  // jthread는 callable의 첫 인자로 stop_token을 전달하고 소멸 시 자동 join한다.
  // 캡처 목록의 this/config는 복사하고 콜백은 std::move로 스레드에 소유권을 넘긴다.
  thread_ = std::jthread(
      [this, config, on_frame = std::move(on_frame),
       on_error = std::move(on_error)](std::stop_token stop_token) mutable {
        // mutable은 값으로 캡처한 콜백을 move할 수 있도록 lambda의 operator()를
        // 기본 const가 아닌 상태로 만든다.
        capture_loop(config, std::move(on_frame), std::move(on_error), stop_token);
      });
}

void CameraProcess::stop() {
  if (!thread_.joinable()) {
    // 시작하지 않았거나 이미 join했다면 할 일이 없으므로 반복 호출에도 안전하다.
    return;
  }

  // request_stop()은 이번 호출이 처음 중단 요청이면 true, 이미 요청됐으면 false다.
  thread_.request_stop();
  {
    // lock_guard가 child_pid_를 읽는 동안 캡처 스레드의 쓰기를 막는다.
    std::lock_guard lock(child_mutex_);
    if (child_pid_ > 0) {
      // kill(pid, SIGTERM)은 성공 시 0, 실패 시 -1이고 errno에 이유를 남긴다.
      // ESRCH는 해당 PID가 이미 종료되어 존재하지 않는다는 뜻이다.
      if (::kill(child_pid_, SIGTERM) != 0 && errno != ESRCH) {
        // SIGTERM 전달이 실패해도 아래 join/정리 과정은 계속 진행해야 한다.
      }
    }
  }
  // join()은 capture_loop가 자식 회수까지 마칠 때까지 현재 스레드를 기다리게 한다.
  thread_.join();
}

void CameraProcess::capture_loop(Config config, FrameHandler on_frame,
                                 ErrorHandler on_error,
                                 std::stop_token stop_token) {
  // pipe_fds[0]은 읽기 끝, [1]은 쓰기 끝이며 -1은 아직 열리지 않았다는 값이다.
  int pipe_fds[2] = {-1, -1};
  // pipe는 성공 시 두 파일 디스크립터를 배열에 쓰고 0, 실패 시 -1을 반환한다.
  if (::pipe(pipe_fds) != 0) {
    // strerror(errno)는 현재 POSIX 오류 번호를 설명하는 C 문자열을 반환한다.
    on_error("pipe() failed: " + std::string(std::strerror(errno)));
    return;
  }

  // fork는 부모에게 자식 PID, 자식에게 0, 실패하면 -1을 반환한다.
  const pid_t pid = ::fork();
  if (pid < 0) {
    // close(fd)는 성공 시 0, 실패 시 -1이다. 오류 경로에서는 두 끝을 모두 닫는다.
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    on_error("fork() failed: " + std::string(std::strerror(errno)));
    return;
  }

  // fork 이후 자식 프로세스만 pid == 0 분기로 들어간다.
  if (pid == 0) {
    // 자식은 카메라 출력을 쓰기만 하므로 pipe의 읽기 끝을 닫는다.
    ::close(pipe_fds[0]);
    // dup2(oldfd,newfd)는 oldfd를 newfd 번호로 복제하고 성공 시 newfd, 실패 시 -1이다.
    // 따라서 rpicam-vid의 STDOUT_FILENO(1)가 pipe 쓰기 끝을 가리키게 된다.
    if (::dup2(pipe_fds[1], STDOUT_FILENO) < 0) {
      // _exit는 fork로 복제된 부모의 C/C++ 버퍼를 다시 flush하지 않고 즉시 종료한다.
      _exit(126);
    }
    ::close(pipe_fds[1]);

    // to_string은 정수 설정을 execlp에 전달할 NUL 종료 std::string으로 변환한다.
    const std::string width = std::to_string(config.width);
    const std::string height = std::to_string(config.height);
    const std::string fps = std::to_string(config.fps);
    const std::string quality = std::to_string(config.jpeg_quality);
    // execlp(file, argv0, ..., nullptr)는 PATH에서 실행 파일을 찾아 현재 자식
    // 프로세스 이미지를 교체한다. 성공하면 돌아오지 않고 실패할 때만 -1을 반환한다.
    // c_str()은 각 std::string의 읽기 전용 NUL 종료 포인터를 반환한다.
    ::execlp("rpicam-vid", "rpicam-vid", "--nopreview", "--timeout", "0",
             "--codec", "mjpeg", "--width", width.c_str(), "--height",
             height.c_str(), "--framerate", fps.c_str(), "--quality",
             quality.c_str(), "--output", "-", static_cast<char*>(nullptr));
    // 127은 관례적으로 명령을 찾거나 실행하지 못했음을 나타낸다.
    _exit(127);
  }

  // 부모는 pipe에 쓰지 않으므로 쓰기 끝을 닫아 EOF를 정확히 감지할 수 있게 한다.
  ::close(pipe_fds[1]);
  {
    std::lock_guard lock(child_mutex_);
    // stop()이 SIGTERM을 보낼 수 있도록 자식 PID를 공유 상태에 기록한다.
    child_pid_ = pid;
  }

  // std::array는 크기가 컴파일 시간에 고정된 스택 버퍼이며 {}로 0 초기화한다.
  std::array<std::uint8_t, kReadBufferBytes> read_buffer{};
  // pending은 read 경계를 넘어 나뉜 JPEG 조각을 이어 붙이는 가변 버퍼다.
  std::vector<std::uint8_t> pending;
  // reserve는 원소 수(size)는 바꾸지 않고 재할당을 줄이도록 용량(capacity)만 확보한다.
  pending.reserve(512U * 1024U);

  // stop_requested()는 중단 요청 여부를 bool로 반환한다.
  while (!stop_token.stop_requested()) {
    // data()는 배열 첫 바이트 포인터, size()는 읽을 최대 바이트 수를 반환한다.
    // read는 실제 읽은 바이트 수, EOF면 0, 오류면 -1을 반환한다.
    const ssize_t count = ::read(pipe_fds[0], read_buffer.data(), read_buffer.size());
    if (count == 0) {
      // 정상 종료 요청이 아니라 카메라가 먼저 닫힌 경우만 오류 콜백을 호출한다.
      if (!stop_token.stop_requested()) {
        on_error("rpicam-vid closed its output stream");
      }
      break;
    }
    if (count < 0) {
      if (errno == EINTR) {
        // EINTR은 signal이 read를 중단했다는 뜻이므로 while 조건을 다시 확인한다.
        continue;
      }
      on_error("read() failed: " + std::string(std::strerror(errno)));
      break;
    }

    // count가 양수임을 확인했으므로 부호 없는 size_t로 안전하게 변환한다.
    const auto valid_bytes = static_cast<std::size_t>(count);
    // insert(pos, first, last)는 [first,last) 범위를 pending 끝에 복사한다.
    // ptrdiff_t 변환은 vector iterator에 더하는 표준 signed 차이 형식에 맞춘다.
    pending.insert(pending.end(), read_buffer.begin(),
                   read_buffer.begin() + static_cast<std::ptrdiff_t>(valid_bytes));

    while (true) {
      // JPEG SOI(Start Of Image) 표식 FF D8의 시작 위치를 찾는다.
      const std::size_t start = find_marker(pending, 0xFFU, 0xD8U);
      if (start == pending.size()) {
        if (pending.size() > 1U) {
          // 완전한 SOI가 없으면 마지막 1바이트만 남겨 다음 read의 첫 바이트와 조합한다.
          pending.erase(pending.begin(), pending.end() - 1);
        }
        break;
      }

      // SOI 다음부터 JPEG EOI(End Of Image) 표식 FF D9를 찾는다.
      const std::size_t end = find_marker(pending, 0xFFU, 0xD9U, start + 2U);
      if (end == pending.size()) {
        if (start > 0U) {
          // SOI 앞의 불필요한 데이터만 제거하고 미완성 JPEG는 다음 read까지 보존한다.
          pending.erase(pending.begin(),
                        pending.begin() + static_cast<std::ptrdiff_t>(start));
        }
        break;
      }

      // EOI 두 바이트까지 포함하기 위해 마지막 위치에 2를 더한다.
      const std::size_t frame_end = end + 2U;
      // Frame은 기본 생성되고 timestamp/JPEG를 다음 줄에서 채운다.
      Frame frame;
      frame.timestamp_ms = epoch_milliseconds();
      // assign(first,last)는 해당 반복자 범위의 바이트로 jpeg 내용을 교체한다.
      frame.jpeg.assign(
          pending.begin() + static_cast<std::ptrdiff_t>(start),
          pending.begin() + static_cast<std::ptrdiff_t>(frame_end));
      // 콜백에 Frame을 이동해 JPEG 버퍼 소유권을 넘기고 큰 복사를 피한다.
      on_frame(std::move(frame));
      // 전달을 마친 바이트를 pending에서 지워 다음 JPEG가 앞에 오게 한다.
      pending.erase(pending.begin(),
                    pending.begin() + static_cast<std::ptrdiff_t>(frame_end));
    }

    if (pending.size() > kMaximumBufferedBytes) {
      // clear는 size를 0으로 만들며 비정상 스트림이 메모리를 무한히 늘리지 못하게 한다.
      pending.clear();
      // on_error는 반환값이 없는 std::function이며 Application 오류 콜백을 실행한다.
      on_error("MJPEG parser discarded an oversized incomplete frame");
    }
  }

  // pipe 읽기 끝을 닫는다. close는 성공 시 0이지만 종료 정리에서는 복구할 일이 없다.
  ::close(pipe_fds[0]);
  if (!stop_token.stop_requested()) {
    // 카메라 출력이 먼저 끝난 비정상 경로에서도 자식 종료를 요청한다.
    ::kill(pid, SIGTERM);
  }
  // waitpid가 자식의 종료 상태를 기록할 정수다.
  int status = 0;
  // waitpid(pid,&status,0)는 지정 자식을 기다린 뒤 PID, 실패 시 -1을 반환한다.
  // EINTR일 때만 반복하고, 성공하면 zombie 프로세스를 회수한다.
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  // status에는 종료 코드/signal 정보가 bit field로 들어오지만 현재는 자식 회수만
  // 필요하므로 WIFEXITED/WEXITSTATUS로 해석하지 않는다.
  {
    std::lock_guard lock(child_mutex_);
    // 회수가 끝났으므로 stop()이 더 이상 이전 PID에 signal을 보내지 않게 한다.
    child_pid_ = -1;
  }
}

}  // namespace camera_client
