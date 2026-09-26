#pragma once

#include "protocol.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace tion4s {

constexpr std::uint32_t kDeviceType4S = 0x8003;
constexpr std::uint8_t kWorkModeNormal = 1;
// Upstream does not confirm writes on 02D0; it needs the explicit HomeKit opt-in.
constexpr std::uint16_t kFirmware02D0 = 0x02D0;
constexpr std::uint16_t kFirmwareMinimumUpstream = 0x02D2;
// Upstream reports broken control on 03CD.
constexpr std::uint16_t kFirmwareBlocked03CD = 0x03CD;

// Limits shared by the protocol and HomeKit layers.
constexpr std::uint8_t kMaxFanSteps = 6;
constexpr std::int8_t kMinHeaterTargetC = 1;
constexpr std::int8_t kMaxHeaterTargetC = 25;
// Upstream's configurable target envelope: a reported target outside it is
// never relayed in a full-state write.
constexpr std::int8_t kMinTargetEnvelopeC = -25;
constexpr std::int8_t kMaxTargetEnvelopeC = 30;
constexpr std::size_t kStateWriteFrameSize = 18;

// A recognized Tion 4S in its normal work mode.
bool is_4s_normal_mode(const DeviceInfo& info);

// Firmware policy for any state write. The legacy flag is the explicit opt-in.
bool firmware_allows_control(std::uint16_t version, bool allow_legacy_02d0);

// The device reports the maximum supported step. Zero means off in HomeKit,
// but the Tion write packet always carries a nonzero stored fan step.
std::uint8_t speed_to_percent(std::uint8_t speed, std::uint8_t maximum);
std::uint8_t percent_to_speed(std::uint8_t percent, std::uint8_t maximum);

// Fields that one state frame writes. A command changes one group only.
enum class ChangeGroup : std::uint8_t { None, Fan, Gate, Settings, Heater, FilterReset, Mixed };

// Reported values a context-bound command requires, besides power on.
struct CommandContext {
  std::uint8_t fan_speed = 0;
  std::uint8_t gate_position = 0;
  bool heater_allowed = false;
};

// A requested set of changes: every engaged field is one change, named like
// the State field it sets. Build commands with the factories.
struct ControlCommand {
  std::optional<bool> power_on;                   // Fan
  std::optional<std::uint8_t> fan_speed;          // Fan
  std::optional<std::uint8_t> gate_position;      // Gate: 0 outdoor, 1 recirculation
  std::optional<bool> sound_on;                   // Settings
  std::optional<bool> led_on;                     // Settings
  std::optional<bool> heater_allowed;             // Heater
  std::optional<std::int8_t> target_temperature;  // Heater
  bool reset_filter = false;                      // FilterReset
  // Applied only while the breezer is on and still reports these values.
  std::optional<CommandContext> context;

  static ControlCommand power(bool on);
  static ControlCommand speed_step(std::uint8_t step);
  static ControlCommand power_and_speed(bool on, std::uint8_t step);
  static ControlCommand recirculation(bool on);
  static ControlCommand sound(bool on);
  static ControlCommand led(bool on);
  static ControlCommand heater(bool allowed);
  static ControlCommand target(std::int8_t celsius);
  static ControlCommand heater_and_target(bool allowed, std::int8_t celsius);
  static ControlCommand filter_reset();
  static ControlCommand speed_in_context(std::uint8_t step, const CommandContext& context);

  ChangeGroup group() const;
};

// True when every requested field already has its target value. Filter reset
// is an action and never matches; an invalid command never matches.
bool command_matches(const ControlCommand& command, const State& state);

// A waiting command may absorb a newer one that changes the same group: the
// newer fields win, older fields that the newer one does not set remain.
// Filter reset and context-bound commands never merge.
bool can_merge(const ControlCommand& older, const ControlCommand& newer);
ControlCommand merge(const ControlCommand& older, const ControlCommand& newer);

// Why a full-state write was not encoded.
enum class WriteRejection : std::uint8_t {
  None,
  InvalidRequest,        // No or mixed groups, or a value outside its range.
  ReservedRequestId,     // Request IDs 0 and 1 belong to reads.
  InvalidBaseline,       // The reported state cannot be relayed as is.
  MagicAirAuto,          // MagicAir automation owns the breezer.
  DeviceErrors,          // The breezer reports errors; only safe changes pass.
  NoChange,              // Every requested value is already reported.
  UnsafeCombination,     // Heat without intake air, a heater or power, or
                         // recirculation while heat is allowed.
  FilterResetNeedsStop,  // Filter reset needs the breezer and heat off.
};

// Encodes one upstream state write from a fresh reported state; returns None
// on success. Unrequested fields keep their reported values, reset bits stay
// clear except for filter reset. Sound and LED use the persistent frame type.
// Reported device errors block every change except power-off and heat-off.
WriteRejection encode_command(const State& reported, const ControlCommand& command,
                              std::uint32_t request_id,
                              std::uint8_t (&output)[kStateWriteFrameSize]);

}  // namespace tion4s
