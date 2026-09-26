#include "control_transaction.h"

#include <cstring>

namespace tion4s {

bool ControlTransaction::busy() const {
  return phase_ == ControlPhase::Queued || phase_ == ControlPhase::Reading ||
         phase_ == ControlPhase::Writing || phase_ == ControlPhase::Verifying;
}

bool ControlTransaction::write_sent() const {
  return phase_ == ControlPhase::Writing || phase_ == ControlPhase::Verifying;
}

void ControlTransaction::fail(ControlFailure reason, WriteRejection refusal) {
  phase_ = ControlPhase::Failed;
  failure_ = reason;
  refusal_ = refusal;
}

bool ControlTransaction::matches(const State& state) const {
  if (command_.reset_filter) {
    // This is the remaining-life counter. A small decrease may happen
    // naturally while the fan runs; only a substantial reset is evidence.
    return !state.filter_warning && state.filter_seconds > baseline_.filter_seconds &&
           state.filter_seconds - baseline_.filter_seconds > kFilterResetEvidenceSeconds;
  }
  return command_matches(command_, state);
}

bool ControlTransaction::context_matches(const State& state) const {
  if (!command_.context) {
    return true;
  }
  const auto& context = *command_.context;
  return state.power_on && state.fan_speed == context.fan_speed &&
         state.gate_position == context.gate_position &&
         state.heater_allowed == context.heater_allowed;
}

ControlOutput ControlTransaction::read_request(ControlStep step) const {
  ControlOutput result;
  std::uint8_t request[kRequestFrameSize];
  encode_request(FrameType::StateRequest, request);
  std::memcpy(result.bytes, request, sizeof(request));
  result.size = sizeof(request);
  result.step = step;
  return result;
}

bool ControlTransaction::submit(const ControlCommand& command, const Snapshot& snapshot,
                                bool allow_legacy_02d0, std::uint32_t queued_ms,
                                std::uint32_t now_ms) {
  if (busy()) {
    return false;
  }
  command_ = command;
  if (now_ms - queued_ms > kIntentMaxAgeMs) {
    fail(ControlFailure::Expired);
    return false;
  }
  if (!snapshot.has_device_info || !is_4s_normal_mode(snapshot.device_info) ||
      !firmware_allows_control(snapshot.device_info.firmware_version,
                               allow_legacy_02d0)) {
    fail(ControlFailure::UnsupportedDevice);
    return false;
  }
  const auto group = command.group();
  if (group == ChangeGroup::None || group == ChangeGroup::Mixed) {
    fail(ControlFailure::Refused, WriteRejection::InvalidRequest);
    return false;
  }
  if (!snapshot.has_state || !snapshot.fresh || snapshot.age_ms > kMaxSnapshotAgeMs) {
    fail(ControlFailure::StaleState);
    return false;
  }
  if (!context_matches(snapshot.state)) {
    fail(ControlFailure::ContextChanged);
    return false;
  }
  legacy_02d0_ = snapshot.device_info.firmware_version == kFirmware02D0;
  queued_ms_ = queued_ms;
  baseline_ready_ = false;
  verify_reads_ = 0;
  failure_ = ControlFailure::None;
  refusal_ = WriteRejection::None;
  phase_ = ControlPhase::Queued;
  return true;
}

ControlOutput ControlTransaction::next(std::uint32_t now_ms) {
  if (!busy()) {
    return {};
  }
  if (now_ms - queued_ms_ > kIntentMaxAgeMs) {
    fail(write_sent() ? ControlFailure::VerifyTimeout : ControlFailure::Expired);
    return {};
  }
  if (phase_ == ControlPhase::Queued) {
    return read_request(ControlStep::Read);
  }
  if (phase_ == ControlPhase::Reading) {
    return write_from_baseline(now_ms);
  }
  if (phase_ == ControlPhase::Writing) {
    return now_ms - sent_ms_ >= kWriteSettleMs ? read_request(ControlStep::Verify)
                                               : ControlOutput{};
  }
  // Verifying.
  if (now_ms - sent_ms_ <= kVerifyIntervalMs) {
    return {};
  }
  if (verify_reads_ < kMaxVerifyReads &&
      now_ms - queued_ms_ < kIntentMaxAgeMs - kVerifyIntervalMs) {
    return read_request(ControlStep::Verify);
  }
  fail(ControlFailure::VerifyTimeout);
  return {};
}

ControlOutput ControlTransaction::write_from_baseline(std::uint32_t now_ms) {
  if (!baseline_ready_) {
    if (now_ms - sent_ms_ > kReadTimeoutMs) {
      fail(ControlFailure::ReadTimeout);
    }
    return {};
  }
  if (now_ms - baseline_ms_ > kBaselineMaxAgeMs) {
    fail(ControlFailure::StaleState);
    return {};
  }
  if (!context_matches(baseline_)) {
    fail(ControlFailure::ContextChanged);
    return {};
  }
  // A filter reset never matches its own baseline.
  if (matches(baseline_)) {
    // Another controller already set the requested values; no write.
    phase_ = ControlPhase::Unchanged;
    return {};
  }
  ControlOutput write;
  const auto refusal = encode_command(baseline_, command_, request_id_ + 1, write.bytes);
  if (refusal != WriteRejection::None) {
    fail(ControlFailure::Refused, refusal);
    return {};
  }
  write.step = ControlStep::Write;
  write.size = kStateWriteFrameSize;
  return write;
}

void ControlTransaction::sent(ControlStep step, std::uint32_t now_ms) {
  if (step == ControlStep::Read && phase_ == ControlPhase::Queued) {
    phase_ = ControlPhase::Reading;
    sent_ms_ = now_ms;
  } else if (step == ControlStep::Write && phase_ == ControlPhase::Reading &&
             baseline_ready_) {
    ++request_id_;
    phase_ = ControlPhase::Writing;
    sent_ms_ = now_ms;
    baseline_ready_ = false;
  } else if (step == ControlStep::Verify &&
             (phase_ == ControlPhase::Writing || phase_ == ControlPhase::Verifying)) {
    phase_ = ControlPhase::Verifying;
    sent_ms_ = now_ms;
    ++verify_reads_;
  }
}

void ControlTransaction::observe(const Frame& frame, std::uint32_t now_ms) {
  State state;
  if (!decode_state(frame, state)) {
    return;
  }
  if (phase_ == ControlPhase::Reading && !baseline_ready_ &&
      (legacy_02d0_ || state.request_id == 1)) {
    baseline_ = state;
    baseline_ms_ = now_ms;
    baseline_ready_ = true;
  } else if (phase_ == ControlPhase::Verifying &&
             (legacy_02d0_ || state.request_id == 1 || state.request_id == request_id_)) {
    if (matches(state)) {
      phase_ = ControlPhase::Applied;
    } else if (verify_reads_ >= kMaxVerifyReads) {
      fail(ControlFailure::NotApplied);
    }
  }
}

void ControlTransaction::transport_failed() {
  if (busy()) {
    fail(ControlFailure::Transport);
  }
}

}  // namespace tion4s
