#include "camera_client/config.hpp"

// from_chars, getenv, optional, 예외와 문자열 view에 필요한 표준 헤더다.
#include <charconv>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace camera_client {
// 이름 없는 namespace의 함수는 이 번역 단위(config.cpp) 안에서만 보인다.
namespace {

/**
 * 비어 있지 않은 환경 변수 하나를 읽는다.
 *
 * @param name 마지막이 '\0'인 환경 변수 이름 C 문자열이다.
 * @return 값이 있으면 소유하는 std::string, 없거나 비었으면 std::nullopt다.
 *         getenv의 프로세스 소유 포인터는 함수 안에서 즉시 복사한다.
 */
std::optional<std::string> environment(const char* name) {
  // getenv(name)은 값의 C 문자열 주소를 반환하고, 변수가 없으면 nullptr를 반환한다.
  const char* value = std::getenv(name);
  // *value == '\0'은 첫 문자가 문자열 종료 문자, 즉 빈 값이라는 뜻이다.
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  // getenv가 가리키는 프로세스 메모리를 std::string으로 즉시 복사해 안전하게 소유한다.
  return std::string(value);
}

/**
 * 뒤쪽의 불필요한 문자를 허용하지 않고 10진 문자열 전체를 int로 변환한다.
 *
 * @param text 사용자가 입력한 숫자 문자열 보기다.
 * @param option 오류 메시지에 쓸 공개 옵션 이름이며 비밀값을 포함하지 않는다.
 * @return 변환한 int 값이다.
 * @throws std::invalid_argument from_chars가 실패하거나 일부만 읽었을 때 발생한다.
 */
int parse_integer(std::string_view text, std::string_view option) {
  // from_chars가 성공하면 이 변수에 정수 결과를 기록한다.
  int result = 0;
  // string_view::data()는 첫 문자 포인터를, size()는 문자 개수를 반환한다.
  const char* first = text.data();
  const char* last = first + text.size();
  // 구조적 바인딩으로 반환 구조체의 ptr과 ec를 각각 이름 붙여 받는다.
  // from_chars는 예외 없이 [first,last) 범위를 읽고 멈춘 위치와 오류 코드를 반환한다.
  const auto [parsed_until, error] = std::from_chars(first, last, result);
  // errc{}는 오류 없음, parsed_until == last는 모든 문자를 소비했다는 뜻이다.
  if (error != std::errc{} || parsed_until != last) {
    throw std::invalid_argument("invalid integer for " + std::string(option));
  }
  return result;
}

/** 정수 환경 변수가 존재할 때만 destination을 변경한다. 반환값은 없다. */
void apply_environment_integer(int& destination, const char* name) {
  // if 초기화문에서 optional을 만들고, 값이 있을 때만 본문을 실행한다.
  if (const auto value = environment(name)) {
    // *value는 optional 내부 문자열을 참조한다. destination은 참조이므로 원본이 변경된다.
    destination = parse_integer(*value, name);
  }
}

/**
 * NestJS gateway가 허용하는 카메라 ID인지 검사한다.
 *
 * @param value 검사할 카메라 ID 문자열 보기다.
 * @return 길이 1~64이고 ASCII 영문자, 숫자, 밑줄, 하이픈만 있으면 true다.
 */
bool valid_camera_id(std::string_view value) {
  // empty()는 길이가 0이면 true이고 size()는 문자 수를 반환한다.
  if (value.empty() || value.size() > 64U) {
    return false;
  }
  // 범위 기반 for는 value의 문자를 처음부터 끝까지 한 개씩 복사해 읽는다.
  for (const char character : value) {
    const bool alpha = (character >= 'a' && character <= 'z') ||
                       (character >= 'A' && character <= 'Z');
    // char 범위를 직접 비교해 locale에 따른 isalpha 동작 차이를 피한다.
    const bool digit = character >= '0' && character <= '9';
    if (!alpha && !digit && character != '_' && character != '-') {
      return false;
    }
  }
  return true;
}

/** 설정 병합 후 필수값과 각 범위를 검사한다. 성공 시 반환값 없이 끝나고 실패 시 예외다. */
void validate(const Config& config) {
  // C++20 starts_with는 문자열이 지정 접두사로 시작하면 true를 반환한다.
  if (!config.server_url.starts_with("http://")) {
    throw std::invalid_argument(
        "--server must use http:// (TLS support is not enabled in this MVP)");
  }
  if (config.token.empty()) {
    // throw 이후 현재 함수는 즉시 끝나며 parse_config의 호출자 catch로 이동한다.
    throw std::invalid_argument("camera token is required");
  }
  if (!valid_camera_id(config.camera_id)) {
    throw std::invalid_argument(
        "--camera-id must contain 1..64 letters, digits, '_' or '-'");
  }
  // ||는 둘 중 하나라도 양수가 아니면 전체 조건을 true로 만든다.
  if (config.width <= 0 || config.height <= 0) {
    throw std::invalid_argument("--width and --height must be positive");
  }
  // 하한과 상한을 모두 검사해 rpicam-vid와 서버가 감당할 범위로 제한한다.
  if (config.fps < 1 || config.fps > 30) {
    throw std::invalid_argument("--fps must be between 1 and 30");
  }
  if (config.jpeg_quality < 30 || config.jpeg_quality > 95) {
    throw std::invalid_argument("--quality must be between 30 and 95");
  }
}

}  // namespace

