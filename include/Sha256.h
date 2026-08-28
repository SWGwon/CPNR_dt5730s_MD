#ifndef CPNR_SHA256_H
#define CPNR_SHA256_H

#include <string>
#include <string_view>
#include <cstdint>

std::string Sha256Hex(std::string_view data);
std::string Sha256FileHex(const std::string& path);
std::string Sha256FileDescriptorHex(int descriptor, uint64_t size_bytes);

#endif  // CPNR_SHA256_H
