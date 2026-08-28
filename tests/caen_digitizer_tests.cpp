#include "CaenDigitizer.h"

#include <iostream>
#include <stdexcept>

int main() {
  caen_mock::ResetLifecycleInstrumentation();
  caen_mock::SetResetFailure(true);

  bool reset_failure_propagated = false;
  {
    CaenDigitizer digitizer(CAEN_DGTZ_USB, 0, 0, 0);
    if (caen_mock::reset_calls != 0U) {
      std::cerr << "Opening a digitizer reset it before identity validation\n";
      return 1;
    }
    try {
      digitizer.Reset();
    } catch (const std::runtime_error&) {
      reset_failure_propagated = true;
    }
  }

  if (!reset_failure_propagated) {
    std::cerr << "Explicit reset failure was not propagated\n";
    return 1;
  }
  if (caen_mock::open_calls != 1U || caen_mock::close_calls != 1U) {
    std::cerr << "Opened CAEN handle was not closed after reset failure\n";
    return 1;
  }

  caen_mock::SetResetFailure(false);
  {
    CaenDigitizer digitizer(CAEN_DGTZ_USB, 7, 0, 0);
    if (digitizer.GetHandle() < 0) {
      std::cerr << "Successful construction returned an invalid handle\n";
      return 1;
    }
    digitizer.Reset();
    if (caen_mock::last_open_link != 7U || caen_mock::reset_calls != 2U) {
      std::cerr << "Configured USB link or explicit reset was not preserved\n";
      return 1;
    }
  }

  if (caen_mock::open_calls != 2U || caen_mock::close_calls != 2U) {
    std::cerr << "Normally constructed CAEN handle was not closed exactly once\n";
    return 1;
  }

  std::cout << "All CaenDigitizer lifecycle tests passed.\n";
  return 0;
}
