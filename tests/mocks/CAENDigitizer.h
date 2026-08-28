#ifndef TESTS_MOCKS_CAEN_DIGITIZER_H
#define TESTS_MOCKS_CAEN_DIGITIZER_H

#include "DT5730Status.h"
#include "DataQuality.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

typedef enum {
  CAEN_DGTZ_Success = 0,
  CAEN_DGTZ_GenericError = -1,
} CAEN_DGTZ_ErrorCode;

typedef enum {
  CAEN_DGTZ_USB = 0,
} CAEN_DGTZ_ConnectionType;

typedef enum {
  CAEN_DGTZ_TriggerOnRisingEdge = 0,
  CAEN_DGTZ_TriggerOnFallingEdge = 1,
} CAEN_DGTZ_TriggerPolarity_t;

typedef enum {
  CAEN_DGTZ_TRGMODE_DISABLED = 0,
  CAEN_DGTZ_TRGMODE_ACQ_ONLY = 1,
} CAEN_DGTZ_TriggerMode_t;

typedef enum {
  CAEN_DGTZ_SW_CONTROLLED = 0,
} CAEN_DGTZ_AcqMode_t;

typedef enum {
  CAEN_DGTZ_RUN_SYNC_Disabled = 0,
  CAEN_DGTZ_RUN_SYNC_TrgOutTrgInDaisyChain = 1,
  CAEN_DGTZ_RUN_SYNC_TrgOutSinDaisyChain = 2,
  CAEN_DGTZ_RUN_SYNC_SinFanout = 3,
  CAEN_DGTZ_RUN_SYNC_GpioGpioDaisyChain = 4,
} CAEN_DGTZ_RunSyncMode_t;

typedef enum {
  CAEN_DGTZ_SLAVE_TERMINATED_READOUT_MBLT = 0,
} CAEN_DGTZ_ReadMode_t;

constexpr int CAEN_DGTZ_XX730_FAMILY_CODE = 730;

struct CAEN_DGTZ_BoardInfo_t {
  char ModelName[32];
  char ROC_FirmwareRel[32];
  char AMC_FirmwareRel[32];
  uint32_t SerialNumber;
  int FamilyCode;
  uint32_t ADC_NBits;
};

struct CAEN_DGTZ_EventInfo_t {
  uint32_t EventSize;
  uint32_t BoardId;
  uint32_t Pattern;
  uint32_t ChannelMask;
  uint32_t EventCounter;
  uint32_t TriggerTimeTag;
};

struct CAEN_DGTZ_UINT16_EVENT_t {
  uint32_t ChSize[8];
  uint16_t* DataChannel[8];
};

namespace caen_mock {

constexpr uint32_t kGlobalTriggerMaskRegister = 0x810C;
constexpr uint32_t kAcquisitionControlRegister = 0x8100;
constexpr uint32_t kExternalClockControlMask = 1U << 6U;

struct State {
  bool unstable_baseline = false;
  bool corrupt_threshold_readback = false;
  bool corrupt_pair_logic_readback = false;
  bool corrupt_channel_mask_readback = false;
  bool corrupt_adc_bits = false;
  bool readout_failure = false;
  bool corrupt_event_record_length = false;
  int decode_failure_event_index = -1;
  uint32_t current_event_index = 0;
  bool runtime_health_read_failure = false;
  uint32_t runtime_temperature_c = 25U;
  bool null_active_waveform = false;
  bool stop_acquisition_failure = false;
  bool run_status_stuck_low = false;
  bool board_not_ready_fault = false;
  bool pll_unlock_fault = false;
  bool event_full_fault = false;
  bool channel_shutdown_fault = false;
  bool dt_temperature_fault = false;
  bool nim_temperature_fault = false;
  bool board_failure_pll_fault = false;
  bool board_failure_temperature_fault = false;
  bool board_failure_adc_power_down_fault = false;
  int event_header_board_failure_index = -1;
  bool force_zero_event_count = false;
  bool acquisition_running = false;
  uint32_t channel_mask = 0;
  uint32_t record_length = 512;
  uint32_t post_trigger = 60;
  uint32_t pending_events = 0;
  uint32_t current_read_events = 0;
  uint32_t baseline_batch = 0;
  uint32_t software_triggers_sent = 0;
  CAEN_DGTZ_RunSyncMode_t run_sync_mode = CAEN_DGTZ_RUN_SYNC_Disabled;
  std::array<uint32_t, 8> dc_offsets{};
  std::array<uint32_t, 8> thresholds{};
  std::array<CAEN_DGTZ_TriggerPolarity_t, 8> polarities{};
  std::map<uint32_t, uint32_t> registers{
      {dt5730_status::kAcquisitionStatusRegister,
       dt5730_status::kPllNoUnlockMask |
           dt5730_status::kBoardReadyMask},
      {dt5730_status::kBoardFailureStatusRegister, 0U},
  };
  std::array<std::vector<uint16_t>, 8> traces;
  CAEN_DGTZ_UINT16_EVENT_t decoded_event{};
  std::array<uint32_t, 4> event_words{};
  std::array<char, 4096> readout_buffer{};
  std::vector<uint32_t> event_counter_sequence;
  std::vector<uint32_t> trigger_time_tag_sequence;

