#pragma once

#include "camera_client/config.hpp"

#include <atomic>

namespace camera_client {

/** Coordinates capture, latest-frame buffering, transmission, and shutdown. */
class Application {
 public:
  /**
   * Construct the application with an immutable validated configuration.
   *
   * @param config Configuration copied into application-owned storage.
   */
  explicit Application(Config config);

  /**
   * Run until the external signal flag is set or an unrecoverable startup fails.
   *
   * @param stop_requested Atomic flag changed by the process signal handler.
   * @return 0 after an orderly stop; nonzero after an unrecoverable failure.
   */
  int run(const std::atomic_bool& stop_requested);

 private:
  Config config_;
};

}  // namespace camera_client
