#ifndef EVENT_HEADER_H
#define EVENT_HEADER_H
#include <cstdint>

constexpr int MAX_CH = 8;

#pragma pack(push, 1)
struct EventHeader {
  // Rollover-corrected raw TriggerTimeTag counts.  One raw count is 8 ns;
  // x730 waveform values are observable at 16 ns (two-count) resolution.
  uint64_t ExtendedTTT;
  uint32_t EventID;           // 4 Bytes: Software saved event counter
  uint32_t RecordLength;      // 4 Bytes: Waveform length per channel
  uint16_t ChannelMask;       // 2 Bytes: Active channels
  uint16_t Pattern;           // 2 Bytes: TRG-IN and Board specific patterns
  // Hardware 24-bit event counter used to reconstruct lost triggers.
  uint32_t BoardEventCounter;
};
#pragma pack(pop)

static_assert(sizeof(EventHeader) == 24,
              "Changing EventHeader layout would break the raw file format");

#endif // EVENT_HEADER_H
