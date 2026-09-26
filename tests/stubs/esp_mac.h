#pragma once
#include <cstdint>
#include <cstring>

using esp_err_t = int;
enum esp_mac_type_t : std::uint8_t { ESP_MAC_WIFI_STA };

// A fixed, locally administered test address.
inline esp_err_t esp_read_mac(std::uint8_t* mac, esp_mac_type_t) {
  constexpr std::uint8_t kMac[6] = {0x02, 0x6F, 0x28, 0xAB, 0xCD, 0xEF};
  std::memcpy(mac, kMac, sizeof(kMac));
  return 0;
}
