#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fake {
// Values esp_random() returns first, in order; then a fixed one.
inline std::vector<std::uint32_t> random_values;
inline std::size_t random_index = 0;
}  // namespace fake

inline std::uint32_t esp_random() {
  if (fake::random_index < fake::random_values.size()) {
    return fake::random_values[fake::random_index++];
  }
  return 31415926;
}
