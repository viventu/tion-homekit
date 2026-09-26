#pragma once

#include <cstddef>
#include <cstdint>

namespace tion4s {

std::uint16_t crc16_ccitt_false(const std::uint8_t* bytes, std::size_t size);

}  // namespace tion4s
