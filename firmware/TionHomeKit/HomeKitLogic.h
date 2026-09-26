#pragma once

// HomeKit behavior without HomeSpan types, so that it runs in host tests.

#include "ControlAccess.h"

#include <tion4s/control.h>
#include <tion4s/uart_core.h>

#include <cmath>
#include <cstdint>
#include <optional>

namespace tion_homekit {

// HAP's default CurrentTemperature range is 0-100 C. Intake air is often below
// zero, so both temperature characteristics use the range from HomeSpan's own
// outdoor-sensor example instead of reporting a fault all winter.
constexpr int kMinTemperatureC = -50;
constexpr int kMaxTemperatureC = 100;

inline bool temperature_in_range(std::int8_t celsius) {
  return celsius >= kMinTemperatureC && celsius <= kMaxTemperatureC;
}

// A state the fan tile can show and control.
inline bool fan_snapshot_valid(const tion4s::Snapshot& snapshot) {
  return snapshot.has_device_info && tion4s::is_4s_normal_mode(snapshot.device_info) &&
         snapshot.fresh && snapshot.state.max_fan_speed >= 1 &&
         snapshot.state.max_fan_speed <= tion4s::kMaxFanSteps &&
         snapshot.state.fan_speed <= snapshot.state.max_fan_speed;
}

// Fan tile write: Active and/or RotationSpeed in percent. At least one of
// them was updated. Empty result: refuse the write.
inline std::optional<tion4s::ControlCommand> fan_command(
    bool active_updated, bool active, bool speed_updated, float percent,
    const tion4s::Snapshot& snapshot) {
  if (!fan_snapshot_valid(snapshot) || (!active_updated && !speed_updated)) {
    return std::nullopt;
  }
  if (!speed_updated) {
    return tion4s::ControlCommand::power(active);
  }
  if (!std::isfinite(percent) || percent < 0 || percent > 100) {
    return std::nullopt;
  }
  if (percent == 0) {
    return tion4s::ControlCommand::power(false);
  }
  const auto step = tion4s::percent_to_speed(
      static_cast<std::uint8_t>(std::lround(percent)), snapshot.state.max_fan_speed);
  // A positive speed on a stopped breezer also starts it.
  if (active_updated || !snapshot.state.power_on) {
    return tion4s::ControlCommand::power_and_speed(active_updated ? active : true, step);
  }
  return tion4s::ControlCommand::speed_step(step);
}

// HeaterCooler write: Active and/or the heating threshold. `celsius` is the
// new threshold when it was updated, otherwise the current one.
inline std::optional<tion4s::ControlCommand> heater_command(bool active_updated,
                                                            bool active,
                                                            bool target_updated,
                                                            float celsius) {
  if ((!active_updated && !target_updated) || !std::isfinite(celsius) ||
      celsius < tion4s::kMinHeaterTargetC || celsius > tion4s::kMaxHeaterTargetC) {
    return std::nullopt;
  }
  const auto rounded = static_cast<std::int8_t>(std::lround(celsius));
  if (active_updated && target_updated) {
    return tion4s::ControlCommand::heater_and_target(active, rounded);
  }
  if (active_updated) {
    return tion4s::ControlCommand::heater(active);
  }
  return tion4s::ControlCommand::target(rounded);
}

// A plain on/off switch: the command that sets it and the reported value.
struct Auxiliary {
  tion4s::ControlCommand (*command)(bool on);
  bool (*reported)(const tion4s::State& state);
};

inline constexpr Auxiliary kRecirculation{
    tion4s::ControlCommand::recirculation,
    [](const tion4s::State& state) { return state.gate_position == 1; }};
inline constexpr Auxiliary kSound{
    tion4s::ControlCommand::sound,
    [](const tion4s::State& state) { return state.sound_on; }};
inline constexpr Auxiliary kLed{tion4s::ControlCommand::led,
                                [](const tion4s::State& state) { return state.led_on; }};

// UART does not expose native turbo. This is a five-minute local max-speed
// timer; restoration happens only if the breezer still has the exact mode
// that the timer produced, and it is attempted once.
class BoostTimer {
 public:
  static constexpr std::uint32_t kDurationMs = 5 * 60 * 1000;
  static constexpr std::uint32_t kRestoreWindowMs = 10 * 1000;

