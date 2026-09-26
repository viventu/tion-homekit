#pragma once
#include <cstdint>
#include <string>

namespace fake {
// The last setup code a verifier was made from.
inline std::string srp_code;
}  // namespace fake

// Deterministic host double. This tests NVS failure handling, not cryptography.
struct Verification {
  std::uint8_t salt[16];
  std::uint8_t verifyCode[384];
};
struct SRP6A {
  void createVerifyCode(const char* code, Verification* verifier) {
    fake::srp_code = code;
    verifier->salt[0] = 42;
    verifier->verifyCode[0] = 24;
  }
};
