#ifndef DT5730_CONSTRAINTS_H
#define DT5730_CONSTRAINTS_H

#include <cstdint>

namespace dt5730_constraints {

inline constexpr uint32_t kMinimumRecordLengthSamples = 128U;
inline constexpr uint32_t kMaximumRecordLengthSamples = 102400U;
inline constexpr uint32_t kRecordLengthGranularitySamples = 10U;
inline constexpr uint32_t kPostTriggerGranularitySamples = 8U;
inline constexpr uint32_t kMinimumPreTriggerSamples = 80U;

constexpr bool IsSupportedRecordLength(uint32_t record_length) noexcept {
  return record_length >= kMinimumRecordLengthSamples &&
         record_length <= kMaximumRecordLengthSamples &&
         record_length % kRecordLengthGranularitySamples == 0U;
}

// CAEN's x730 SetRecordLength implementation rounds a custom request upward
// to the next 10-sample value before programming the N_LOC register.
constexpr uint32_t RoundRecordLengthUp(uint32_t requested) noexcept {
  const uint32_t remainder =
      requested % kRecordLengthGranularitySamples;
  return remainder == 0U
             ? requested
             : requested + kRecordLengthGranularitySamples - remainder;
}

struct PostTriggerLayout {
  uint32_t register_value = 0U;
  uint32_t pre_trigger_samples = 0U;
  uint32_t post_trigger_samples = 0U;
  uint32_t predicted_readback_percent = 0U;
};

// On x730 standard waveform firmware the post-trigger register is expressed
// in 8-sample units. SetPostTriggerSize rounds the requested percentage up to
// a register value K, while GetPostTriggerSize converts K back with truncating
// integer arithmetic.
constexpr PostTriggerLayout PredictPostTriggerLayout(
    uint32_t record_length, uint32_t requested_percent) noexcept {
  if (record_length == 0U) return {};

  const uint32_t available_register_units =
      record_length / kPostTriggerGranularitySamples;
  const uint32_t register_value = static_cast<uint32_t>(
      (static_cast<uint64_t>(requested_percent) *
           available_register_units +
       99U) /
      100U);
  const uint32_t post_trigger_samples =
      register_value * kPostTriggerGranularitySamples;
  const uint32_t pre_trigger_samples =
      record_length - post_trigger_samples;
  const uint32_t predicted_readback_percent = static_cast<uint32_t>(
      static_cast<uint64_t>(post_trigger_samples) * 100U / record_length);
  return {register_value, pre_trigger_samples, post_trigger_samples,
          predicted_readback_percent};
}

static_assert(RoundRecordLengthUp(256U) == 260U);
static_assert(PredictPostTriggerLayout(260U, 27U).register_value == 9U);
static_assert(PredictPostTriggerLayout(260U, 27U).pre_trigger_samples ==
              188U);
static_assert(PredictPostTriggerLayout(260U, 27U).post_trigger_samples ==
              72U);
static_assert(PredictPostTriggerLayout(260U, 27U)
                  .predicted_readback_percent == 27U);
static_assert(PredictPostTriggerLayout(1030U, 70U)
                  .predicted_readback_percent == 69U);

}  // namespace dt5730_constraints

#endif  // DT5730_CONSTRAINTS_H
