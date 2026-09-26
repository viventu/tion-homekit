#pragma once

#include <cstdint>

namespace tion4s {

// Counts software recovery requests and clears the budget after sustained
// healthy operation. The platform preserves the count in RTC memory, counts
// watchdog resets on boot, and must not start the failing driver/task again
// once the budget is exhausted. GiveUp alone cannot disarm a task watchdog.
class StallGuard {
 public:
  // Twice the heartbeat interval; well below upstream's 8-10 s warning.
  static constexpr std::uint32_t kHeartbeatStallMs = 6000;
  static constexpr std::uint32_t kHealthyResetMs = 60000;
  static constexpr std::uint32_t kMaxConsecutiveRestarts = 3;

  enum class Action : std::uint8_t { None, Restart, GiveUp };

  // `heartbeat_age_ms` is the time since the last completed heartbeat TX, or
  // since UART start before the first one.
  Action evaluate(std::uint32_t heartbeat_age_ms, std::uint32_t now_ms,
                  std::uint32_t& consecutive_restarts) {
    if (heartbeat_age_ms < kHeartbeatStallMs) {
      if (!healthy_) {
        healthy_ = true;
        healthy_since_ms_ = now_ms;
      } else if (consecutive_restarts != 0 &&
                 now_ms - healthy_since_ms_ >= kHealthyResetMs) {
        consecutive_restarts = 0;
        gave_up_ = false;
      }
      return Action::None;
    }
    healthy_ = false;
    if (consecutive_restarts < kMaxConsecutiveRestarts) {
      ++consecutive_restarts;
      return Action::Restart;
    }
    if (!gave_up_) {
      gave_up_ = true;
      return Action::GiveUp;
    }
    return Action::None;
  }

  bool gave_up() const { return gave_up_; }

 private:
  std::uint32_t healthy_since_ms_ = 0;
  bool healthy_ = false;
  bool gave_up_ = false;
};

}  // namespace tion4s
