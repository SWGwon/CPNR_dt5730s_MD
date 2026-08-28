#include "RootValidator.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <getopt.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

std::atomic_bool g_cancelled{false};

extern "C" void HandleSignal(int) { g_cancelled.store(true); }

void PrintUsage(const char* program) {
  std::cerr << "Usage: " << program
            << " -i FILE [--max-events N]\n";
}

std::uint64_t ParseMaxEvents(const char* text) {
  if (text == nullptr || *text == '\0' || *text == '-') {
    throw std::invalid_argument("--max-events must be a positive integer");
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(text, &end, 10);
  if (errno == ERANGE || end == text || end == nullptr || *end != '\0' ||
      parsed == 0ULL ||
      parsed > static_cast<unsigned long long>(
                   std::numeric_limits<std::uint64_t>::max())) {
    throw std::invalid_argument("--max-events must be a positive integer");
  }
  return static_cast<std::uint64_t>(parsed);
}

}  // namespace

int main(int argc, char** argv) {
  std::string input_path;
  std::uint64_t max_events = 0U;
  static const option long_options[] = {
      {"input", required_argument, nullptr, 'i'},
      {"max-events", required_argument, nullptr, 'n'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0}};

  int option_index = 0;
  int selected = 0;
  while ((selected =
              ::getopt_long(argc, argv, "i:n:h", long_options,
                            &option_index)) != -1) {
    try {
      switch (selected) {
        case 'i':
          input_path = optarg == nullptr ? "" : optarg;
          break;
        case 'n':
          max_events = ParseMaxEvents(optarg);
          break;
        case 'h':
          PrintUsage(argv[0]);
          return 0;
        default:
          PrintUsage(argv[0]);
          return 2;
      }
    } catch (const std::exception& error) {
      std::cerr << "[ValidationFatal] " << error.what() << '\n';
      PrintUsage(argv[0]);
      return 2;
    }
  }

  if (input_path.empty() || optind != argc) {
    PrintUsage(argv[0]);
    return 2;
  }

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  cpnr::RootValidationOptions options;
  options.max_events = max_events;
  options.cancelled = &g_cancelled;
  options.progress = [](double percent, std::string_view stage) {
    std::cerr << "[ValidationProgress] " << std::fixed
              << std::setprecision(1) << percent << "% | " << stage << '\n';
  };

  try {
    const nlohmann::json report =
        cpnr::ValidateRootFile(input_path, options);
    std::cout << report.dump(2) << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "[ValidationFatal] " << error.what() << '\n';
    return 3;
  }
}
