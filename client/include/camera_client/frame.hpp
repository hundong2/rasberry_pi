// 헤더 중복 포함으로 생기는 재정의 오류를 막는다.
#pragma once

// std::int64_t와 std::uint8_t처럼 비트 수가 고정된 정수형을 제공한다.
#include <cstdint>
// std::vector는 연속된 동적 배열이며 메모리 해제를 자동으로 처리한다.
#include <vector>

namespace camera_client {

/** JPEG 한 장과 캡처 완료 시각을 함께 전달하는 값 객체다. */
struct Frame {
  // Unix epoch millisecond는 1970-01-01 UTC 이후 흐른 밀리초다.
  // {}는 0으로 값 초기화한다. int64_t는 플랫폼과 무관하게 정확히 64비트다.
  std::int64_t timestamp_ms{};
  // JPEG 파일의 원시 바이트를 0~255 범위의 부호 없는 정수로 저장한다.
  std::vector<std::uint8_t> jpeg;
};

}  // namespace camera_client 종료
