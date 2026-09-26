#include "crc.h"

namespace tion4s {

std::uint16_t crc16_ccitt_false(const std::uint8_t* bytes, std::size_t size) {
  std::uint16_t crc = 0xFFFF;
  for (std::size_t i = 0; i < size; ++i) {
    crc = static_cast<std::uint16_t>(crc ^ (bytes[i] << 8));
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<std::uint16_t>(crc << 1);
    }
  }
  return crc;
}

}  // namespace tion4s
