#ifndef CPNR_SHA256_H
#define CPNR_SHA256_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

// Incremental SHA-256 for streams that must be authenticated while they are
// produced.  FinalHex() is idempotent; Update() after finalization is rejected.
// The implementation is hidden so callers do not depend on the compression
// state layout.
class Sha256Accumulator {
 public:
  Sha256Accumulator();
  ~Sha256Accumulator();
  Sha256Accumulator(Sha256Accumulator&&) noexcept;
  Sha256Accumulator& operator=(Sha256Accumulator&&) noexcept;

  Sha256Accumulator(const Sha256Accumulator&) = delete;
  Sha256Accumulator& operator=(const Sha256Accumulator&) = delete;

  void Update(const void* data, std::size_t size);
  std::string FinalHex();
  uint64_t SizeBytes() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::string Sha256Hex(std::string_view data);
std::string Sha256FileHex(const std::string& path);
std::string Sha256FileDescriptorHex(int descriptor, uint64_t size_bytes);

#endif  // CPNR_SHA256_H
