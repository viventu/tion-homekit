#pragma once
#include <cstddef>
inline void mbedtls_platform_zeroize(void* buffer, std::size_t size) {
  auto* byte = static_cast<volatile unsigned char*>(buffer);
  while (size-- != 0) {
    *byte++ = 0;
  }
}
