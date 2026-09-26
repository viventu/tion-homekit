#pragma once

#include "protocol.h"

namespace tion4s {

// A due read request; each value is the request's frame type.
enum class Request : std::uint16_t {
  None = 0,
  Heartbeat = static_cast<std::uint16_t>(FrameType::HeartbeatRequest),
  State = static_cast<std::uint16_t>(FrameType::StateRequest),
  DeviceInfo = static_cast<std::uint16_t>(FrameType::DeviceInfoRequest),
};

struct Snapshot {
  State state{};
  bool has_state = false;
  bool fresh = false;
  std::uint32_t age_ms = 0;
  std::uint32_t state_count = 0;
  std::uint32_t heartbeat_count = 0;
  DeviceInfo device_info{};
  bool has_device_info = false;
  bool heartbeat_sent = false;
  std::uint32_t heartbeat_age_ms = 0;
  std::uint32_t max_heartbeat_gap_ms = 0;
};

// Schedules the mandatory heartbeat and the periodic reads, and keeps the last
// reported state. Writes are owned by ControlTransaction.
class PollingSession {
 public:
  static constexpr std::uint32_t kHeartbeatIntervalMs = 3000;
  static constexpr std::uint32_t kStateIntervalMs = 2000;
  static constexpr std::uint32_t kStateRetryMs = 3000;
  static constexpr std::uint32_t kStateFreshnessMs = 10000;
  static constexpr std::uint32_t kDeviceInfoRetryMs = 10000;

  Request next_request(std::uint32_t now_ms) const;
  void sent(Request request, std::uint32_t now_ms);
  void accept(const Frame& frame, std::uint32_t now_ms);
  // Call at least once per freshness period. Ages are unsigned millisecond
  // differences and wrap after 49.7 days; tick() latches expiry first, so a
  // state that is never refreshed can not become fresh again after a wrap.
  void tick(std::uint32_t now_ms);
  // Age of an expired state whose true age is no longer measurable.
  static constexpr std::uint32_t kUnknownAgeMs = 0xFFFF'FFFFu;
  Snapshot snapshot(std::uint32_t now_ms) const;
  bool heartbeat_sent() const { return heartbeat_sent_; }

 private:
  State state_{};
  DeviceInfo device_info_{};
  std::uint32_t last_state_ms_ = 0;
  std::uint32_t last_heartbeat_ms_ = 0;
  std::uint32_t last_state_request_ms_ = 0;
  std::uint32_t last_device_info_request_ms_ = 0;
  std::uint32_t max_heartbeat_gap_ms_ = 0;
  std::uint32_t state_count_ = 0;
  std::uint32_t heartbeat_count_ = 0;
  bool has_state_ = false;
  bool state_expired_ = false;
  bool heartbeat_sent_ = false;
  bool state_requested_ = false;
  bool awaiting_state_ = false;
  bool device_info_requested_ = false;
  bool has_device_info_ = false;
};

}  // namespace tion4s
