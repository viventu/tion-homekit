#pragma once

// ControlChannel over UartCore on the host, with a hook that runs between a
// caller's status read and its next call to reproduce task interleavings.

#include "tion4s/uart_core.h"

#include <cstdint>
#include <functional>

namespace fake {

class HostChannel final : public tion4s::ControlChannel, private tion4s::Clock {
 public:
  HostChannel(tion4s::UartCore& core, std::uint32_t& clock) : core_(core), clock_(clock) {}

  tion4s::UartStatus status() const override {
    const auto result = core_.status(*this);
    if (after_status) {
      auto hook = after_status;
      after_status = nullptr;
      hook();
    }
    return result;
  }

  std::uint32_t submit(const tion4s::ControlCommand& command, bool allow_legacy_02d0,
                       std::uint32_t expected_failures) override {
    ++submitted;
    return core_.submit(command, allow_legacy_02d0, expected_failures, clock_);
  }

  mutable std::function<void()> after_status;
  unsigned submitted = 0;

 private:
  std::uint32_t now_ms() const override { return clock_; }
  tion4s::UartCore& core_;
  std::uint32_t& clock_;
};

}  // namespace fake
