#include "Sha256.h"

#include <array>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace {

constexpr std::array<uint32_t, 64> kRoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

uint32_t RotateRight(uint32_t value, unsigned int count) {
  return (value >> count) | (value << (32U - count));
}

class Sha256Context {
 public:
  void Update(const uint8_t* data, std::size_t size) {
    if (size > std::numeric_limits<uint64_t>::max() - total_bytes_) {
      throw std::runtime_error("Input is too large for SHA-256");
    }
    total_bytes_ += static_cast<uint64_t>(size);

    if (buffer_size_ != 0U) {
      const std::size_t copied =
          std::min(size, buffer_.size() - buffer_size_);
      std::memcpy(buffer_.data() + buffer_size_, data, copied);
      buffer_size_ += copied;
      data += copied;
      size -= copied;
      if (buffer_size_ == buffer_.size()) {
        Transform(buffer_.data());
        buffer_size_ = 0U;
      }
    }
    while (size >= buffer_.size()) {
      Transform(data);
      data += buffer_.size();
      size -= buffer_.size();
    }
    if (size != 0U) {
      std::memcpy(buffer_.data(), data, size);
      buffer_size_ = size;
    }
  }

  std::string FinalHex() {
    if (total_bytes_ > std::numeric_limits<uint64_t>::max() / 8U) {
      throw std::runtime_error("Input is too large for SHA-256");
    }
    const uint64_t bit_length = total_bytes_ * 8U;
    buffer_[buffer_size_++] = 0x80U;
    if (buffer_size_ > 56U) {
      std::fill(buffer_.begin() + buffer_size_, buffer_.end(), 0U);
      Transform(buffer_.data());
      buffer_size_ = 0U;
    }
    std::fill(buffer_.begin() + buffer_size_, buffer_.begin() + 56U, 0U);
    for (int shift = 56; shift >= 0; shift -= 8) {
      buffer_[56U + static_cast<std::size_t>((56 - shift) / 8)] =
          static_cast<uint8_t>(bit_length >> shift);
    }
    Transform(buffer_.data());

    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (const uint32_t word : hash_) encoded << std::setw(8) << word;
    return encoded.str();
  }

 private:
  void Transform(const uint8_t* block) {
    std::array<uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16U; ++index) {
      const std::size_t offset = index * 4U;
      words[index] = (static_cast<uint32_t>(block[offset]) << 24U) |
                     (static_cast<uint32_t>(block[offset + 1U]) << 16U) |
                     (static_cast<uint32_t>(block[offset + 2U]) << 8U) |
                     static_cast<uint32_t>(block[offset + 3U]);
    }
    for (std::size_t index = 16U; index < words.size(); ++index) {
      const uint32_t sigma0 = RotateRight(words[index - 15U], 7U) ^
                              RotateRight(words[index - 15U], 18U) ^
                              (words[index - 15U] >> 3U);
      const uint32_t sigma1 = RotateRight(words[index - 2U], 17U) ^
                              RotateRight(words[index - 2U], 19U) ^
                              (words[index - 2U] >> 10U);
      words[index] = words[index - 16U] + sigma0 + words[index - 7U] + sigma1;
    }

    uint32_t a = hash_[0];
    uint32_t b = hash_[1];
    uint32_t c = hash_[2];
    uint32_t d = hash_[3];
    uint32_t e = hash_[4];
    uint32_t f = hash_[5];
    uint32_t g = hash_[6];
    uint32_t h = hash_[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const uint32_t choose = (e & f) ^ ((~e) & g);
      const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t big_sigma0 =
          RotateRight(a, 2U) ^ RotateRight(a, 13U) ^ RotateRight(a, 22U);
      const uint32_t big_sigma1 =
          RotateRight(e, 6U) ^ RotateRight(e, 11U) ^ RotateRight(e, 25U);
      const uint32_t temporary1 =
          h + big_sigma1 + choose + kRoundConstants[index] + words[index];
      const uint32_t temporary2 = big_sigma0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }
    hash_[0] += a;
    hash_[1] += b;
    hash_[2] += c;
    hash_[3] += d;
    hash_[4] += e;
    hash_[5] += f;
    hash_[6] += g;
    hash_[7] += h;
  }

  std::array<uint32_t, 8> hash_ = {
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<uint8_t, 64> buffer_{};
  std::size_t buffer_size_ = 0U;
  uint64_t total_bytes_ = 0U;
};

}  // namespace

