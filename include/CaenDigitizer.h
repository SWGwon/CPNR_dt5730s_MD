#ifndef CAEN_DIGITIZER_H
#define CAEN_DIGITIZER_H

#include <CAENDigitizer.h>
#include <iostream>
#include <stdexcept>
#include <string>

// CAEN API 에러 검증 매크로
#define CAEN_CHECK(call) \
    do { \
        CAEN_DGTZ_ErrorCode err = (call); \
        if (err != CAEN_DGTZ_Success) { \
            throw std::runtime_error(std::string("CAEN API call failed (") + \
                                     #call + "): error code " + \
                                     std::to_string(err)); \
        } \
    } while(0)

class CaenDigitizer {
public:
  CaenDigitizer(CAEN_DGTZ_ConnectionType linkType, int linkNum, int conetNode, uint32_t vmeBaseAddress)
      : handle_(-1), caen_buffer_(nullptr), caen_event_(nullptr),
        hardware_control_authorized_(false) {
      uint32_t link_arg = static_cast<uint32_t>(linkNum);
      CAEN_CHECK(CAEN_DGTZ_OpenDigitizer2(linkType, &link_arg, conetNode, vmeBaseAddress, &handle_));
  }

  ~CaenDigitizer() {
      if (handle_ >= 0) {
          if (hardware_control_authorized_) {
              CAEN_DGTZ_SWStopAcquisition(handle_);
          }
          if (caen_buffer_) CAEN_DGTZ_FreeReadoutBuffer(&caen_buffer_);
          if (caen_event_) CAEN_DGTZ_FreeEvent(handle_, (void **)&caen_event_);
          CAEN_DGTZ_CloseDigitizer(handle_);
      }
  }

  void AllocateBuffers() {
      if (!hardware_control_authorized_) {
          throw std::logic_error(
              "Cannot allocate acquisition buffers before identity-validated reset");
      }
      uint32_t size = 0;
      CAEN_CHECK(CAEN_DGTZ_MallocReadoutBuffer(handle_, &caen_buffer_, &size));
      // 수집 루프에서의 동적 할당(Memory Corruption 원인)을 피하기 위해 여기서 1회 사전 할당합니다.
      CAEN_CHECK(CAEN_DGTZ_AllocateEvent(handle_, (void **)&caen_event_));
  }

  void Reset() {
      hardware_control_authorized_ = true;
      CAEN_CHECK(CAEN_DGTZ_Reset(handle_));
  }

  int GetHandle() const { return handle_; }
  char *GetReadoutBuffer() const { return caen_buffer_; }
  CAEN_DGTZ_UINT16_EVENT_t *GetDecodedEvent() const { return caen_event_; }

  void WriteRegister(uint32_t reg, uint32_t value) {
      CAEN_CHECK(CAEN_DGTZ_WriteRegister(handle_, reg, value));
  }
  
  uint32_t ReadRegister(uint32_t reg) const {
      uint32_t value = 0;
      CAEN_CHECK(CAEN_DGTZ_ReadRegister(handle_, reg, &value));
      return value;
  }

private:
  int handle_;
  char *caen_buffer_;
  CAEN_DGTZ_UINT16_EVENT_t *caen_event_;
  bool hardware_control_authorized_;
};

#endif // CAEN_DIGITIZER_H
