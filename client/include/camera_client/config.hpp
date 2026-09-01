#pragma once

#include <string>

namespace camera_client {

/** Runtime configuration after environment variables and CLI options are merged. */
struct Config {
  std::string server_url;
  std::string token;
  std::string camera_id{"raspberry-pi-1"};
  int width{1280};
  int height{720};
  int fps{10};
  int jpeg_quality{80};
};

/**
 * Parse and validate camera client configuration.
 *
 * @param argc Number of entries in argv, including the executable name.
 * @param argv Array of NUL-terminated CLI argument strings.
 * @return A validated Config. CLI values override environment variables, which
 *         override defaults.
 * @throws std::invalid_argument When an option is unknown, missing, or outside
 *         its supported range. Secret values are never included in errors.
 */
Config parse_config(int argc, char* argv[]);

/**
 * Build the command-line help text.
 *
 * @param executable_name Name printed in the usage line; normally argv[0].
 * @return Human-readable help text. No configuration secrets are included.
 */
std::string usage(const std::string& executable_name);

}  // namespace camera_client
