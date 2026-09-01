#include "camera_client/application.hpp"
#include "camera_client/config.hpp"

#include <atomic>
#include <csignal>
#include <exception>
#include <iostream>
#include <string_view>

namespace {

std::atomic_bool stop_requested{false};

/**
 * Record a process stop request without performing unsafe cleanup in signal context.
 *
 * @param signal_number POSIX signal number; intentionally unused.
 */
extern "C" void handle_signal(int signal_number) {
  static_cast<void>(signal_number);
  stop_requested.store(true, std::memory_order_relaxed);
}

/** @return true when argv contains -h or --help. */
bool wants_help(int argc, char* argv[]) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "-h" || argument == "--help") {
      return true;
    }
  }
  return false;
}

}  // namespace

/**
 * Process entry point.
 *
 * @param argc Number of command-line entries.
 * @param argv Command-line strings owned by the process runtime.
 * @return 0 for help/orderly stop, 1 for configuration errors, or the
 *         application's nonzero runtime failure code.
 */
int main(int argc, char* argv[]) {
  if (wants_help(argc, argv)) {
    std::cout << camera_client::usage(argv[0]);
    return 0;
  }

  // signal() returns the previous handler or SIG_ERR; startup fails if registration fails.
  if (std::signal(SIGINT, handle_signal) == SIG_ERR ||
      std::signal(SIGTERM, handle_signal) == SIG_ERR) {
    std::cerr << "failed to install SIGINT/SIGTERM handlers\n";
    return 1;
  }

  try {
    camera_client::Config config = camera_client::parse_config(argc, argv);
    camera_client::Application application(std::move(config));
    return application.run(stop_requested);
  } catch (const std::exception& error) {
    std::cerr << "configuration error: " << error.what() << "\n\n"
              << camera_client::usage(argv[0]);
    return 1;
  }
}
