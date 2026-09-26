#include "session.h"

namespace tion4s {

Request PollingSession::next_request(std::uint32_t now_ms) const {
  if (!heartbeat_sent_ || now_ms - last_heartbeat_ms_ >= kHeartbeatIntervalMs) {
    return Request::Heartbeat;
  }
  const auto state_wait_ms = awaiting_state_ ? kStateRetryMs : kStateIntervalMs;
  if (!state_requested_ || now_ms - last_state_request_ms_ >= state_wait_ms) {
    return Request::State;
  }
  if (!has_device_info_ &&
      (!device_info_requested_ ||
       now_ms - last_device_info_request_ms_ >= kDeviceInfoRetryMs)) {
    return Request::DeviceInfo;
  }
  return Request::None;
}

void PollingSession::sent(Request request, std::uint32_t now_ms) {
  if (request == Request::Heartbeat) {
    if (heartbeat_sent_) {
      const auto gap = now_ms - last_heartbeat_ms_;
      if (gap > max_heartbeat_gap_ms_) {
        max_heartbeat_gap_ms_ = gap;
      }
    }
    last_heartbeat_ms_ = now_ms;
    heartbeat_sent_ = true;
  } else if (request == Request::State) {
    last_state_request_ms_ = now_ms;
    state_requested_ = true;
    awaiting_state_ = true;
  } else if (request == Request::DeviceInfo) {
    last_device_info_request_ms_ = now_ms;
    device_info_requested_ = true;
  }
}

void PollingSession::accept(const Frame& frame, std::uint32_t now_ms) {
  DeviceInfo info{};
  if (decode_device_info(frame, info)) {
    device_info_ = info;
    has_device_info_ = true;
    return;
  }
  if (frame.type == static_cast<std::uint16_t>(FrameType::HeartbeatResponse)) {
    if (frame.payload_size == 1) {
      ++heartbeat_count_;
    }
    return;
  }

  State decoded{};
  if (decode_state(frame, decoded)) {
    state_ = decoded;
    last_state_ms_ = now_ms;
    has_state_ = true;
    state_expired_ = false;
    awaiting_state_ = false;
    ++state_count_;
  }
}

void PollingSession::tick(std::uint32_t now_ms) {
  if (has_state_ && !state_expired_ && now_ms - last_state_ms_ > kStateFreshnessMs) {
    state_expired_ = true;
  }
}

Snapshot PollingSession::snapshot(std::uint32_t now_ms) const {
  Snapshot result{};
  result.has_state = has_state_;
  result.state_count = state_count_;
  result.heartbeat_count = heartbeat_count_;
  result.device_info = device_info_;
  result.has_device_info = has_device_info_;
  result.max_heartbeat_gap_ms = max_heartbeat_gap_ms_;
  result.heartbeat_sent = heartbeat_sent_;
  if (heartbeat_sent_) {
    result.heartbeat_age_ms = now_ms - last_heartbeat_ms_;
  }
  if (has_state_) {
    result.state = state_;
    result.age_ms = now_ms - last_state_ms_;
    if (state_expired_ && result.age_ms <= kStateFreshnessMs) {
      result.age_ms = kUnknownAgeMs;  // The difference wrapped.
    }
    result.fresh = !state_expired_ && result.age_ms <= kStateFreshnessMs;
  }
  return result;
}

}  // namespace tion4s
