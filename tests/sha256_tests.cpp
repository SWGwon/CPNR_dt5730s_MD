#include "Sha256.h"

#include <iostream>
#include <cstdio>

int main() {
  int failures = 0;
  const auto check = [&](const std::string& actual,
                         const std::string& expected,
                         const char* description) {
    if (actual != expected) {
      std::cerr << "[FAIL] " << description << ": " << actual << '\n';
      ++failures;
    }
  };
  check(Sha256Hex(""),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "empty SHA-256 vector");
  check(Sha256Hex("abc"),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "abc SHA-256 vector");
  std::FILE* fixture = std::tmpfile();
  if (!fixture || std::fwrite("abc", 1, 3, fixture) != 3 ||
      std::fflush(fixture) != 0) {
    std::cerr << "[FAIL] descriptor SHA-256 fixture setup\n";
    ++failures;
  } else {
    check(Sha256FileDescriptorHex(fileno(fixture), 3),
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "descriptor SHA-256 vector");
  }
  if (fixture) std::fclose(fixture);
  return failures == 0 ? 0 : 1;
}
