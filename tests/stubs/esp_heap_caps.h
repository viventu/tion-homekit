#pragma once
#include <cstddef>
#include <cstdint>

constexpr std::uint32_t MALLOC_CAP_8BIT = 1u << 2;
inline std::size_t heap_caps_get_free_size(std::uint32_t) { return 165792; }
inline std::size_t heap_caps_get_minimum_free_size(std::uint32_t) { return 158256; }
inline std::size_t heap_caps_get_largest_free_block(std::uint32_t) { return 110580; }
