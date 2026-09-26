#include "protocol.h"
#include "crc.h"

#include <cstring>

namespace tion4s {
namespace {

constexpr std::uint8_t kMagic = 0x3A;
constexpr std::size_t kMinFrameSize = kFrameOverhead;

std::uint16_t read_le16(const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
}

std::uint32_t read_le32(const std::uint8_t* bytes) {
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::int8_t read_signed(std::uint8_t byte) {
  const auto value = byte < 0x80 ? static_cast<std::int16_t>(byte)
                                 : static_cast<std::int16_t>(byte) - 256;
  return static_cast<std::int8_t>(value);
}

}  // namespace

void encode_request(FrameType type, std::uint8_t (&output)[kRequestFrameSize]) {
  const auto wire_type = static_cast<std::uint16_t>(type);
  output[0] = kMagic;
  output[1] = static_cast<std::uint8_t>(kRequestFrameSize);
  output[2] = 0;
  output[3] = static_cast<std::uint8_t>(wire_type);
  output[4] = static_cast<std::uint8_t>(wire_type >> 8);
  const auto crc = crc16_ccitt_false(output, 5);
  output[5] = static_cast<std::uint8_t>(crc >> 8);
  output[6] = static_cast<std::uint8_t>(crc);
}

void FrameParser::discard_prefix(std::size_t count) {
  size_ -= count;
  std::memmove(buffer_, buffer_ + count, size_);
}

bool FrameParser::push(std::uint8_t byte, Frame& frame) {
  // next() leaves at most kMaxFrameSize - 1 bytes: a candidate that reached
  // its declared size was either returned or discarded.
  buffer_[size_++] = byte;
  return next(frame);
}

bool FrameParser::flush(Frame& frame) {
  while (size_ != 0) {
    if (next(frame)) {
      return true;
    }
    if (size_ != 0) {
      discard_prefix(1);  // Stale incomplete candidate.
      ++rejected_frames_;
    }
  }
  return false;
}

bool FrameParser::next(Frame& frame) {
  while (size_ != 0) {
    if (buffer_[0] != kMagic) {
      discard_prefix(1);
      continue;
    }
    if (size_ < 3) {
      return false;
    }

    const auto frame_size = read_le16(buffer_ + 1);
    if (frame_size < kMinFrameSize || frame_size > kMaxFrameSize) {
      discard_prefix(1);
      ++rejected_frames_;
      continue;
    }
    if (size_ < frame_size) {
      return false;
    }
    if (crc16_ccitt_false(buffer_, frame_size) != 0) {
      discard_prefix(1);
      ++rejected_frames_;
      continue;
    }

    frame.type = read_le16(buffer_ + 3);
    frame.payload_size = frame_size - kMinFrameSize;
    std::memcpy(frame.payload, buffer_ + 5, frame.payload_size);
    discard_prefix(frame_size);
    return true;
  }
  return false;
}

bool decode_state(const Frame& frame, State& state) {
  if (frame.type != static_cast<std::uint16_t>(FrameType::StateResponse) ||
      frame.payload_size != kStatePayloadSize) {
    return false;
  }

  const auto* p = frame.payload;
  State next{};
  next.request_id = read_le32(p);
  const auto flags = p[4];
  next.power_on = flags & 0x01;
  next.sound_on = flags & 0x02;
  next.led_on = flags & 0x04;
  next.heater_state_bit = flags & 0x08;
  next.heater_allowed = (flags & 0x10) == 0;
  next.comm_source = flags & 0x20;
  next.filter_warning = flags & 0x40;
  next.heater_present = static_cast<std::uint8_t>(((flags >> 7) & 1) | ((p[5] & 0x03) << 1));
  next.magic_air_connected = p[5] & 0x04;
  next.magic_air_auto = p[5] & 0x08;
  next.gate_position = p[6];
  next.target_temperature = read_signed(p[7]);
  next.fan_speed = p[8];
  next.outdoor_temperature = read_signed(p[9]);
  next.current_temperature = read_signed(p[10]);
  next.control_board_temperature = read_signed(p[11]);
  next.power_board_temperature = read_signed(p[12]);
  next.work_seconds = read_le32(p + 13);
  next.fan_seconds = read_le32(p + 17);
  next.filter_seconds = read_le32(p + 21);
  next.airflow_counter = read_le32(p + 25);
  next.errors = read_le32(p + 29);
  next.max_fan_speed = p[33];
  next.heater_percent = p[34];
  state = next;
  return true;
}

bool decode_device_info(const Frame& frame, DeviceInfo& info) {
  if (frame.type != static_cast<std::uint16_t>(FrameType::DeviceInfoResponse) ||
      frame.payload_size != kDeviceInfoPayloadSize) {
    return false;
  }
  DeviceInfo next{};
  next.work_mode = frame.payload[0];
  next.device_type = read_le32(frame.payload + 1);
  next.firmware_version = read_le16(frame.payload + 5);
  next.hardware_version = read_le16(frame.payload + 7);
  info = next;
  return true;
}

}  // namespace tion4s
