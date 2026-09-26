#include "describe.h"

namespace tion4s {

const char* describe(ControlPhase phase) {
  switch (phase) {
    case ControlPhase::Idle:
      return "idle";
    case ControlPhase::Queued:
      return "queued";
    case ControlPhase::Reading:
      return "reading";
    case ControlPhase::Writing:
      return "writing";
    case ControlPhase::Verifying:
      return "verifying";
    case ControlPhase::Applied:
      return "applied";
    case ControlPhase::Unchanged:
      return "unchanged, no write";
    case ControlPhase::Failed:
      return "failed";
  }
  return "unknown";
}

const char* describe(ControlFailure failure) {
  switch (failure) {
    case ControlFailure::None:
      return "none";
    case ControlFailure::UnsupportedDevice:
      return "unsupported device";
    case ControlFailure::StaleState:
      return "stale state";
    case ControlFailure::ContextChanged:
      return "mode changed";
    case ControlFailure::Refused:
      return "refused";
    case ControlFailure::ReadTimeout:
      return "read timeout";
    case ControlFailure::VerifyTimeout:
      return "verification timeout";
    case ControlFailure::NotApplied:
      return "reported state did not change";
    case ControlFailure::Transport:
      return "UART transport";
    case ControlFailure::Expired:
      return "expired before write";
  }
  return "unknown";
}

const char* describe(WriteRejection rejection) {
  switch (rejection) {
    case WriteRejection::None:
      return "none";
    case WriteRejection::InvalidRequest:
      return "invalid request";
    case WriteRejection::ReservedRequestId:
      return "reserved request ID";
    case WriteRejection::InvalidBaseline:
      return "reported state cannot be relayed";
    case WriteRejection::MagicAirAuto:
      return "MagicAir automation active";
    case WriteRejection::DeviceErrors:
      return "breezer reports errors";
    case WriteRejection::NoChange:
      return "nothing to change";
    case WriteRejection::UnsafeCombination:
      return "unsafe heat or recirculation";
    case WriteRejection::FilterResetNeedsStop:
      return "filter reset needs breezer and heat off";
  }
  return "unknown";
}

const char* describe(const ControlCommand& command) {
  const auto group = command.group();
  if (group == ChangeGroup::Fan) {
    if (command.power_on && command.fan_speed) {
      return "power and speed";
    }
    return command.power_on ? "power" : "speed";
  }
  if (group == ChangeGroup::Settings) {
    if (command.sound_on && command.led_on) {
      return "sound and LED";
    }
    return command.sound_on ? "sound" : "LED";
  }
  if (group == ChangeGroup::Heater) {
    if (command.heater_allowed && command.target_temperature) {
      return "heater and target";
    }
    return command.heater_allowed ? "heater permission" : "target temperature";
  }
  if (group == ChangeGroup::Gate) {
    return "recirculation";
  }
  if (group == ChangeGroup::FilterReset) {
    return "filter reset";
  }
  return group == ChangeGroup::Mixed ? "mixed groups" : "none";
}

}  // namespace tion4s
