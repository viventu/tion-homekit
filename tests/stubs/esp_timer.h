#pragma once
#include <Arduino.h>

#include <cstdint>

inline std::int64_t esp_timer_get_time() { return std::int64_t{fake_millis} * 1000; }
