#include "DT5730Status.h"

#include <CAENDigitizer.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "[FAIL] " << message << '\n';
    ++failures;
  }
}

struct AcquisitionFlagCase {
  const char* name;
  std::uint32_t mask;
  bool dt5730_status::AcquisitionStatus::*field;
};

struct FailureFlagCase {
  const char* name;
  std::uint32_t mask;
  bool dt5730_status::BoardFailureStatus::*field;
};

void CheckRegisterContract() {
  static_assert(dt5730_status::kAcquisitionStatusRegister == 0x8104U);
  static_assert(dt5730_status::kBoardFailureStatusRegister == 0x8178U);
  static_assert(dt5730_status::kRunMask == (1U << 2U));
  static_assert(dt5730_status::kEventReadyMask == (1U << 3U));
  static_assert(dt5730_status::kEventFullMask == (1U << 4U));
  static_assert(dt5730_status::kClockSourceMask == (1U << 5U));
  static_assert(dt5730_status::kPllNoUnlockMask == (1U << 7U));
  static_assert(dt5730_status::kBoardReadyMask == (1U << 8U));
  static_assert(dt5730_status::kChannelShutdownMask == (1U << 19U));
  static_assert(dt5730_status::kDtTemperatureAlarmMask == (1U << 20U));
  static_assert(dt5730_status::kNimTemperatureAlarmMask == (1U << 21U));
  static_assert(dt5730_status::kBoardFailurePllMask == (1U << 4U));
  static_assert(dt5730_status::kBoardFailureTemperatureMask == (1U << 5U));
  static_assert(dt5730_status::kBoardFailureAdcPowerDownMask == (1U << 6U));

  constexpr std::array<AcquisitionFlagCase, 8> cases{{
      {"RUN", dt5730_status::kRunMask,
       &dt5730_status::AcquisitionStatus::run},
      {"event-ready", dt5730_status::kEventReadyMask,
       &dt5730_status::AcquisitionStatus::event_ready},
      {"event-full", dt5730_status::kEventFullMask,
       &dt5730_status::AcquisitionStatus::event_full},
      {"PLL no-unlock", dt5730_status::kPllNoUnlockMask,
       &dt5730_status::AcquisitionStatus::pll_no_unlock},
      {"board-ready", dt5730_status::kBoardReadyMask,
       &dt5730_status::AcquisitionStatus::board_ready},
      {"channel-shutdown", dt5730_status::kChannelShutdownMask,
       &dt5730_status::AcquisitionStatus::channel_shutdown},
      {"DT temperature", dt5730_status::kDtTemperatureAlarmMask,
       &dt5730_status::AcquisitionStatus::dt_temperature_alarm},
      {"NIM temperature", dt5730_status::kNimTemperatureAlarmMask,
       &dt5730_status::AcquisitionStatus::nim_temperature_alarm},
  }};

  for (const auto& test_case : cases) {
    const auto decoded =
        dt5730_status::DecodeAcquisitionStatus(test_case.mask);
    Check(decoded.raw == test_case.mask,
          std::string(test_case.name) + " preserves the raw register");
    Check(decoded.*(test_case.field),
          std::string(test_case.name) + " decodes its documented bit");
  }

  const auto internal = dt5730_status::DecodeAcquisitionStatus(0U);
  const auto external = dt5730_status::DecodeAcquisitionStatus(
      dt5730_status::kClockSourceMask);
  Check(internal.clock_source == dt5730_status::ClockSource::kInternal,
        "cleared clock-source bit means internal clock");
  Check(external.clock_source == dt5730_status::ClockSource::kExternal,
        "set clock-source bit means external clock");

  constexpr std::uint32_t kReservedBit = 1U << 31U;
  const auto reserved =
      dt5730_status::DecodeAcquisitionStatus(kReservedBit);
  Check(reserved.raw == kReservedBit && !reserved.run &&
            !reserved.event_ready && !reserved.event_full &&
            !reserved.pll_no_unlock && !reserved.board_ready &&
            !reserved.channel_shutdown &&
            !reserved.dt_temperature_alarm &&
            !reserved.nim_temperature_alarm,
        "unknown bits are retained in raw but do not create known flags");

  const std::uint32_t healthy_raw =
      dt5730_status::kPllNoUnlockMask |
      dt5730_status::kBoardReadyMask |
      dt5730_status::kRunMask | dt5730_status::kEventReadyMask |
      dt5730_status::kEventFullMask |
      dt5730_status::kClockSourceMask;
  Check(!dt5730_status::DecodeAcquisitionStatus(healthy_raw)
             .HasFatalHealthFault(),
        "run/buffer/clock state does not turn a healthy board into a fault");
  Check(dt5730_status::DecodeAcquisitionStatus(
            healthy_raw & ~dt5730_status::kPllNoUnlockMask)
            .HasFatalHealthFault(),
        "cleared active-good PLL flag is fatal");
  Check(dt5730_status::DecodeAcquisitionStatus(
            healthy_raw & ~dt5730_status::kBoardReadyMask)
            .HasFatalHealthFault(),
        "cleared active-good board-ready flag is fatal");
  Check(dt5730_status::DecodeAcquisitionStatus(
            healthy_raw | dt5730_status::kChannelShutdownMask)
            .HasFatalHealthFault(),
        "channel shutdown is fatal");
  Check(dt5730_status::DecodeAcquisitionStatus(
            healthy_raw | dt5730_status::kDtTemperatureAlarmMask)
            .HasTemperatureAlarm(),
        "DT temperature status is detected");
  Check(dt5730_status::DecodeAcquisitionStatus(
            healthy_raw | dt5730_status::kNimTemperatureAlarmMask)
            .HasTemperatureAlarm(),
        "NIM temperature status is detected");
}