struct Sha256Accumulator::Impl {
  Sha256Context context;
  uint64_t size_bytes = 0U;
  bool finalized = false;
  std::string digest;
};

Sha256Accumulator::Sha256Accumulator() : impl_(std::make_unique<Impl>()) {}

Sha256Accumulator::~Sha256Accumulator() = default;

Sha256Accumulator::Sha256Accumulator(Sha256Accumulator&&) noexcept = default;

Sha256Accumulator& Sha256Accumulator::operator=(
    Sha256Accumulator&&) noexcept = default;

void Sha256Accumulator::Update(const void* data, std::size_t size) {
  if (!impl_) throw std::logic_error("SHA-256 accumulator was moved from");
  if (impl_->finalized) {
    throw std::logic_error("Cannot update a finalized SHA-256 accumulator");
  }
  if (size != 0U && data == nullptr) {
    throw std::invalid_argument("Cannot hash a null non-empty buffer");
  }
  if (size == 0U) return;
  if (size > std::numeric_limits<uint64_t>::max() - impl_->size_bytes) {
    throw std::runtime_error("Input is too large for SHA-256");
  }
  impl_->context.Update(static_cast<const uint8_t*>(data), size);
  impl_->size_bytes += static_cast<uint64_t>(size);
}

std::string Sha256Accumulator::FinalHex() {
  if (!impl_) throw std::logic_error("SHA-256 accumulator was moved from");
  if (!impl_->finalized) {
    impl_->digest = impl_->context.FinalHex();
    impl_->finalized = true;
  }
  return impl_->digest;
}

uint64_t Sha256Accumulator::SizeBytes() const noexcept {
  return impl_ ? impl_->size_bytes : 0U;
}

std::string Sha256Hex(std::string_view data) {
  Sha256Accumulator accumulator;
  accumulator.Update(data.data(), data.size());
  return accumulator.FinalHex();
}

std::string Sha256FileHex(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot open file for SHA-256: " + path);
  Sha256Context context;
  std::array<char, 1024U * 1024U> buffer{};
  while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0) {
    context.Update(reinterpret_cast<const uint8_t*>(buffer.data()),
                   static_cast<std::size_t>(input.gcount()));
  }
  if (input.bad()) {
    throw std::runtime_error("Cannot read file for SHA-256: " + path);
  }
  return context.FinalHex();
}

std::string Sha256FileDescriptorHex(int descriptor, uint64_t size_bytes) {
  if (descriptor < 0) {
    throw std::runtime_error("Invalid file descriptor for SHA-256");
  }
  Sha256Context context;
  std::array<uint8_t, 1024U * 1024U> buffer{};
  uint64_t offset = 0U;
  while (offset < size_bytes) {
    const std::size_t requested = static_cast<std::size_t>(
        std::min<uint64_t>(buffer.size(), size_bytes - offset));
    const ssize_t count =
        ::pread(descriptor, buffer.data(), requested, static_cast<off_t>(offset));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) {
      throw std::runtime_error(
          "Cannot read file descriptor for SHA-256 (" +
          std::string(std::strerror(errno)) + ")");
    }
    if (count == 0) {
      throw std::runtime_error(
          "File became shorter while computing SHA-256");
    }
    context.Update(buffer.data(), static_cast<std::size_t>(count));
    offset += static_cast<uint64_t>(count);
  }
  return context.FinalHex();
}
