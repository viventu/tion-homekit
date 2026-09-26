#pragma once

#include <cstddef>
#include <cstdint>

namespace tion4s {

// Frame: magic, little-endian size and type, payload, big-endian CRC-16.
constexpr std::size_t kFrameOverhead = 7;
constexpr std::size_t kMaxFrameSize = 42;
constexpr std::size_t kMaxPayloadSize = kMaxFrameSize - kFrameOverhead;
constexpr std::size_t kStatePayloadSize = 35;
constexpr std::size_t kDeviceInfoPayloadSize = 25;
// Requests carry no payload.
constexpr std::size_t kRequestFrameSize = kFrameOverhead;
static_assert(kStatePayloadSize <= kMaxPayloadSize, "state payload fits a frame");
static_assert(kDeviceInfoPayloadSize <= kMaxPayloadSize, "info payload fits a frame");

enum class FrameType : std::uint16_t {
  StateSet = 0x3230,
  StateResponse = 0x3231,
  StateRequest = 0x3232,
  StateSave = 0x3234,
  DeviceInfoResponse = 0x3331,
  DeviceInfoRequest = 0x3332,
  HeartbeatResponse = 0x3931,
  HeartbeatRequest = 0x3932,
};

struct Frame {
  std::uint16_t type = 0;
  std::uint8_t payload[kMaxPayloadSize]{};
  std::size_t payload_size = 0;
};

// Frames one of the *Request types.
void encode_request(FrameType type, std::uint8_t (&output)[kRequestFrameSize]);

class FrameParser {
 public:
  // Appends one byte and returns the first complete frame, if any. Call next()
  // until it returns false to drain further frames already in the buffer.
  // Invalid candidates are discarded one byte at a time, so after any call
  // the buffer holds less than one maximum frame and push() cannot overflow.
  bool push(std::uint8_t byte, Frame& frame);
  // Returns the next complete buffered frame without consuming new input.
  bool next(Frame& frame);
  // Call after the line has been idle: an incomplete candidate can no longer
  // finish, so it is dropped and any complete frame behind it is recovered.
  // Returns buffered frames one at a time and leaves the buffer empty.
  bool flush(Frame& frame);
  void reset() { size_ = 0; }
  bool empty() const { return size_ == 0; }
  // Number of discarded candidates that started with the frame magic.
  std::uint32_t rejected_frames() const { return rejected_frames_; }

 private:
  void discard_prefix(std::size_t count);

  std::uint8_t buffer_[kMaxFrameSize]{};
  std::size_t size_ = 0;
  std::uint32_t rejected_frames_ = 0;
};

struct State {
  std::uint32_t request_id = 0;
  bool power_on = false;
  bool sound_on = false;
  bool led_on = false;
  bool heater_state_bit = false;
  bool heater_allowed = false;
  bool comm_source = false;
  bool filter_warning = false;
  std::uint8_t heater_present = 0;
  bool magic_air_connected = false;
  bool magic_air_auto = false;
  std::uint8_t gate_position = 0;
  std::int8_t target_temperature = 0;
  std::uint8_t fan_speed = 0;
  std::int8_t outdoor_temperature = 0;
  std::int8_t current_temperature = 0;
  std::int8_t control_board_temperature = 0;
  std::int8_t power_board_temperature = 0;
  std::uint32_t work_seconds = 0;
  std::uint32_t fan_seconds = 0;
  std::uint32_t filter_seconds = 0;
  std::uint32_t airflow_counter = 0;
  std::uint32_t errors = 0;
  std::uint8_t max_fan_speed = 0;
  std::uint8_t heater_percent = 0;
};

// Decodes the 4S raw state. Sensor names follow upstream until their physical
// locations are verified on the user's devices.
bool decode_state(const Frame& frame, State& state);

struct DeviceInfo {
  std::uint8_t work_mode = 0;
  std::uint32_t device_type = 0;
  std::uint16_t firmware_version = 0;
  std::uint16_t hardware_version = 0;
};

// The 25-byte response is read-only device metadata; reserved bytes are ignored.
bool decode_device_info(const Frame& frame, DeviceInfo& info);

}  // namespace tion4s
