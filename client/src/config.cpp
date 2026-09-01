#include "camera_client/config.hpp"

#include <charconv>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace camera_client {
namespace {

/**
 * Read a non-empty environment variable.
 *
 * @param name NUL-terminated environment variable name.
 * @return Variable contents, or std::nullopt when absent/empty. getenv() returns
 *         a process-owned pointer that this function immediately copies.
 */
std::optional<std::string> environment(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::string(value);
}

/**
 * Convert a decimal string to int without accepting trailing characters.
 *
 * @param text User-provided numeric text.
 * @param option Public option name used in errors; never contains a secret.
 * @return Parsed integer.
 * @throws std::invalid_argument If from_chars() reports an error or partial parse.
 */
int parse_integer(std::string_view text, std::string_view option) {
  int result = 0;
  const char* first = text.data();
  const char* last = first + text.size();
  const auto [parsed_until, error] = std::from_chars(first, last, result);
  if (error != std::errc{} || parsed_until != last) {
    throw std::invalid_argument("invalid integer for " + std::string(option));
  }
  return result;
}

/** Assign an integer environment value only when it is present. */
void apply_environment_integer(int& destination, const char* name) {
  if (const auto value = environment(name)) {
    destination = parse_integer(*value, name);
  }
}

/**
 * Validate a camera ID accepted by the NestJS gateway.
 *
 * @param value Candidate camera ID.
 * @return true for 1..64 ASCII letters, digits, underscore, or hyphen.
 */
bool valid_camera_id(std::string_view value) {
  if (value.empty() || value.size() > 64U) {
    return false;
  }
  for (const char character : value) {
    const bool alpha = (character >= 'a' && character <= 'z') ||
                       (character >= 'A' && character <= 'Z');
    const bool digit = character >= '0' && character <= '9';
    if (!alpha && !digit && character != '_' && character != '-') {
      return false;
    }
  }
  return true;
}

/** Validate all cross-field and range constraints after merging configuration. */
void validate(const Config& config) {
  if (!config.server_url.starts_with("http://")) {
    throw std::invalid_argument(
        "--server must use http:// (TLS support is not enabled in this MVP)");
  }
  if (config.token.empty()) {
    throw std::invalid_argument("camera token is required");
  }
  if (!valid_camera_id(config.camera_id)) {
    throw std::invalid_argument(
        "--camera-id must contain 1..64 letters, digits, '_' or '-'");
  }
  if (config.width <= 0 || config.height <= 0) {
    throw std::invalid_argument("--width and --height must be positive");
  }
  if (config.fps < 1 || config.fps > 30) {
    throw std::invalid_argument("--fps must be between 1 and 30");
  }
  if (config.jpeg_quality < 30 || config.jpeg_quality > 95) {
    throw std::invalid_argument("--quality must be between 30 and 95");
  }
}

}  // namespace

Config parse_config(int argc, char* argv[]) {
  Config config;

  if (const auto value = environment("CAMERA_SERVER_URL")) {
    config.server_url = *value;
  }
  if (const auto value = environment("CAMERA_TOKEN")) {
    config.token = *value;
  }
  if (const auto value = environment("CAMERA_ID")) {
    config.camera_id = *value;
  }
  apply_environment_integer(config.width, "CAMERA_WIDTH");
  apply_environment_integer(config.height, "CAMERA_HEIGHT");
  apply_environment_integer(config.fps, "CAMERA_FPS");
  apply_environment_integer(config.jpeg_quality, "JPEG_QUALITY");

  for (int index = 1; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if (option == "--help" || option == "-h") {
      throw std::invalid_argument("help requested");
    }
    if (index + 1 >= argc) {
      throw std::invalid_argument("missing value for " + std::string(option));
    }

    // Incrementing index consumes the value paired with the current option.
    const std::string value(argv[++index]);
    if (option == "--server") {
      config.server_url = value;
    } else if (option == "--token") {
      config.token = value;
    } else if (option == "--camera-id") {
      config.camera_id = value;
    } else if (option == "--width") {
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

  validate(config);
  return config;
}

std::string usage(const std::string& executable_name) {
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