  explicit BoostTimer(ControlAccess& access) : access_(access) {}

  // HomeKit turned the switch on. True when the speed-up was accepted.
  bool start(std::uint32_t now_ms) {
    if (phase_ != Phase::Idle) {
      return false;
    }
    const auto status = access_.status();
    const auto& snapshot = status.device;
    if (!access_.available(status) || !snapshot.state.power_on ||
        snapshot.state.fan_speed == 0 ||
        snapshot.state.fan_speed >= snapshot.state.max_fan_speed) {
      return false;
    }
    original_.fan_speed = snapshot.state.fan_speed;
    original_.gate_position = snapshot.state.gate_position;
    original_.heater_allowed = snapshot.state.heater_allowed;
    maximum_speed_ = snapshot.state.max_fan_speed;
    ticket_ = access_.submit(
        tion4s::ControlCommand::speed_in_context(maximum_speed_, original_));
    if (ticket_ == 0) {
      return false;
    }
    failures_at_start_ = status.control_failed;
    phase_ = Phase::Starting;
    deadline_ms_ = now_ms + kDurationMs;
    return true;
  }

  // HomeKit turned the switch off.
  void stop(std::uint32_t now_ms) {
    if (phase_ != Phase::Idle) {
      phase_ = Phase::RestorePending;
      restore_requested_ms_ = now_ms;
    }
  }

  // Call on every loop pass. Returns whether the switch should read On.
  bool tick(const tion4s::UartStatus& status, std::uint32_t now_ms) {
    // Any failed command cancels this timer, including its automatic restore.
    if (phase_ != Phase::Idle && status.control_failed != failures_at_start_) {
      phase_ = Phase::Idle;
    }
    if (phase_ == Phase::Starting) {
      const auto outcome = tion4s::outcome_of(status, ticket_);
      if (outcome == tion4s::CommandOutcome::Applied) {
        phase_ = Phase::Running;
      } else if (outcome != tion4s::CommandOutcome::Pending) {
        phase_ = Phase::Idle;
      }
    }
    if (phase_ == Phase::Running) {
      if (!boosted(status.device)) {
        phase_ = Phase::Idle;  // Native control changed the mode.
      } else if (static_cast<std::int32_t>(now_ms - deadline_ms_) >= 0) {
        phase_ = Phase::RestorePending;
        restore_requested_ms_ = now_ms;
      }
    }
    if (phase_ == Phase::RestorePending && !status.control_pending) {
      if (now_ms - restore_requested_ms_ <= kRestoreWindowMs && boosted(status.device)) {
        tion4s::CommandContext boosted_context = original_;
        boosted_context.fan_speed = maximum_speed_;
        access_.submit(tion4s::ControlCommand::speed_in_context(original_.fan_speed,
                                                                boosted_context));
      }
      phase_ = Phase::Idle;
    }
    return phase_ == Phase::Starting || phase_ == Phase::Running;
  }

 private:
  enum class Phase : std::uint8_t { Idle, Starting, Running, RestorePending };

  bool boosted(const tion4s::Snapshot& snapshot) const {
    return snapshot.fresh && snapshot.state.power_on &&
           snapshot.state.gate_position == original_.gate_position &&
           snapshot.state.heater_allowed == original_.heater_allowed &&
           snapshot.state.fan_speed == maximum_speed_;
  }

  ControlAccess& access_;
  Phase phase_ = Phase::Idle;
  tion4s::CommandContext original_{};
  std::uint8_t maximum_speed_ = 0;
  std::uint32_t ticket_ = 0;
  std::uint32_t failures_at_start_ = 0;
  std::uint32_t deadline_ms_ = 0;
  std::uint32_t restore_requested_ms_ = 0;
};

}  // namespace tion_homekit
