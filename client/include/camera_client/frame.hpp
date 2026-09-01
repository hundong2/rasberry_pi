#pragma once

#include <cstdint>
#include <vector>

namespace camera_client {

/** One encoded camera frame with its capture-completion wall-clock timestamp. */
struct Frame {
  std::int64_t timestamp_ms{};
  std::vector<std::uint8_t> jpeg;
};

}  // namespace camera_client
