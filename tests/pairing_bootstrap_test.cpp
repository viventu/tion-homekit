// Private pairing code bootstrap against an in-memory NVS with one injected
// failure at a time (tests/stubs/nvs.h).

#include "PairingBootstrap.h"
#include "support/check.h"

#include <iostream>

namespace {

using tion_homekit::code_is_allowed;
using tion_homekit::ensure_private_pairing_code;

// A reboot: stored values survive, counters and injected failures do not.
void reboot() {
  fake_nvs::reset(false);
  fake::srp_code.clear();
}

void test_every_nvs_failure_keeps_hap_closed() {
  fake_nvs::reset(true);
  CHECK(ensure_private_pairing_code() && fake_nvs::blob_writes == 1 &&
            fake_nvs::open_handles == 0,
        "new verifier and marker must be committed before starting HAP");
  CHECK(fake::srp_code.size() == 8 && code_is_allowed(fake::srp_code.c_str()),
        "the verifier is made from an allowed eight-digit code");
  const auto operations = fake_nvs::operations;
  for (int index = 1; index <= operations; ++index) {
    fake_nvs::reset(true);
    fake_nvs::fail_at = index;
    CHECK(!ensure_private_pairing_code() && fake_nvs::open_handles == 0,
          "every failed NVS open/write/commit prevents HAP startup and closes handles");
  }
}

void test_stored_verifier_is_checked() {
  fake_nvs::reset(true);
  CHECK(ensure_private_pairing_code(), "first boot");
  reboot();
  CHECK(ensure_private_pairing_code() && fake_nvs::blob_writes == 0 &&
            fake::srp_code.empty(),
        "an existing private verifier survives reboot without NVS writes");

  fake_nvs::values["SRP/VERIFYDATA"].resize(4);
  reboot();
  CHECK(ensure_private_pairing_code() && fake_nvs::blob_writes == 1,
        "a truncated verifier is replaced");
  fake_nvs::values["SRP/VERIFYDATA"].resize(sizeof(Verification) + 1);
  reboot();
  CHECK(ensure_private_pairing_code() && fake_nvs::blob_writes == 1,
        "an oversized verifier is replaced");
  fake_nvs::values.erase("SRP/VERIFYDATA");
  reboot();
  CHECK(ensure_private_pairing_code() && fake_nvs::blob_writes == 1,
        "a marker without a verifier is not trusted");
  fake_nvs::values["TIONSETUP/SEEDED"] = {0};
  reboot();
  CHECK(ensure_private_pairing_code() && fake_nvs::blob_writes == 1,
        "an unset marker makes a new verifier");
}

void test_code_generation() {
  CHECK(!code_is_allowed("11111111") && !code_is_allowed("12345678") &&
            !code_is_allowed("87654321") && code_is_allowed("11111112"),
        "HomeSpan's disallowed code patterns are rejected");
  fake_nvs::reset(true);
  fake::random_values = {12345678, 87654321, 22222222, 100000042};
  fake::random_index = 0;
  CHECK(ensure_private_pairing_code() && fake::srp_code == "00000042" &&
            fake::random_index == 4,
        "disallowed codes are skipped and leading zeros keep eight digits");
}

}  // namespace

int main() {
  test_every_nvs_failure_keeps_hap_closed();
  test_stored_verifier_is_checked();
  test_code_generation();
  std::cout << "pairing bootstrap NVS failures: PASS\n";
}
