// 같은 헤더가 여러 경로로 포함되어도 선언이 한 번만 보이게 한다.
#pragma once

// std::string은 길이가 가변적인 문자열의 메모리를 자동 관리한다.
#include <string>

namespace camera_client {

/**
 * 기본값, 환경 변수, 명령행 인자를 합친 최종 실행 설정이다.
 * struct는 class와 달리 멤버가 기본 public이어서 config.width처럼 직접 읽을 수 있다.
 */
struct Config {
  // 기본 초기값이 없는 두 값은 validate()에서 필수 입력 여부를 검사한다.
  std::string server_url;
  std::string token;
  // 중괄호 멤버 초기화는 Config 객체가 생성될 때 적용되는 기본값이다.
  std::string camera_id{"raspberry-pi-1"};
  int width{1280};
  int height{720};
  int fps{10};
  int jpeg_quality{80};
};

/**
 * 카메라 클라이언트 설정을 읽고 유효성을 검사한다.
 *
 * @param argc 실행 파일 이름을 포함한 argv 원소 개수다.
 * @param argv 마지막 문자가 '\0'인 C 문자열 포인터 배열이다.
 * @return 검증된 Config 값이다. 우선순위는 CLI > 환경 변수 > 기본값이다.
 * @throws std::invalid_argument 옵션이 없거나 잘못됐거나 범위를 벗어날 때 발생한다.
 *         오류 문자열에는 비밀 토큰 값을 포함하지 않는다.
 */
// 성공하면 Config 값을 반환하고, 실패하면 std::invalid_argument 예외를 던진다.
Config parse_config(int argc, char* argv[]);

/**
 * 명령행 도움말 문자열을 만든다.
 *
 * @param executable_name 사용법 첫 줄에 출력할 이름이며 일반적으로 argv[0]이다.
 * @return 사람이 읽을 수 있는 도움말 문자열이며 비밀 설정은 포함하지 않는다.
 */
// 반환된 std::string이 도움말 텍스트의 메모리를 소유한다.
std::string usage(const std::string& executable_name);

}  // namespace camera_client
