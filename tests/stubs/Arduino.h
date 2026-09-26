#pragma once

// Host double of the Arduino core parts the sketch uses. Time only moves when
// a test or delay() advances it; delay() lets the simulated UART task run.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using boolean = bool;

constexpr std::uint8_t LOW = 0;
constexpr std::uint8_t HIGH = 1;
constexpr std::uint8_t OUTPUT = 3;

inline std::uint32_t fake_millis = 0;

namespace fake {

// Runs every 10 ms of delay(), like a task on the other core.
inline std::function<void()> background;
// Every digitalWrite(), in order: (pin, level).
inline std::vector<std::pair<int, int>> pin_writes;
inline std::vector<int> output_pins;

}  // namespace fake

inline std::uint32_t millis() { return fake_millis; }

inline void delay(std::uint32_t ms) {
  const auto end = fake_millis + ms;
  while (static_cast<std::int32_t>(fake_millis - end) < 0) {
    fake_millis += 10;
    if (fake::background) {
      fake::background();
    }
  }
}

inline void pinMode(int pin, std::uint8_t mode) {
  if (mode == OUTPUT) {
    fake::output_pins.push_back(pin);
  }
}

inline void digitalWrite(int pin, int level) {
  fake::pin_writes.emplace_back(pin, level);
}

// The subset of Arduino's String the diagnostics page needs.
class String {
 public:
  String() = default;
  String& operator+=(const char* text) {
    text_ += text;
    return *this;
  }
  void reserve(std::size_t size) { text_.reserve(size); }
  std::size_t length() const { return text_.size(); }
  const char* c_str() const { return text_.c_str(); }

 private:
  std::string text_;
};