Config parse_config(int argc, char* argv[]) {
  // Config{} 생성 시 구조체에 선언된 camera_id, 해상도, FPS 기본값이 적용된다.
  Config config;

  // 환경 변수가 있으면 optional이 true로 평가되어 기본값을 덮어쓴다.
  if (const auto value = environment("CAMERA_SERVER_URL")) {
    config.server_url = *value;
  }
  if (const auto value = environment("CAMERA_TOKEN")) {
    config.token = *value;
  }
  if (const auto value = environment("CAMERA_ID")) {
    config.camera_id = *value;
  }
  // 숫자 환경 변수도 존재할 때만 변환하며 잘못된 값은 예외로 보고한다.
  apply_environment_integer(config.width, "CAMERA_WIDTH");
  apply_environment_integer(config.height, "CAMERA_HEIGHT");
  apply_environment_integer(config.fps, "CAMERA_FPS");
  apply_environment_integer(config.jpeg_quality, "JPEG_QUALITY");

  // argv[0]은 실행 파일 이름이므로 실제 옵션이 시작되는 1부터 순회한다.
  for (int index = 1; index < argc; ++index) {
    // string_view는 argv 문자열을 복사하거나 소유하지 않는 읽기 전용 보기다.
    const std::string_view option(argv[index]);
    if (option == "--help" || option == "-h") {
      throw std::invalid_argument("help requested");
    }
    // 현재 옵션 뒤에 값이 없으면 argv 범위를 넘어 읽기 전에 실패시킨다.
    if (index + 1 >= argc) {
      throw std::invalid_argument("missing value for " + std::string(option));
    }

    // 전위 ++로 index를 먼저 증가시켜 현재 옵션에 짝지어진 값을 소비한다.
    const std::string value(argv[++index]);
    if (option == "--server") {
      // 문자열 옵션은 std::string 대입으로 config가 자신의 복사본을 소유한다.
      config.server_url = value;
    } else if (option == "--token") {
      config.token = value;
    } else if (option == "--camera-id") {
      config.camera_id = value;
    } else if (option == "--width") {
      // 숫자 옵션은 공통 parse_integer를 거쳐 완전한 10진 정수인지 확인한다.
      config.width = parse_integer(value, option);
    } else if (option == "--height") {
      config.height = parse_integer(value, option);
    } else if (option == "--fps") {
      config.fps = parse_integer(value, option);
    } else if (option == "--quality") {
      config.jpeg_quality = parse_integer(value, option);
    } else {
      throw std::invalid_argument("unknown option: " + std::string(option));
    }
  }

  // 모든 입력을 합친 뒤 한 번 검증하므로 CLI가 환경 변수보다 우선한다.
  validate(config);
  // 값 반환은 컴파일러의 복사 생략 또는 이동을 사용하므로 효율적이다.
  return config;
}

std::string usage(const std::string& executable_name) {
  // R"(...)"는 줄바꿈과 따옴표를 그대로 담을 수 있는 raw string literal이다.
  // operator+가 실행 파일 이름과 도움말을 합쳐 새 std::string을 반환한다.
  return "Usage: " + executable_name + R"( [options]

Options (CLI overrides environment variables):
  --server URL       CAMERA_SERVER_URL, required, http:// only
  --token TOKEN      CAMERA_TOKEN, required and never logged
  --camera-id ID     CAMERA_ID, default raspberry-pi-1
  --width PIXELS     CAMERA_WIDTH, default 1280
  --height PIXELS    CAMERA_HEIGHT, default 720
  --fps RATE         CAMERA_FPS, default 10, range 1..30
  --quality VALUE    JPEG_QUALITY, default 80, range 30..95
  -h, --help         Show this help
)";
}

}  // namespace camera_client
