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
constexpr int kMinTemperatureC = tion4s::kMinMeasuredTemperatureC;
constexpr int kMaxTemperatureC = tion4s::kMaxMeasuredTemperatureC;

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
// timer. Only an airflow change made elsewhere (power, speed or gate) ends it
// without a restore. Heat changes, other commands' failures and stale
// telemetry do not: the timer waits for a fresh state, and the restore is a
// context-bound write that the pre-read refuses if the airflow changed.
class BoostTimer {
 public:
  static constexpr std::uint32_t kDurationMs = 5 * 60 * 1000;
  // A restore that failed for another reason than changed airflow is tried
  // again on the next opportunity, up to this many attempts in all.
  static constexpr std::uint8_t kRestoreAttempts = 3;

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
    original_ = tion4s::CommandContext(snapshot.state.fan_speed,
                                       snapshot.state.gate_position, std::nullopt);
    maximum_speed_ = snapshot.state.max_fan_speed;
    ticket_ = access_.submit(
        tion4s::ControlCommand::speed_in_context(maximum_speed_, original_));
    if (ticket_ == 0) {
      return false;
    }
    phase_ = Phase::Starting;
    deadline_ms_ = now_ms + kDurationMs;
    restore_attempts_ = 0;
    return true;
  }

  // HomeKit turned the switch off.
  void stop() {
    if (phase_ == Phase::Starting || phase_ == Phase::Running) {
      phase_ = Phase::RestorePending;
    }
  }

  // Call on every loop pass. Returns whether the switch should read On.
  bool tick(const tion4s::UartStatus& status, std::uint32_t now_ms) {
    const auto& snapshot = status.device;
    if (phase_ == Phase::Starting) {
      const auto outcome = tion4s::outcome_of(status, ticket_);
      if (outcome == tion4s::CommandOutcome::Applied) {
        phase_ = Phase::Running;
      } else if (outcome != tion4s::CommandOutcome::Pending) {
        phase_ = Phase::Idle;
      }
    }
    // Without fresh telemetry nothing is decided; the timer keeps running.
    if (phase_ == Phase::Running && snapshot.fresh) {
      if (!boosted(snapshot)) {
        phase_ = Phase::Idle;  // The airflow was changed elsewhere.
      } else if (static_cast<std::int32_t>(now_ms - deadline_ms_) >= 0) {
        phase_ = Phase::RestorePending;
      }
    }
    if (phase_ == Phase::Restoring) {
      const auto result = tion4s::result_of(status, ticket_);
      if (result.outcome == tion4s::CommandOutcome::Pending) {
        return false;
      }
      // The restore's context needs the maximum step, so it never ends
      // Unchanged; a changed context means the airflow was changed.
      const bool done = result.outcome == tion4s::CommandOutcome::Applied ||
                        result.failure == tion4s::ControlFailure::ContextChanged;
      phase_ = done || restore_attempts_ >= kRestoreAttempts ? Phase::Idle
                                                             : Phase::RestorePending;
    }
    if (phase_ == Phase::RestorePending && snapshot.fresh && !status.control_pending) {
      if (!boosted(snapshot)) {
        phase_ = Phase::Idle;  // The airflow was changed elsewhere.
      } else {
        tion4s::CommandContext boosted_context = original_;
        boosted_context.fan_speed = maximum_speed_;
        ticket_ = access_.submit(tion4s::ControlCommand::speed_in_context(
            original_.fan_speed, boosted_context));
        if (ticket_ != 0) {
          ++restore_attempts_;
          phase_ = Phase::Restoring;
        }
      }
    }
    return phase_ == Phase::Starting || phase_ == Phase::Running;
  }

 private:
  enum class Phase : std::uint8_t { Idle, Starting, Running, RestorePending, Restoring };

  bool boosted(const tion4s::Snapshot& snapshot) const {
    return snapshot.state.power_on && snapshot.state.gate_position == original_.gate_position &&
           snapshot.state.fan_speed == maximum_speed_;
  }

  ControlAccess& access_;
  Phase phase_ = Phase::Idle;
  tion4s::CommandContext original_;
  std::uint8_t maximum_speed_ = 0;
  std::uint8_t restore_attempts_ = 0;
  std::uint32_t ticket_ = 0;
  std::uint32_t deadline_ms_ = 0;
};

}  // namespace tion_homekit