  void ApplyStatusFaults() {
    auto& acquisition_status =
        registers[dt5730_status::kAcquisitionStatusRegister];
    const auto assign_bit = [](uint32_t& target, uint32_t mask,
                               bool enabled) {
      if (enabled) target |= mask;
      else target &= ~mask;
    };

    assign_bit(acquisition_status, dt5730_status::kBoardReadyMask,
               !board_not_ready_fault);
    assign_bit(acquisition_status, dt5730_status::kPllNoUnlockMask,
               !pll_unlock_fault);
    assign_bit(acquisition_status, dt5730_status::kEventFullMask,
               event_full_fault);
    assign_bit(acquisition_status, dt5730_status::kChannelShutdownMask,
               channel_shutdown_fault);
    assign_bit(acquisition_status, dt5730_status::kDtTemperatureAlarmMask,
               dt_temperature_fault);
    assign_bit(acquisition_status, dt5730_status::kNimTemperatureAlarmMask,
               nim_temperature_fault);

    auto& board_failure =
        registers[dt5730_status::kBoardFailureStatusRegister];
    assign_bit(board_failure, dt5730_status::kBoardFailurePllMask,
               pll_unlock_fault || board_failure_pll_fault);
    assign_bit(board_failure, dt5730_status::kBoardFailureTemperatureMask,
               dt_temperature_fault || nim_temperature_fault ||
                   board_failure_temperature_fault);
    assign_bit(board_failure,
               dt5730_status::kBoardFailureAdcPowerDownMask,
               board_failure_adc_power_down_fault);
  }

  void ResetHardware() {
    const bool preserve_unstable = unstable_baseline;
    const bool preserve_threshold_fault = corrupt_threshold_readback;
    const bool preserve_pair_fault = corrupt_pair_logic_readback;
    const bool preserve_channel_mask_fault = corrupt_channel_mask_readback;
    const bool preserve_adc_bits_fault = corrupt_adc_bits;
    const bool preserve_board_not_ready_fault = board_not_ready_fault;
    const bool preserve_pll_unlock_fault = pll_unlock_fault;
    const bool preserve_event_full_fault = event_full_fault;
    const bool preserve_channel_shutdown_fault = channel_shutdown_fault;
    const bool preserve_dt_temperature_fault = dt_temperature_fault;
    const bool preserve_nim_temperature_fault = nim_temperature_fault;
    const bool preserve_board_failure_pll_fault =
        board_failure_pll_fault;
    const bool preserve_board_failure_temperature_fault =
        board_failure_temperature_fault;
    const bool preserve_board_failure_adc_power_down_fault =
        board_failure_adc_power_down_fault;
    const int preserve_event_header_board_failure_index =
        event_header_board_failure_index;
    *this = State{};
    unstable_baseline = preserve_unstable;
    corrupt_threshold_readback = preserve_threshold_fault;
    corrupt_pair_logic_readback = preserve_pair_fault;
    corrupt_channel_mask_readback = preserve_channel_mask_fault;
    corrupt_adc_bits = preserve_adc_bits_fault;
    board_not_ready_fault = preserve_board_not_ready_fault;
    pll_unlock_fault = preserve_pll_unlock_fault;
    event_full_fault = preserve_event_full_fault;
    channel_shutdown_fault = preserve_channel_shutdown_fault;
    dt_temperature_fault = preserve_dt_temperature_fault;
    nim_temperature_fault = preserve_nim_temperature_fault;
    board_failure_pll_fault = preserve_board_failure_pll_fault;
    board_failure_temperature_fault =
        preserve_board_failure_temperature_fault;
    board_failure_adc_power_down_fault =
        preserve_board_failure_adc_power_down_fault;
    event_header_board_failure_index =
        preserve_event_header_board_failure_index;
    ApplyStatusFaults();
  }

