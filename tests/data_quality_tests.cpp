#include "DataQuality.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>

int main() {
  const cpnr::LostEventPolicy strict{};
  if (cpnr::LostEventPolicyExceeded(100U, 0U, strict) ||
      !cpnr::LostEventPolicyExceeded(100U, 1U, strict)) {
    std::cerr << "Strict loss policy boundary failed\n";
    return 1;
  }

  const cpnr::LostEventPolicy bounded{2U, 0.02};
  if (cpnr::LostEventPolicyExceeded(98U, 2U, bounded) ||
      !cpnr::LostEventPolicyExceeded(97U, 3U, bounded) ||
      !cpnr::LostEventPolicyExceeded(49U, 1U,
                                     cpnr::LostEventPolicy{2U, 0.01})) {
    std::cerr << "Configured loss policy boundary failed\n";
    return 1;
  }
  if (std::abs(cpnr::LostEventFraction(98U, 2U) - 0.02) > 1e-15) {
    std::cerr << "Loss fraction calculation failed\n";
    return 1;
  }

  const double exact_fraction = cpnr::LostEventFraction(2U, 1U);
  const auto exact_boundary = cpnr::EvaluateLostEventPolicy(
      2U, 1U, cpnr::LostEventPolicy{1U, exact_fraction});
  if (exact_boundary.Exceeded() || exact_boundary.count_exceeded ||
      exact_boundary.fraction_exceeded ||
      exact_boundary.observed_events != 2U ||
      exact_boundary.inferred_lost_events != 1U ||
      exact_boundary.lost_fraction != exact_fraction) {
    std::cerr << "Loss policy must allow equality at both boundaries\n";
    return 1;
  }
  const auto count_failure = cpnr::EvaluateLostEventPolicy(
      2U, 2U, cpnr::LostEventPolicy{1U, 1.0});
  const auto fraction_failure = cpnr::EvaluateLostEventPolicy(
      2U, 1U, cpnr::LostEventPolicy{10U, 0.25});
  if (!count_failure.Exceeded() || !count_failure.count_exceeded ||
      count_failure.fraction_exceeded || !fraction_failure.Exceeded() ||
      fraction_failure.count_exceeded ||
      !fraction_failure.fraction_exceeded) {
    std::cerr << "Loss policy count/fraction failure attribution failed\n";
    return 1;
  }
  const auto empty = cpnr::EvaluateLostEventPolicy(
      0U, 0U, cpnr::LostEventPolicy{});
  if (empty.Exceeded() || empty.lost_fraction != 0.0) {
    std::cerr << "Empty loss-policy observation failed\n";
    return 1;
  }

  std::array<std::uint32_t, 4> raw_header{
      0xA0000004U, 0x00A55A0FU, 7U, 11U};
  std::uint32_t observed_second_word = 0U;
  if (cpnr::InspectCaenEventHeader(raw_header.data(), sizeof(raw_header),
                                   &observed_second_word) !=
          cpnr::CaenEventHeaderQuality::kOk ||
      observed_second_word != raw_header[1]) {
    std::cerr << "Healthy raw CAEN event header inspection failed\n";
    return 1;
  }
  raw_header[1] |= cpnr::kCaenEventHeaderBoardFailureMask;
  if (cpnr::InspectCaenEventHeader(raw_header.data(), sizeof(raw_header)) !=
      cpnr::CaenEventHeaderQuality::kBoardFailure) {
    std::cerr << "Raw event-header Board Fail bit was not detected\n";
    return 1;
  }
  raw_header[1] &= ~cpnr::kCaenEventHeaderBoardFailureMask;
  raw_header[2] |= cpnr::kCaenEventHeaderBoardFailureMask;
  if (cpnr::InspectCaenEventHeader(raw_header.data(), sizeof(raw_header)) !=
      cpnr::CaenEventHeaderQuality::kOk ||
      cpnr::InspectCaenEventHeader(raw_header.data(),
                                   cpnr::kCaenStandardEventHeaderBytes - 1U) !=
          cpnr::CaenEventHeaderQuality::kTruncated ||
      cpnr::InspectCaenEventHeader(nullptr, sizeof(raw_header)) !=
          cpnr::CaenEventHeaderQuality::kTruncated) {
    std::cerr << "Raw Board Fail inspection used the wrong word or accepted "
                 "a truncated header\n";
    return 1;
  }
  raw_header[0] = 0xB0000004U;
  if (cpnr::InspectCaenEventHeader(raw_header.data(), sizeof(raw_header)) !=
      cpnr::CaenEventHeaderQuality::kMalformed) {
    std::cerr << "Raw event header accepted a non-standard tag\n";
    return 1;
  }
  raw_header[0] = 0xA0000005U;
  if (cpnr::InspectCaenEventHeader(raw_header.data(), sizeof(raw_header)) !=
      cpnr::CaenEventHeaderQuality::kMalformed) {
    std::cerr << "Raw event header accepted an inconsistent event size\n";
    return 1;
  }
  std::cout << "Data-quality policy tests passed\n";
  return 0;
}
