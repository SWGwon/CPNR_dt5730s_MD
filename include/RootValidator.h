#ifndef CPNR_ROOT_VALIDATOR_H
#define CPNR_ROOT_VALIDATOR_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace cpnr {

using RootValidationProgress =
    std::function<void(double percent, std::string_view stage)>;

struct RootValidationOptions {
  // Zero means scan every event.  A non-zero value scans the leading prefix
  // while still reporting the full TTree entry count.
  std::uint64_t max_events = 0U;
  const std::atomic_bool* cancelled = nullptr;
  RootValidationProgress progress;
};

// Returns a self-contained JSON report.  Validation failures are represented
// in report["checks"] and do not throw.  Exceptions are reserved for failures
// that prevent the validator itself from producing a meaningful report.
nlohmann::json ValidateRootFile(const std::string& input_path,
                                const RootValidationOptions& options = {});

}  // namespace cpnr

#endif  // CPNR_ROOT_VALIDATOR_H