void CheckBoardFailureContract() {
  constexpr std::array<FailureFlagCase, 3> cases{{
      {"PLL failure", dt5730_status::kBoardFailurePllMask,
       &dt5730_status::BoardFailureStatus::pll_failure},
      {"temperature failure",
       dt5730_status::kBoardFailureTemperatureMask,
       &dt5730_status::BoardFailureStatus::temperature_failure},
      {"ADC power-down", dt5730_status::kBoardFailureAdcPowerDownMask,
       &dt5730_status::BoardFailureStatus::adc_power_down},
  }};

  for (const auto& test_case : cases) {
    const auto decoded =
        dt5730_status::DecodeBoardFailureStatus(test_case.mask);
    Check(decoded.raw == test_case.mask,
          std::string(test_case.name) + " preserves the raw register");
    Check(decoded.*(test_case.field),
          std::string(test_case.name) + " decodes its documented bit");
    Check(decoded.Any(),
          std::string(test_case.name) + " contributes to Any()");
  }

  const auto none = dt5730_status::DecodeBoardFailureStatus(1U << 31U);
  Check(none.raw == (1U << 31U) && !none.Any(),
        "unknown board-failure bits do not create a known failure");
}

void CheckMockTransitions() {
  caen_mock::state = caen_mock::State{};

  std::uint32_t raw = 0U;
  Check(CAEN_DGTZ_ReadRegister(
            1, dt5730_status::kAcquisitionStatusRegister, &raw) ==
            CAEN_DGTZ_Success,
        "mock acquisition status is readable");
  auto status = dt5730_status::DecodeAcquisitionStatus(raw);
  Check(status.board_ready && status.pll_no_unlock && !status.run,
        "mock reset default is board-ready, PLL-stable, and stopped");

  Check(CAEN_DGTZ_SWStartAcquisition(1) == CAEN_DGTZ_Success,
        "mock acquisition starts");
  CAEN_DGTZ_ReadRegister(
      1, dt5730_status::kAcquisitionStatusRegister, &raw);
  Check(dt5730_status::DecodeAcquisitionStatus(raw).run,
        "mock start asserts the real RUN bit 2");

  Check(CAEN_DGTZ_SendSWtrigger(1) == CAEN_DGTZ_Success,
        "mock software trigger succeeds while running");
  CAEN_DGTZ_ReadRegister(
      1, dt5730_status::kAcquisitionStatusRegister, &raw);
  Check(dt5730_status::DecodeAcquisitionStatus(raw).event_ready,
        "queued mock event asserts event-ready bit 3");

  std::uint32_t bytes_read = 0U;
  Check(CAEN_DGTZ_ReadData(
            1, CAEN_DGTZ_SLAVE_TERMINATED_READOUT_MBLT, nullptr,
            &bytes_read) == CAEN_DGTZ_Success &&
            bytes_read != 0U,
        "mock read consumes the queued event");
  CAEN_DGTZ_ReadRegister(
      1, dt5730_status::kAcquisitionStatusRegister, &raw);
  Check(!dt5730_status::DecodeAcquisitionStatus(raw).event_ready,
        "draining mock data clears event-ready");

  Check(CAEN_DGTZ_WriteRegister(
            1, caen_mock::kAcquisitionControlRegister,
            caen_mock::kExternalClockControlMask) == CAEN_DGTZ_Success,
        "mock accepts external-clock configuration");
  CAEN_DGTZ_ReadRegister(
      1, dt5730_status::kAcquisitionStatusRegister, &raw);
  Check(dt5730_status::DecodeAcquisitionStatus(raw).clock_source ==
            dt5730_status::ClockSource::kExternal,
        "clock-source status follows acquisition-control configuration");

  caen_mock::SetBoardNotReadyFault(true);
  caen_mock::SetPllUnlockFault(true);
  caen_mock::SetEventFullFault(true);
  caen_mock::SetChannelShutdownFault(true);
  caen_mock::SetDtTemperatureFault(true);
  caen_mock::SetNimTemperatureFault(true);
  caen_mock::SetBoardFailureAdcPowerDownFault(true);

  CAEN_DGTZ_ReadRegister(
      1, dt5730_status::kAcquisitionStatusRegister, &raw);
  status = dt5730_status::DecodeAcquisitionStatus(raw);
  Check(!status.board_ready && !status.pll_no_unlock && status.event_full &&
            status.channel_shutdown && status.dt_temperature_alarm &&
            status.nim_temperature_alarm,
        "mock fault setters drive every documented acquisition-status bit");

  std::uint32_t failure_raw = 0U;
  CAEN_DGTZ_ReadRegister(
      1, dt5730_status::kBoardFailureStatusRegister, &failure_raw);
  const auto failure =
      dt5730_status::DecodeBoardFailureStatus(failure_raw);
  Check(failure.pll_failure && failure.temperature_failure &&
            failure.adc_power_down,
        "mock related faults propagate to all board-failure bits");

  Check(CAEN_DGTZ_Reset(1) == CAEN_DGTZ_Success,
        "mock hardware reset succeeds");
  CAEN_DGTZ_ReadRegister(
      1, dt5730_status::kAcquisitionStatusRegister, &raw);
  status = dt5730_status::DecodeAcquisitionStatus(raw);
  Check(!status.run && !status.board_ready && !status.pll_no_unlock &&
            status.event_full && status.channel_shutdown &&
            status.dt_temperature_alarm && status.nim_temperature_alarm,
        "fault injection survives reset while transient RUN state clears");

  caen_mock::SetBoardNotReadyFault(false);
  caen_mock::SetPllUnlockFault(false);
  caen_mock::SetEventFullFault(false);
  caen_mock::SetChannelShutdownFault(false);
  caen_mock::SetDtTemperatureFault(false);
  caen_mock::SetNimTemperatureFault(false);
  caen_mock::SetBoardFailureAdcPowerDownFault(false);
  Check(CAEN_DGTZ_Reset(1) == CAEN_DGTZ_Success,
        "mock resets after clearing injected faults");
  CAEN_DGTZ_ReadRegister(
      1, dt5730_status::kAcquisitionStatusRegister, &raw);
  status = dt5730_status::DecodeAcquisitionStatus(raw);
  CAEN_DGTZ_ReadRegister(
      1, dt5730_status::kBoardFailureStatusRegister, &failure_raw);
  Check(status.board_ready && status.pll_no_unlock &&
            !status.HasTemperatureAlarm() && !status.channel_shutdown &&
            !status.event_full &&
            !dt5730_status::DecodeBoardFailureStatus(failure_raw).Any(),
        "cleared mock faults restore healthy register defaults");

  caen_mock::SetBoardFailurePllFault(true);
  caen_mock::SetBoardFailureTemperatureFault(true);
  CAEN_DGTZ_ReadRegister(
      1, dt5730_status::kBoardFailureStatusRegister, &failure_raw);
  const auto injected_board_failure =
      dt5730_status::DecodeBoardFailureStatus(failure_raw);
  CAEN_DGTZ_ReadRegister(
      1, dt5730_status::kAcquisitionStatusRegister, &raw);
  status = dt5730_status::DecodeAcquisitionStatus(raw);
  Check(injected_board_failure.pll_failure &&
            injected_board_failure.temperature_failure &&
            !injected_board_failure.adc_power_down &&
            status.pll_no_unlock && !status.HasTemperatureAlarm(),
        "board-failure register faults can be injected independently");
  Check(CAEN_DGTZ_Reset(1) == CAEN_DGTZ_Success,
        "mock reset preserves independent board-failure injections");
  CAEN_DGTZ_ReadRegister(
      1, dt5730_status::kBoardFailureStatusRegister, &failure_raw);
  Check(dt5730_status::DecodeBoardFailureStatus(failure_raw).pll_failure &&
            dt5730_status::DecodeBoardFailureStatus(failure_raw)
                .temperature_failure,
        "independent board-failure injection remains active after reset");
  caen_mock::SetBoardFailurePllFault(false);
  caen_mock::SetBoardFailureTemperatureFault(false);

  caen_mock::SetRunStatusStuckLow(true);
  CAEN_DGTZ_SWStartAcquisition(1);
  CAEN_DGTZ_ReadRegister(
      1, dt5730_status::kAcquisitionStatusRegister, &raw);
  Check(!dt5730_status::DecodeAcquisitionStatus(raw).run,
        "stuck-low injection suppresses the real RUN bit");
  caen_mock::SetRunStatusStuckLow(false);
  CAEN_DGTZ_ReadRegister(
      1, dt5730_status::kAcquisitionStatusRegister, &raw);
  Check(dt5730_status::DecodeAcquisitionStatus(raw).run,
        "clearing stuck-low injection restores RUN while acquisition is active");
  Check(CAEN_DGTZ_SWStopAcquisition(1) == CAEN_DGTZ_Success,
        "mock acquisition stops");
  CAEN_DGTZ_ReadRegister(
      1, dt5730_status::kAcquisitionStatusRegister, &raw);
  Check(!dt5730_status::DecodeAcquisitionStatus(raw).run,
        "mock stop clears the real RUN bit 2");

  caen_mock::SetRuntimeHealthReadFailure(true);
  Check(CAEN_DGTZ_ReadRegister(
            1, dt5730_status::kBoardFailureStatusRegister, &failure_raw) ==
            CAEN_DGTZ_GenericError,
        "runtime health read fault covers board-failure status register");
  caen_mock::SetRuntimeHealthReadFailure(false);
}

}  // namespace

int main() {
  CheckRegisterContract();
  CheckBoardFailureContract();
  CheckMockTransitions();

  if (failures != 0) {
    std::cerr << failures << " DT5730 status test(s) failed\n";
    return 1;
  }
  std::cout << "DT5730 status register tests passed\n";
  return 0;
}
