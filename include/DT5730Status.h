#ifndef DT5730_STATUS_H
#define DT5730_STATUS_H

#include <cstdint>

// Typed, side-effect-free decoding for the x730 waveform-firmware board
// status registers used by DT5730/DT5730S.  Keep the active-good meaning of
// bit 7 explicit: a cleared PLL-no-unlock flag is itself a fault indication.
namespace dt5730_status {

inline constexpr std::uint32_t kAcquisitionStatusRegister = 0x8104U;
inline constexpr std::uint32_t kBoardFailureStatusRegister = 0x8178U;
inline constexpr std::uint32_t kPllUnlockResetRegister = 0xEF04U;

inline constexpr std::uint32_t kRunMask = 1U << 2U;
inline constexpr std::uint32_t kEventReadyMask = 1U << 3U;
inline constexpr std::uint32_t kEventFullMask = 1U << 4U;
inline constexpr std::uint32_t kClockSourceMask = 1U << 5U;
inline constexpr std::uint32_t kPllNoUnlockMask = 1U << 7U;
inline constexpr std::uint32_t kBoardReadyMask = 1U << 8U;
inline constexpr std::uint32_t kChannelShutdownMask = 1U << 19U;
inline constexpr std::uint32_t kDtTemperatureAlarmMask = 1U << 20U;
inline constexpr std::uint32_t kNimTemperatureAlarmMask = 1U << 21U;

inline constexpr std::uint32_t kBoardFailurePllMask = 1U << 4U;
inline constexpr std::uint32_t kBoardFailureTemperatureMask = 1U << 5U;
inline constexpr std::uint32_t kBoardFailureAdcPowerDownMask = 1U << 6U;

enum class ClockSource : std::uint8_t {
  kInternal = 0,
  kExternal = 1,
};

struct AcquisitionStatus {
  std::uint32_t raw = 0U;
  bool run = false;
  bool event_ready = false;
  bool event_full = false;
  ClockSource clock_source = ClockSource::kInternal;
  bool pll_no_unlock = false;
  bool board_ready = false;
  bool channel_shutdown = false;
  bool dt_temperature_alarm = false;
  bool nim_temperature_alarm = false;

  constexpr bool HasTemperatureAlarm() const noexcept {
    return dt_temperature_alarm || nim_temperature_alarm;
  }

  constexpr bool HasFatalHealthFault() const noexcept {
    return !pll_no_unlock || !board_ready || channel_shutdown ||
           HasTemperatureAlarm();
  }
};

struct BoardFailureStatus {
  std::uint32_t raw = 0U;
  bool pll_failure = false;
  bool temperature_failure = false;
  bool adc_power_down = false;

  constexpr bool Any() const noexcept {
    return pll_failure || temperature_failure || adc_power_down;
  }
};

constexpr AcquisitionStatus DecodeAcquisitionStatus(
    std::uint32_t raw) noexcept {
  return AcquisitionStatus{
      raw,
      (raw & kRunMask) != 0U,
      (raw & kEventReadyMask) != 0U,
      (raw & kEventFullMask) != 0U,
      (raw & kClockSourceMask) != 0U ? ClockSource::kExternal
                                      : ClockSource::kInternal,
      (raw & kPllNoUnlockMask) != 0U,
      (raw & kBoardReadyMask) != 0U,
      (raw & kChannelShutdownMask) != 0U,
      (raw & kDtTemperatureAlarmMask) != 0U,
      (raw & kNimTemperatureAlarmMask) != 0U,
  };
}

constexpr BoardFailureStatus DecodeBoardFailureStatus(
    std::uint32_t raw) noexcept {
  return BoardFailureStatus{
      raw,
      (raw & kBoardFailurePllMask) != 0U,
      (raw & kBoardFailureTemperatureMask) != 0U,
      (raw & kBoardFailureAdcPowerDownMask) != 0U,
  };
}

}  // namespace dt5730_status

#endif  // DT5730_STATUS_H
