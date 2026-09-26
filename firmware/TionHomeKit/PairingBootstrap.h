#pragma once

#include <SRP.h>
#include <esp_random.h>
#include <mbedtls/platform_util.h>
#include <nvs.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace tion_homekit {

inline bool code_is_allowed(const char* code) {
  bool repeated = true;
  for (int i = 1; i < 8; ++i) {
    repeated &= code[i] == code[0];
  }
  return !repeated && strcmp(code, "12345678") != 0 &&
         strcmp(code, "87654321") != 0;
}

// Run before the first HomeSpan poll. Its default-code fallback must never
// become reachable after a failed private-verifier write. HomeSpan 2.1.8
// initializes the Wi-Fi entropy source before Arduino setup().
inline bool ensure_private_pairing_code() {
  nvs_handle_t setup;
  nvs_handle_t srp;
  if (nvs_open("TIONSETUP", NVS_READWRITE, &setup) != ESP_OK) {
    return false;
  }
  if (nvs_open("SRP", NVS_READWRITE, &srp) != ESP_OK) {
    nvs_close(setup);
    return false;
  }

  Verification verifier{};
  std::size_t size = sizeof(verifier);
  std::uint8_t seeded = 0;
  bool ready = nvs_get_u8(setup, "SEEDED", &seeded) == ESP_OK && seeded == 1 &&
               nvs_get_blob(srp, "VERIFYDATA", &verifier, &size) == ESP_OK &&
               size == sizeof(verifier);
  if (!ready) {
    char code[9];
    do {
      // Below 10^8, so always exactly eight digits.
      static_cast<void>(snprintf(code, sizeof(code), "%08lu",
                                 static_cast<unsigned long>(esp_random() % 100000000u)));
    } while (!code_is_allowed(code));
    {
      SRP6A generator;
      generator.createVerifyCode(code, &verifier);
    }
    mbedtls_platform_zeroize(code, sizeof(code));
    // Span::setPairingCode ignores NVS return codes in the pinned version.
    // Use its SRP implementation, but explicitly check both stores/commits.
    ready = nvs_set_blob(srp, "VERIFYDATA", &verifier, sizeof(verifier)) == ESP_OK &&
            nvs_commit(srp) == ESP_OK &&
            nvs_set_u8(setup, "SEEDED", 1) == ESP_OK &&
            nvs_commit(setup) == ESP_OK;
  }
  mbedtls_platform_zeroize(&verifier, sizeof(verifier));
  nvs_close(srp);
  nvs_close(setup);
  return ready;
}

}  // namespace tion_homekit
