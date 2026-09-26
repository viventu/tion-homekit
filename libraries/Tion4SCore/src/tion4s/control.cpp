#include "control.h"

#include "crc.h"

#include <algorithm>

namespace tion4s {
namespace {

constexpr std::uint8_t kHeater1000W = 1;
constexpr std::uint8_t kHeater1400W = 2;
constexpr std::uint32_t kReportedErrorMask = 0x7FF;
constexpr std::uint32_t kFirstWriteRequestId = 2;

// Turning the breezer or its heater off moves toward the safe state. Reported
// device errors must not block that direction; every other invariant still
// applies because the write relays the full reported state.
enum class Direction : std::uint8_t { Any, TowardSafeState };

WriteRejection check_baseline(const State& state, std::uint32_t request_id,
                              Direction direction) {
  if (request_id < kFirstWriteRequestId) {
    return WriteRejection::ReservedRequestId;
  }
  // A full-state write must not relay impossible heater/gate combinations or
  // a target outside upstream's configurable envelope.
  const bool valid = state.max_fan_speed >= 1 && state.max_fan_speed <= kMaxFanSteps &&
                     state.fan_speed >= 1 && state.fan_speed <= state.max_fan_speed &&
                     state.gate_position <= 1 &&
                     (state.gate_position == 0 || !state.heater_allowed) &&
                     state.target_temperature >= kMinTargetEnvelopeC &&
                     state.target_temperature <= kMaxTargetEnvelopeC;
  if (!valid) {
    return WriteRejection::InvalidBaseline;
  }
  if (state.magic_air_auto) {
    return WriteRejection::MagicAirAuto;
  }
  if (direction == Direction::Any && (state.errors & kReportedErrorMask) != 0) {
    return WriteRejection::DeviceErrors;
  }
  return WriteRejection::None;
}

constexpr std::uint8_t bit(bool value, unsigned position) {
  return static_cast<std::uint8_t>(static_cast<unsigned>(value) << position);
}

void encode_state_packet(const State& state, FrameType type, bool filter_reset,
                         std::uint32_t request_id,
                         std::uint8_t (&output)[kStateWriteFrameSize]) {
  const auto wire_type = static_cast<std::uint16_t>(type);
  // The heater bit is inverted on the wire: set means heat is not allowed.
  const auto flags = static_cast<std::uint8_t>(
      bit(state.power_on, 0) | bit(state.sound_on, 1) | bit(state.led_on, 2) |
      bit(!state.heater_allowed, 3) | bit(state.comm_source, 4) | bit(filter_reset, 7));
  const auto external_flags = static_cast<std::uint8_t>(
      bit(state.magic_air_connected, 0) | bit(state.magic_air_auto, 1));

  output[0] = 0x3A;
  output[1] = static_cast<std::uint8_t>(kStateWriteFrameSize);
  output[2] = 0;
  output[3] = static_cast<std::uint8_t>(wire_type);
  output[4] = static_cast<std::uint8_t>(wire_type >> 8);
  output[5] = static_cast<std::uint8_t>(request_id);
  output[6] = static_cast<std::uint8_t>(request_id >> 8);
  output[7] = static_cast<std::uint8_t>(request_id >> 16);
  output[8] = static_cast<std::uint8_t>(request_id >> 24);
  output[9] = flags;
  output[10] = external_flags;
  output[11] = state.gate_position;
  output[12] = static_cast<std::uint8_t>(state.target_temperature);
  output[13] = state.fan_speed;
  output[14] = 0;  // Upstream writes zero to its filter_time field.
  output[15] = 0;
  const auto crc = crc16_ccitt_false(output, kStateWriteFrameSize - 2);
  output[16] = static_cast<std::uint8_t>(crc >> 8);
  output[17] = static_cast<std::uint8_t>(crc);
}

// Checks of the request itself: one group, and every requested value in its
// range. The speed range is the one the device reports.
bool well_formed(const ControlCommand& command, const State& reported) {
  const auto group = command.group();
  const auto speed = command.fan_speed.value_or(1);
  const auto target = command.target_temperature.value_or(kMinHeaterTargetC);
  return group != ChangeGroup::None && group != ChangeGroup::Mixed &&
         (!command.fan_speed.has_value() ||
          (speed >= 1 && speed <= reported.max_fan_speed)) &&
         command.gate_position.value_or(0) <= 1 && target >= kMinHeaterTargetC &&
         target <= kMaxHeaterTargetC;
}

Direction direction_of(const ControlCommand& command) {
  const bool power_off_only = command.power_on == false && !command.fan_speed.has_value();
  const bool heat_off_only =
      command.heater_allowed == false && !command.target_temperature.has_value();
  return power_off_only || heat_off_only ? Direction::TowardSafeState : Direction::Any;
}

State apply(const State& reported, const ControlCommand& command) {
  State next = reported;
  next.power_on = command.power_on.value_or(next.power_on);
  next.fan_speed = command.fan_speed.value_or(next.fan_speed);
  next.gate_position = command.gate_position.value_or(next.gate_position);
  next.sound_on = command.sound_on.value_or(next.sound_on);
  next.led_on = command.led_on.value_or(next.led_on);
  next.heater_allowed = command.heater_allowed.value_or(next.heater_allowed);
  next.target_temperature = command.target_temperature.value_or(next.target_temperature);
  return next;
}

bool heating_is_safe(const State& reported, const State& next) {
  const bool supported_heater =
      reported.heater_present == kHeater1000W || reported.heater_present == kHeater1400W;
  return !next.heater_allowed ||
         (reported.power_on && reported.gate_position == 0 && supported_heater &&
          next.target_temperature >= kMinHeaterTargetC &&
          next.target_temperature <= kMaxHeaterTargetC);
}

}  // namespace

bool is_4s_normal_mode(const DeviceInfo& info) {
  return info.device_type == kDeviceType4S && info.work_mode == kWorkModeNormal;
}

bool firmware_allows_control(std::uint16_t version, bool allow_legacy_02d0) {
  if (version == kFirmware02D0) {
    return allow_legacy_02d0;
  }
  return version >= kFirmwareMinimumUpstream && version != kFirmwareBlocked03CD;
}

std::uint8_t speed_to_percent(std::uint8_t speed, std::uint8_t maximum) {
  if (maximum == 0 || maximum > kMaxFanSteps || speed == 0 || speed > maximum) {
    return 0;
  }
  return static_cast<std::uint8_t>((100u * speed + maximum / 2u) / maximum);
}

std::uint8_t percent_to_speed(std::uint8_t percent, std::uint8_t maximum) {
  if (percent == 0 || maximum == 0 || maximum > kMaxFanSteps || percent > 100) {
    return 0;
  }
  // At most `maximum` because percent is at most 100; a positive percent never
  // rounds down to off.
  return std::max<std::uint8_t>(
      static_cast<std::uint8_t>((percent * maximum + 50u) / 100u), 1);
}

ControlCommand ControlCommand::power(bool on) {
  ControlCommand command;
  command.power_on = on;
  return command;
}

ControlCommand ControlCommand::speed_step(std::uint8_t step) {
  ControlCommand command;
  command.fan_speed = step;
  return command;
}

ControlCommand ControlCommand::power_and_speed(bool on, std::uint8_t step) {
  ControlCommand command;
  command.power_on = on;
  command.fan_speed = step;
  return command;
}

ControlCommand ControlCommand::recirculation(bool on) {
  ControlCommand command;
  command.gate_position = static_cast<std::uint8_t>(on ? 1 : 0);
  return command;
}

ControlCommand ControlCommand::sound(bool on) {
  ControlCommand command;
  command.sound_on = on;
  return command;
}

ControlCommand ControlCommand::led(bool on) {
  ControlCommand command;
  command.led_on = on;
  return command;
}

ControlCommand ControlCommand::heater(bool allowed) {
  ControlCommand command;
  command.heater_allowed = allowed;
  return command;
}

ControlCommand ControlCommand::target(std::int8_t celsius) {
  ControlCommand command;
  command.target_temperature = celsius;
  return command;
}

ControlCommand ControlCommand::heater_and_target(bool allowed, std::int8_t celsius) {
  ControlCommand command;
  command.heater_allowed = allowed;
  command.target_temperature = celsius;
  return command;
}

ControlCommand ControlCommand::filter_reset() {
  ControlCommand command;
  command.reset_filter = true;
  return command;
}

ControlCommand ControlCommand::speed_in_context(std::uint8_t step,
                                                const CommandContext& context) {
  ControlCommand command = speed_step(step);
  command.context = context;
  return command;
}

ChangeGroup ControlCommand::group() const {
  const bool fan = power_on.has_value() || fan_speed.has_value();
  const bool gate = gate_position.has_value();
  const bool settings = sound_on.has_value() || led_on.has_value();
  const bool heater = heater_allowed.has_value() || target_temperature.has_value();
  const int groups =
      int{fan} + int{gate} + int{settings} + int{heater} + int{reset_filter};
  if (groups == 0) {
    return ChangeGroup::None;
  }
  if (groups > 1) {
    return ChangeGroup::Mixed;
  }
  if (fan) {
    return ChangeGroup::Fan;
  }
  if (gate) {
    return ChangeGroup::Gate;
  }
  if (settings) {
    return ChangeGroup::Settings;
  }
  return heater ? ChangeGroup::Heater : ChangeGroup::FilterReset;
}

bool command_matches(const ControlCommand& command, const State& state) {
  const auto group = command.group();
  if (group == ChangeGroup::None || group == ChangeGroup::FilterReset ||
      group == ChangeGroup::Mixed) {
    return false;
  }
  const State next = apply(state, command);
  return next.power_on == state.power_on && next.fan_speed == state.fan_speed &&
         next.gate_position == state.gate_position && next.sound_on == state.sound_on &&
         next.led_on == state.led_on && next.heater_allowed == state.heater_allowed &&
         next.target_temperature == state.target_temperature;
}

bool can_merge(const ControlCommand& older, const ControlCommand& newer) {
  const auto group = older.group();
  return group == newer.group() && group != ChangeGroup::None &&
         group != ChangeGroup::Mixed && group != ChangeGroup::FilterReset &&
         !older.context.has_value() && !newer.context.has_value();
}

ControlCommand merge(const ControlCommand& older, const ControlCommand& newer) {
  ControlCommand result = older;
  if (newer.power_on) {
    result.power_on = newer.power_on;
  }
  if (newer.fan_speed) {
    result.fan_speed = newer.fan_speed;
  }
  if (newer.gate_position) {
    result.gate_position = newer.gate_position;
  }
  if (newer.sound_on) {
    result.sound_on = newer.sound_on;
  }
  if (newer.led_on) {
    result.led_on = newer.led_on;
  }
  if (newer.heater_allowed) {
    result.heater_allowed = newer.heater_allowed;
  }
  if (newer.target_temperature) {
    result.target_temperature = newer.target_temperature;
  }
  return result;
}

WriteRejection encode_command(const State& reported, const ControlCommand& command,
                              std::uint32_t request_id,
                              std::uint8_t (&output)[kStateWriteFrameSize]) {
  if (!well_formed(command, reported)) {
    return WriteRejection::InvalidRequest;
  }
  const auto baseline = check_baseline(reported, request_id, direction_of(command));
  if (baseline != WriteRejection::None) {
    return baseline;
  }
  const auto group = command.group();
  if (group == ChangeGroup::FilterReset) {
    if (reported.power_on || reported.heater_allowed) {
      return WriteRejection::FilterResetNeedsStop;
    }
    encode_state_packet(reported, FrameType::StateSet, true, request_id, output);
    return WriteRejection::None;
  }
  if (command_matches(command, reported)) {
    return WriteRejection::NoChange;
  }
  const State next = apply(reported, command);
  // Only the heater group can allow heat; recirculation cannot start while
  // heat is allowed. Other groups relay the reported heat setting unchanged.
  const bool unsafe =
      (group == ChangeGroup::Heater && !heating_is_safe(reported, next)) ||
      (group == ChangeGroup::Gate && next.gate_position == 1 && reported.heater_allowed);
  if (unsafe) {
    return WriteRejection::UnsafeCombination;
  }
  encode_state_packet(
      next, group == ChangeGroup::Settings ? FrameType::StateSave : FrameType::StateSet,
      false, request_id, output);
  return WriteRejection::None;
}

}  // namespace tion4s