  void BuildTraceBatch() {
    const uint16_t ch0_baseline = static_cast<uint16_t>(
        unstable_baseline ? 15000U + 10U * (baseline_batch % 100U)
                          : 16164U);
    const uint16_t ch1_baseline = static_cast<uint16_t>(
        unstable_baseline ? 15100U + 10U * (baseline_batch % 100U)
                          : 16255U);
    ++baseline_batch;

    for (std::size_t ch = 0; ch < traces.size(); ++ch) {
      const uint16_t baseline =
          ch == 0U ? ch0_baseline : (ch == 1U ? ch1_baseline : 8192U);
      const uint32_t trace_length =
          corrupt_event_record_length ? record_length / 2U : record_length;
      traces[ch].assign(trace_length, baseline);
      decoded_event.ChSize[ch] =
          ((channel_mask >> ch) & 1U) != 0U ? trace_length : 0U;
      decoded_event.DataChannel[ch] = traces[ch].empty()
                                          ? nullptr
                                          : traces[ch].data();
      if (null_active_waveform && ch == 0U) {
        decoded_event.DataChannel[ch] = nullptr;
      }
    }
  }
};

inline State state;
inline bool reset_should_fail = false;
inline uint32_t open_calls = 0;
inline uint32_t close_calls = 0;
inline uint32_t reset_calls = 0;
inline uint32_t last_open_link = 0;
inline int last_open_node = 0;
inline uint32_t last_open_base_address = 0;

inline void ResetLifecycleInstrumentation() {
  reset_should_fail = false;
  open_calls = 0;
  close_calls = 0;
  reset_calls = 0;
  last_open_link = 0;
  last_open_node = 0;
  last_open_base_address = 0;
}

inline void SetResetFailure(bool enabled) {
  reset_should_fail = enabled;
}

inline void SetUnstableBaseline(bool enabled) {
  state.unstable_baseline = enabled;
}

inline void SetThresholdReadbackFault(bool enabled) {
  state.corrupt_threshold_readback = enabled;
}

inline void SetPairLogicReadbackFault(bool enabled) {
  state.corrupt_pair_logic_readback = enabled;
}

inline void SetChannelMaskReadbackFault(bool enabled) {
  state.corrupt_channel_mask_readback = enabled;
}

inline void SetAdcBitsFault(bool enabled) {
  state.corrupt_adc_bits = enabled;
}

inline void SetReadoutFailure(bool enabled) {
  state.readout_failure = enabled;
}

inline void SetCorruptEventRecordLength(bool enabled) {
  state.corrupt_event_record_length = enabled;
}

inline void SetDecodeFailureEventIndex(int event_index) {
  state.decode_failure_event_index = event_index;
}

inline void SetRuntimeHealthReadFailure(bool enabled) {
  state.runtime_health_read_failure = enabled;
}

inline void SetRuntimeTemperature(uint32_t temperature_c) {
  state.runtime_temperature_c = temperature_c;
}

inline void SetNullActiveWaveform(bool enabled) {
  state.null_active_waveform = enabled;
}

inline void SetStopAcquisitionFailure(bool enabled) {
  state.stop_acquisition_failure = enabled;
}

inline void SetRunStatusStuckLow(bool enabled) {
  state.run_status_stuck_low = enabled;
  auto& status =
      state.registers[dt5730_status::kAcquisitionStatusRegister];
  if (enabled || !state.acquisition_running) {
    status &= ~dt5730_status::kRunMask;
  } else {
    status |= dt5730_status::kRunMask;
  }
}

inline void SetBoardNotReadyFault(bool enabled) {
  state.board_not_ready_fault = enabled;
  state.ApplyStatusFaults();
}

inline void SetPllUnlockFault(bool enabled) {
  state.pll_unlock_fault = enabled;
  state.ApplyStatusFaults();
}

inline void SetEventFullFault(bool enabled) {
  state.event_full_fault = enabled;
  state.ApplyStatusFaults();
}

inline void SetChannelShutdownFault(bool enabled) {
  state.channel_shutdown_fault = enabled;
  state.ApplyStatusFaults();
}

inline void SetDtTemperatureFault(bool enabled) {
  state.dt_temperature_fault = enabled;
  state.ApplyStatusFaults();
}

inline void SetNimTemperatureFault(bool enabled) {
  state.nim_temperature_fault = enabled;
  state.ApplyStatusFaults();
}

inline void SetBoardFailurePllFault(bool enabled) {
  state.board_failure_pll_fault = enabled;
  state.ApplyStatusFaults();
}

inline void SetBoardFailureTemperatureFault(bool enabled) {
  state.board_failure_temperature_fault = enabled;
  state.ApplyStatusFaults();
}

inline void SetBoardFailureAdcPowerDownFault(bool enabled) {
  state.board_failure_adc_power_down_fault = enabled;
  state.ApplyStatusFaults();
}

inline void SetForceZeroEventCount(bool enabled) {
  state.force_zero_event_count = enabled;
}

inline void SetEventCounterSequence(std::vector<uint32_t> counters) {
  state.event_counter_sequence = std::move(counters);
}

inline void SetTriggerTimeTagSequence(std::vector<uint32_t> tags) {
  state.trigger_time_tag_sequence = std::move(tags);
}

inline void SetEventHeaderBoardFailureIndex(int event_index) {
  state.event_header_board_failure_index = event_index;
}

}  // namespace caen_mock

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_OpenDigitizer2(
    CAEN_DGTZ_ConnectionType, void* link, int node, uint32_t base_address,
    int* handle) {
  ++caen_mock::open_calls;
  caen_mock::last_open_link =
      link == nullptr ? 0U : *static_cast<uint32_t*>(link);
  caen_mock::last_open_node = node;
  caen_mock::last_open_base_address = base_address;
  *handle = 1;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_CloseDigitizer(int) {
  ++caen_mock::close_calls;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_Reset(int) {
  ++caen_mock::reset_calls;
  if (caen_mock::reset_should_fail) return CAEN_DGTZ_GenericError;
  caen_mock::state.ResetHardware();
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_GetInfo(
    int, CAEN_DGTZ_BoardInfo_t* info) {
  std::memset(info, 0, sizeof(*info));
  std::strcpy(info->ModelName, "MOCK-DT5730S");
  std::strcpy(info->ROC_FirmwareRel, "3.9");
  std::strcpy(info->AMC_FirmwareRel, "4.8");
  info->SerialNumber = 5730U;
  info->FamilyCode = CAEN_DGTZ_XX730_FAMILY_CODE;
  info->ADC_NBits = caen_mock::state.corrupt_adc_bits ? 12U : 14U;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_ReadRegister(
    int, uint32_t address, uint32_t* value) {
  const bool temperature_register =
      address >= 0x10A8U && address <= 0x17A8U &&
      ((address - 0x10A8U) % 0x100U) == 0U;
  if (caen_mock::state.runtime_health_read_failure &&
      (temperature_register ||
       address == dt5730_status::kAcquisitionStatusRegister ||
       address == dt5730_status::kBoardFailureStatusRegister)) {
    return CAEN_DGTZ_GenericError;
  }
  if (temperature_register) {
    *value = caen_mock::state.runtime_temperature_c;
    return CAEN_DGTZ_Success;
  }
  *value = caen_mock::state.registers[address];
  if (address == 0x1084U &&
      caen_mock::state.corrupt_pair_logic_readback) {
    *value ^= 0x1U;
  }
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_WriteRegister(
    int, uint32_t address, uint32_t value) {
  caen_mock::state.registers[address] = value;
  if (address == caen_mock::kAcquisitionControlRegister) {
    auto& status = caen_mock::state.registers[
        dt5730_status::kAcquisitionStatusRegister];
    if ((value & caen_mock::kExternalClockControlMask) != 0U) {
      status |= dt5730_status::kClockSourceMask;
    } else {
      status &= ~dt5730_status::kClockSourceMask;
    }
  }
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_SetRecordLength(int, uint32_t length) {
  caen_mock::state.record_length = length;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_GetRecordLength(int, uint32_t* length) {
  *length = caen_mock::state.record_length;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_SetChannelEnableMask(
    int, uint32_t mask) {
  caen_mock::state.channel_mask = mask;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_GetChannelEnableMask(
    int, uint32_t* mask) {
  *mask = caen_mock::state.channel_mask;
  if (caen_mock::state.corrupt_channel_mask_readback) *mask ^= 0x1U;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_SetPostTriggerSize(
    int, uint32_t post_trigger) {
  caen_mock::state.post_trigger = post_trigger;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_GetPostTriggerSize(
    int, uint32_t* post_trigger) {
  *post_trigger = caen_mock::state.post_trigger;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_SetAcquisitionMode(
    int, CAEN_DGTZ_AcqMode_t) {
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_SetRunSynchronizationMode(
    int, CAEN_DGTZ_RunSyncMode_t mode) {
  caen_mock::state.run_sync_mode = mode;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_GetRunSynchronizationMode(
    int, CAEN_DGTZ_RunSyncMode_t* mode) {
  *mode = caen_mock::state.run_sync_mode;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_SetExtTriggerInputMode(
    int, CAEN_DGTZ_TriggerMode_t mode) {
  auto& value =
      caen_mock::state.registers[caen_mock::kGlobalTriggerMaskRegister];
  if (mode == CAEN_DGTZ_TRGMODE_DISABLED) value &= ~(1U << 30);
  else value |= 1U << 30;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_SetChannelSelfTrigger(
    int, CAEN_DGTZ_TriggerMode_t mode, uint32_t channel_mask) {
  auto& value =
      caen_mock::state.registers[caen_mock::kGlobalTriggerMaskRegister];
  value &= ~0x0FU;
  if (mode != CAEN_DGTZ_TRGMODE_DISABLED) {
    for (int pair = 0; pair < 4; ++pair) {
      if ((channel_mask & (0x3U << (2 * pair))) != 0U) {
        value |= 1U << pair;
      }
    }
  }
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_SetSWTriggerMode(
    int, CAEN_DGTZ_TriggerMode_t mode) {
  auto& value =
      caen_mock::state.registers[caen_mock::kGlobalTriggerMaskRegister];
  if (mode == CAEN_DGTZ_TRGMODE_DISABLED) value &= ~(1U << 31);
  else value |= 1U << 31;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_SetChannelDCOffset(
    int, uint32_t channel, uint32_t value) {
  caen_mock::state.dc_offsets.at(channel) = value;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_GetChannelDCOffset(
    int, uint32_t channel, uint32_t* value) {
  *value = caen_mock::state.dc_offsets.at(channel);
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_SetTriggerPolarity(
    int, uint32_t channel, CAEN_DGTZ_TriggerPolarity_t polarity) {
  caen_mock::state.polarities.at(channel) = polarity;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_GetTriggerPolarity(
    int, uint32_t channel, CAEN_DGTZ_TriggerPolarity_t* polarity) {
  *polarity = caen_mock::state.polarities.at(channel);
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_SetChannelTriggerThreshold(
    int, uint32_t channel, uint32_t value) {
  caen_mock::state.thresholds.at(channel) = value;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_GetChannelTriggerThreshold(
    int, uint32_t channel, uint32_t* value) {
  *value = caen_mock::state.thresholds.at(channel);
  if (caen_mock::state.corrupt_threshold_readback) ++*value;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_MallocReadoutBuffer(
    int, char** buffer, uint32_t* size) {
  *buffer = caen_mock::state.readout_buffer.data();
  *size = static_cast<uint32_t>(caen_mock::state.readout_buffer.size());
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_FreeReadoutBuffer(char** buffer) {
  *buffer = nullptr;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_AllocateEvent(int, void** event) {
  *event = &caen_mock::state.decoded_event;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_FreeEvent(int, void** event) {
  *event = nullptr;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_ClearData(int) {
  caen_mock::state.pending_events = 0;
  caen_mock::state.current_read_events = 0;
  caen_mock::state.registers[dt5730_status::kAcquisitionStatusRegister] &=
      ~dt5730_status::kEventReadyMask;
  caen_mock::state.ApplyStatusFaults();
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_SWStartAcquisition(int) {
  caen_mock::state.acquisition_running = true;
  if (!caen_mock::state.run_status_stuck_low) {
    caen_mock::state.registers[dt5730_status::kAcquisitionStatusRegister] |=
        dt5730_status::kRunMask;
  }
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_SWStopAcquisition(int) {
  if (caen_mock::state.stop_acquisition_failure) {
    return CAEN_DGTZ_GenericError;
  }
  caen_mock::state.acquisition_running = false;
  caen_mock::state.registers[dt5730_status::kAcquisitionStatusRegister] &=
      ~dt5730_status::kRunMask;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_SendSWtrigger(int) {
  if (!caen_mock::state.acquisition_running) return CAEN_DGTZ_GenericError;
  ++caen_mock::state.pending_events;
  ++caen_mock::state.software_triggers_sent;
  caen_mock::state.registers[dt5730_status::kAcquisitionStatusRegister] |=
      dt5730_status::kEventReadyMask;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_ReadData(
    int, CAEN_DGTZ_ReadMode_t, char*, uint32_t* bytes_read) {
  if (caen_mock::state.readout_failure) return CAEN_DGTZ_GenericError;
  if (caen_mock::state.pending_events == 0U) {
    *bytes_read = 0;
    caen_mock::state.registers[dt5730_status::kAcquisitionStatusRegister] &=
        ~dt5730_status::kEventReadyMask;
    caen_mock::state.ApplyStatusFaults();
    return CAEN_DGTZ_Success;
  }
  caen_mock::state.current_read_events = caen_mock::state.pending_events;
  caen_mock::state.pending_events = 0;
  caen_mock::state.BuildTraceBatch();
  *bytes_read = caen_mock::state.current_read_events * 16U;
  caen_mock::state.registers[dt5730_status::kAcquisitionStatusRegister] &=
      ~dt5730_status::kEventReadyMask;
  caen_mock::state.ApplyStatusFaults();
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_GetNumEvents(
    int, char*, uint32_t, uint32_t* event_count) {
  *event_count = caen_mock::state.force_zero_event_count
                     ? 0U
                     : caen_mock::state.current_read_events;
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_GetEventInfo(
    int, char*, uint32_t, uint32_t event_index,
    CAEN_DGTZ_EventInfo_t* info, char** event_pointer) {
  *info = CAEN_DGTZ_EventInfo_t{};
  info->EventSize =
      static_cast<uint32_t>(caen_mock::state.event_words.size() *
                            sizeof(uint32_t));
  info->Pattern = 0xA55AU;
  info->ChannelMask = caen_mock::state.channel_mask;
  info->EventCounter =
      event_index < caen_mock::state.event_counter_sequence.size()
          ? caen_mock::state.event_counter_sequence[event_index]
          : event_index;
  info->TriggerTimeTag =
      event_index < caen_mock::state.trigger_time_tag_sequence.size()
          ? caen_mock::state.trigger_time_tag_sequence[event_index]
          : event_index * 2U;
  caen_mock::state.current_event_index = event_index;
  caen_mock::state.event_words = {};
  caen_mock::state.event_words[0] =
      0xA0000000U |
      static_cast<uint32_t>(caen_mock::state.event_words.size());
  caen_mock::state.event_words[1] =
      ((info->Pattern & 0xFFFFU) << 8U) | (info->ChannelMask & 0xFFU);
  if (caen_mock::state.event_header_board_failure_index >= 0 &&
      event_index == static_cast<uint32_t>(
                         caen_mock::state.event_header_board_failure_index)) {
    caen_mock::state.event_words[1] |=
        cpnr::kCaenEventHeaderBoardFailureMask;
  }
  caen_mock::state.event_words[2] = info->EventCounter & 0xFFFFFFU;
  caen_mock::state.event_words[3] = info->TriggerTimeTag;
  *event_pointer = reinterpret_cast<char*>(caen_mock::state.event_words.data());
  return CAEN_DGTZ_Success;
}

inline CAEN_DGTZ_ErrorCode CAEN_DGTZ_DecodeEvent(
    int, char*, void** event) {
  if (caen_mock::state.decode_failure_event_index >= 0 &&
      caen_mock::state.current_event_index ==
          static_cast<uint32_t>(caen_mock::state.decode_failure_event_index)) {
    return CAEN_DGTZ_GenericError;
  }
  *event = &caen_mock::state.decoded_event;
  return CAEN_DGTZ_Success;
}

#endif  // TESTS_MOCKS_CAEN_DIGITIZER_H
