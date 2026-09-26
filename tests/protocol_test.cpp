// Framing, session, command encoding and the control transaction.

#include "support/check.h"
#include "tion4s/control.h"
#include "tion4s/control_transaction.h"
#include "tion4s/crc.h"
#include "tion4s/describe.h"
#include "tion4s/protocol.h"
#include "tion4s/session.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using tion4s::ChangeGroup;
using tion4s::ControlCommand;
using tion4s::ControlFailure;
using tion4s::ControlPhase;
using tion4s::ControlStep;
using tion4s::Frame;
using tion4s::FrameParser;
using tion4s::FrameType;
using tion4s::PollingSession;
using tion4s::Request;
using tion4s::WriteRejection;

std::vector<std::uint8_t> hex(const char* text) {
  std::vector<std::uint8_t> bytes;
  while (*text != '\0') {
    while (*text == ' ') {
      ++text;
    }
    if (*text == '\0') {
      break;
    }
    char* end = nullptr;
    const auto value = std::strtoul(text, &end, 16);
    CHECK(end == text + 2 && value <= 255, "valid test vector");
    bytes.push_back(static_cast<std::uint8_t>(value));
    text = end;
  }
  return bytes;
}

bool feed(FrameParser& parser, const std::vector<std::uint8_t>& bytes, Frame& frame) {
  bool received = false;
  for (const auto byte : bytes) {
    received = parser.push(byte, frame) || received;
  }
  return received;
}

// dentra/esphome-tion 5d1c5b4, tests/test_hw.h: real protocol captures.
constexpr const char* kStateResponse =
    "3A 2A 00 31 32 0E 00 00 00 0F 21 00 0B 03 02 0B 0E 21 "
    "63 D1 26 00 E7 77 26 00 19 D6 C6 00 6E DE 75 00 "
    "00 00 00 00 06 00 3E 98";

// The captured state: power on, speed 3/6, sound and LED on, heater allowed,
// intake, target 11 C, 1000 W heater.
tion4s::State captured_state() {
  FrameParser parser;
  Frame frame;
  CHECK(feed(parser, hex(kStateResponse), frame), "captured state frame");
  tion4s::State state;
  CHECK(tion4s::decode_state(frame, state), "decode captured state");
  return state;
}

tion4s::Snapshot snapshot_02d0(const tion4s::State& state) {
  tion4s::Snapshot snapshot;
  snapshot.has_device_info = true;
  snapshot.device_info.device_type = tion4s::kDeviceType4S;
  snapshot.device_info.work_mode = tion4s::kWorkModeNormal;
  snapshot.device_info.firmware_version = tion4s::kFirmware02D0;
  snapshot.has_state = true;
  snapshot.fresh = true;
  snapshot.state = state;
  return snapshot;
}

WriteRejection encode(const tion4s::State& state, const ControlCommand& command,
                      std::uint32_t request_id, std::uint8_t (&output)[18]) {
  return tion4s::encode_command(state, command, request_id, output);
}

void test_golden_requests() {
  std::uint8_t frame[tion4s::kRequestFrameSize]{};
  tion4s::encode_request(FrameType::HeartbeatRequest, frame);
  const auto heartbeat = hex("3A 07 00 32 39 CE EC");
  CHECK(std::memcmp(frame, heartbeat.data(), 7) == 0, "heartbeat golden bytes");
  tion4s::encode_request(FrameType::StateRequest, frame);
  const auto state = hex("3A 07 00 32 32 7F 87");
  CHECK(std::memcmp(frame, state.data(), 7) == 0, "state request golden bytes");
  tion4s::encode_request(FrameType::DeviceInfoRequest, frame);
  CHECK(frame[3] == 0x32 && frame[4] == 0x33 && tion4s::crc16_ccitt_false(frame, 7) == 0,
        "device info request type and CRC");
  CHECK(static_cast<FrameType>(Request::Heartbeat) == FrameType::HeartbeatRequest &&
            static_cast<FrameType>(Request::State) == FrameType::StateRequest &&
            static_cast<FrameType>(Request::DeviceInfo) == FrameType::DeviceInfoRequest,
        "each scheduled request names its frame type");
}

void test_device_info() {
  Frame frame{};
  frame.type = static_cast<std::uint16_t>(FrameType::DeviceInfoResponse);
  frame.payload_size = tion4s::kDeviceInfoPayloadSize;
  frame.payload[0] = 1;     // Synthetic NORMAL work mode.
  frame.payload[1] = 0x03;  // BR4S = 0x8003, little-endian.
  frame.payload[2] = 0x80;
  frame.payload[5] = 0xD0;
  frame.payload[6] = 0x02;
  frame.payload[7] = 0x34;
  frame.payload[8] = 0x12;
  tion4s::DeviceInfo info{};
  CHECK(tion4s::decode_device_info(frame, info), "decode device metadata");
  CHECK(info.work_mode == 1 && info.device_type == 0x8003 &&
            info.firmware_version == 0x02D0 && info.hardware_version == 0x1234,
        "device metadata fields");
  frame.payload_size = 24;
  CHECK(!tion4s::decode_device_info(frame, info), "truncated device info rejected");
  frame.payload_size = tion4s::kDeviceInfoPayloadSize;
  frame.type = static_cast<std::uint16_t>(FrameType::StateResponse);
  CHECK(!tion4s::decode_device_info(frame, info), "wrong response type rejected");
}

void test_stream_and_state() {
  FrameParser parser;
  Frame frame;
  auto bytes = hex(kStateResponse);
  CHECK(bytes.size() == tion4s::kMaxFrameSize, "captured state frame size");
  for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
    CHECK(!parser.push(bytes[i], frame), "partial frame remains partial");
  }
  CHECK(parser.push(bytes.back(), frame), "complete state frame");
  CHECK(frame.payload_size == tion4s::kStatePayloadSize, "state payload size");

  tion4s::State state;
  CHECK(tion4s::decode_state(frame, state), "decode captured state");
  CHECK(state.request_id == 14 && state.power_on, "power and request id");
  CHECK(state.heater_state_bit && state.heater_allowed,
        "raw heater state bit and mode stay separate");
  CHECK(state.fan_speed == 3 && state.max_fan_speed == 6, "fan values");
  CHECK(state.sound_on && state.led_on && state.gate_position == 0 &&
            !state.filter_warning && state.target_temperature == 11,
        "captured sound, LED, gate, filter and setpoint fields");
  CHECK(state.filter_seconds == 13030937, "captured raw filter counter remains unsigned");
  CHECK(state.outdoor_temperature == 2 && state.current_temperature == 11,
        "signed temperature fields");
  CHECK(state.errors == 0 && state.heater_percent == 0, "errors and heat percentage");

  frame.payload[9] = 0xF9;  // Synthetic negative temperature, not a capture.
  CHECK(tion4s::decode_state(frame, state) && state.outdoor_temperature == -7,
        "negative temperature");
  frame.payload[4] |= 0x40;  // Synthetic filter warning, not a capture.
  frame.payload[6] = 1;      // Synthetic recirculation, not a capture.
  CHECK(tion4s::decode_state(frame, state) && state.filter_warning &&
            state.gate_position == 1,
        "filter warning and recirculation are distinct raw fields");
  frame.payload_size = 34;
  CHECK(!tion4s::decode_state(frame, state), "truncated state rejected");
}

void test_noise_crc_and_resync() {
  FrameParser parser;
  Frame frame;
  const auto heartbeat = hex("3A 08 00 31 39 00 E8 2B");
  auto bad = heartbeat;
  bad[3] ^= 0x80;
  auto stream = hex("FF 55 3A 00 00");
  stream.insert(stream.end(), bad.begin(), bad.end());
  stream.insert(stream.end(), heartbeat.begin(), heartbeat.end());
  CHECK(feed(parser, stream, frame), "resynchronize after invalid length and CRC");
  CHECK(frame.type == static_cast<std::uint16_t>(FrameType::HeartbeatResponse) &&
            frame.payload_size == 1 && frame.payload[0] == 0,
        "recovered heartbeat response");
  CHECK(parser.rejected_frames() >= 2, "invalid candidates counted");

  auto state = hex(kStateResponse);
  state.insert(state.end(), heartbeat.begin(), heartbeat.end());
  std::size_t count = 0;
  for (const auto byte : state) {
    if (parser.push(byte, frame)) {
      ++count;
    }
  }
  CHECK(count == 2, "concatenated frames");

  // A corrupt but plausible length can cover the next frame. Once enough
  // bytes arrive to disprove its CRC, the embedded valid frame is recovered.
  auto long_bad = hex("3A 2A 00 55 55");
  long_bad.insert(long_bad.end(), heartbeat.begin(), heartbeat.end());
  long_bad.resize(50, 0x55);
  CHECK(feed(parser, long_bad, frame), "recover frame inside malformed candidate");
  CHECK(frame.type == static_cast<std::uint16_t>(FrameType::HeartbeatResponse),
        "embedded heartbeat recovered");

  CHECK(!parser.push(0x3A, frame), "incomplete frame prefix");
  parser.reset();
  CHECK(feed(parser, heartbeat, frame), "reset discards a partial frame");

  // Two complete frames hidden inside one stale candidate: the first appears
  // when the candidate is disproved, the second must not wait for more input.
  FrameParser drain;
  auto hidden = hex("3A 13 00");
  hidden.insert(hidden.end(), heartbeat.begin(), heartbeat.end());
  hidden.insert(hidden.end(), heartbeat.begin(), heartbeat.end());
  std::size_t drained = 0;
  for (const auto byte : hidden) {
    bool received = drain.push(byte, frame);
    while (received) {
      ++drained;
      received = drain.next(frame);
    }
  }
  CHECK(drained == 2 && drain.empty(), "next() drains every complete buffered frame");

  // A stale candidate that claims more bytes than will ever arrive.
  FrameParser idle;
  auto stuck = hex("3A 2A 00");
  stuck.insert(stuck.end(), heartbeat.begin(), heartbeat.end());
  CHECK(!feed(idle, stuck, frame), "frame hidden while the candidate is incomplete");
  const auto rejected = idle.rejected_frames();
  CHECK(idle.flush(frame) &&
            frame.type == static_cast<std::uint16_t>(FrameType::HeartbeatResponse),
        "flush after idle recovers the hidden frame");
  CHECK(!idle.flush(frame) && idle.empty() && idle.rejected_frames() == rejected + 1,
        "flush leaves an empty buffer and counts the stale candidate");

  // A stale header followed only by noise leaves nothing to recover.
  FrameParser noisy;
  CHECK(
      !feed(noisy, hex("3A 2A 00 11 22"), frame) && !noisy.flush(frame) && noisy.empty(),
      "flush discards a stale header and the noise behind it");
  FrameParser short_size;
  CHECK(!feed(short_size, hex("3A 03 00"), frame) && short_size.empty() &&
            short_size.rejected_frames() == 1,
        "a declared size below the frame overhead is rejected");
}

void test_session_and_clock_wrap() {
  PollingSession session;
  constexpr std::uint32_t start = 0xFFFF'FFF0u;
  CHECK(session.next_request(start) == Request::Heartbeat, "heartbeat starts first");
  session.sent(Request::Heartbeat, start);
  CHECK(session.next_request(start) == Request::State, "state follows heartbeat");
  session.sent(Request::State, start);
  CHECK(session.next_request(start) == Request::DeviceInfo,
        "device info follows state without delaying heartbeat");
  session.sent(Request::DeviceInfo, start);
  CHECK(session.next_request(start + 100) == Request::None, "no early request");
  CHECK(session.next_request(start + 2000) == Request::None,
        "do not stack state requests while awaiting a response");
  CHECK(session.next_request(start + 3000) == Request::Heartbeat,
        "heartbeat priority across wrap");
  session.sent(Request::Heartbeat, start + 3000);
  CHECK(session.next_request(start + 3000) == Request::State, "bounded state retry");
  session.sent(Request::State, start + 3000);
  CHECK(session.snapshot(start + 3000).max_heartbeat_gap_ms == 3000,
        "actual heartbeat send gap");
  CHECK(session.snapshot(start + 3100).heartbeat_age_ms == 100,
        "time since last heartbeat transmission");

  FrameParser parser;
  Frame frame;
  CHECK(feed(parser, hex(kStateResponse), frame), "valid state input");
  session.accept(frame, start + 3000);
  auto snapshot = session.snapshot(start + 3000);
  CHECK(snapshot.has_state && snapshot.fresh && snapshot.state_count == 1,
        "first reported state is fresh");
  CHECK(session.next_request(start + 3000) == Request::None,
        "valid response clears wait");
  CHECK(session.next_request(start + 5000) == Request::State,
        "poll again after completed response");
  CHECK(!session.snapshot(start + 3000 + PollingSession::kStateFreshnessMs + 1).fresh,
        "old state becomes stale");

  Frame info_frame{};
  info_frame.type = static_cast<std::uint16_t>(FrameType::DeviceInfoResponse);
  info_frame.payload_size = tion4s::kDeviceInfoPayloadSize;
  info_frame.payload[1] = 0x03;
  info_frame.payload[2] = 0x80;
  session.accept(info_frame, start + 4000);
  CHECK(session.snapshot(start + 4000).has_device_info &&
            session.snapshot(start + 4000).device_info.device_type == 0x8003,
        "device info response retained");

  frame.type = 0x3230;  // Synthetic write response type is ignored.
  session.accept(frame, start + 4000);
  CHECK(session.snapshot(start + 4000).state_count == 1, "unrelated frame ignored");
}

void test_session_retries_and_ignores() {
  PollingSession session;
  session.sent(Request::Heartbeat, 0);
  session.sent(Request::State, 0);
  session.sent(Request::DeviceInfo, 0);
  session.sent(Request::None, 0);
  Frame state{};
  state.type = static_cast<std::uint16_t>(FrameType::StateResponse);
  state.payload_size = tion4s::kStatePayloadSize;
  session.accept(state, 100);
  CHECK(session.next_request(2100) == Request::State, "None is not a request");
  session.sent(Request::State, 2100);
  session.accept(state, 2200);
  CHECK(session.next_request(2300) == Request::None &&
            session.next_request(3000) == Request::Heartbeat,
        "device info is not asked again early");
  session.sent(Request::Heartbeat, 3000);
  session.sent(Request::State, 8100);
  session.accept(state, 8200);
  session.sent(Request::Heartbeat, 9000);
  CHECK(session.next_request(9900) == Request::None &&
            session.next_request(10000) == Request::DeviceInfo,
        "unanswered device info is asked again after ten seconds");

  Frame heartbeat{};
  heartbeat.type = static_cast<std::uint16_t>(FrameType::HeartbeatResponse);
  heartbeat.payload_size = 1;
  session.accept(heartbeat, 10000);
  heartbeat.payload_size = 2;
  session.accept(heartbeat, 10000);
  CHECK(session.snapshot(10000).heartbeat_count == 1,
        "a heartbeat response of the wrong size is not counted");
}

void test_state_never_fresh_again_after_wrap() {
  FrameParser parser;
  Frame frame;
  CHECK(feed(parser, hex(kStateResponse), frame), "state for expiry");
  PollingSession session;
  constexpr std::uint32_t received = 5000;
  session.accept(frame, received);
  session.tick(received + PollingSession::kStateFreshnessMs + 1);
  // 49.7 days later the unsigned age wraps to a small number.
  const auto wrapped = session.snapshot(received + 2000);
  CHECK(!wrapped.fresh && wrapped.age_ms == PollingSession::kUnknownAgeMs,
        "an expired state does not become fresh when its age wraps");
  const auto measurable =
      session.snapshot(received + PollingSession::kStateFreshnessMs + 500);
  CHECK(!measurable.fresh && measurable.age_ms == PollingSession::kStateFreshnessMs + 500,
        "a measurable age of an expired state is still reported");
  session.accept(frame, received + 3000);
  CHECK(session.snapshot(received + 3000).fresh, "a new state clears the expiry");
}

void test_fan_mapping() {
  for (std::uint8_t maximum = 1; maximum <= tion4s::kMaxFanSteps; ++maximum) {
    for (std::uint8_t speed = 1; speed <= maximum; ++speed) {
      const auto percent = tion4s::speed_to_percent(speed, maximum);
      CHECK(tion4s::percent_to_speed(percent, maximum) == speed,
            "each supported speed survives HomeKit percentage round-trip");
    }
    CHECK(tion4s::percent_to_speed(0, maximum) == 0, "zero means off");
    CHECK(tion4s::percent_to_speed(1, maximum) == 1,
          "small positive value selects step one");
    CHECK(tion4s::percent_to_speed(100, maximum) == maximum,
          "full percentage selects maximum step");
  }
  CHECK(tion4s::speed_to_percent(7, 6) == 0 && tion4s::speed_to_percent(0, 6) == 0 &&
            tion4s::speed_to_percent(3, 0) == 0 && tion4s::speed_to_percent(3, 7) == 0,
        "invalid reported speed or range is not presented");
  CHECK(tion4s::percent_to_speed(101, 6) == 0 && tion4s::percent_to_speed(50, 0) == 0 &&
            tion4s::percent_to_speed(50, 7) == 0,
        "out-of-range request or range rejected");
}

void test_fan_encoding() {
  auto state = captured_state();
  std::uint8_t output[18]{};
  CHECK(encode(state, ControlCommand::power(false), 2, output) == WriteRejection::None,
        "encode power-off change");
  const auto expected = hex("3A 12 00 30 32 02 00 00 00 06 00 00 0B 03 00 00 7F B4");
  CHECK(std::memcmp(output, expected.data(), sizeof(output)) == 0,
        "power-off packet preserves speed, sound, LED, heater mode and target");

  CHECK(encode(state, ControlCommand::power(true), 3, output) == WriteRejection::NoChange,
        "requested value already reported");
  state.magic_air_auto = true;
  CHECK(encode(state, ControlCommand::power(false), 3, output) ==
            WriteRejection::MagicAirAuto,
        "MagicAir auto control blocks local write");
  state.magic_air_auto = false;
  state.errors = 1;
  CHECK(encode(state, ControlCommand::power(false), 3, output) == WriteRejection::None &&
            (output[9] & 0x01) == 0,
        "reported device error does not block turning the breezer off");
  state.power_on = false;
  CHECK(encode(state, ControlCommand::power(true), 3, output) ==
            WriteRejection::DeviceErrors,
        "reported device error blocks turning the breezer on");
  state.power_on = true;
  CHECK(encode(state, ControlCommand::power_and_speed(false, 2), 3, output) ==
            WriteRejection::DeviceErrors,
        "only a plain power-off bypasses reported device errors");
  state.errors = 0;
  CHECK(encode(state, ControlCommand::speed_step(7), 3, output) ==
            WriteRejection::InvalidRequest,
        "speed beyond reported maximum rejected");
  CHECK(encode(state, ControlCommand::speed_step(0), 3, output) ==
            WriteRejection::InvalidRequest,
        "speed zero is not a stored step");
  CHECK(encode(state, ControlCommand::speed_step(5), 1, output) ==
            WriteRejection::ReservedRequestId,
        "request ID 1 belongs to reads");
  CHECK(encode(state, ControlCommand::power_and_speed(true, 5), 4, output) ==
                WriteRejection::None &&
            output[13] == 5 && (output[9] & 0x01) != 0,
        "power and speed in one write");
  state.max_fan_speed = 7;
  state.fan_speed = 7;
  CHECK(encode(state, ControlCommand::speed_step(2), 4, output) ==
            WriteRejection::InvalidBaseline,
        "a baseline beyond six steps is not relayed");
  state.max_fan_speed = 6;
  state.fan_speed = 0;
  CHECK(encode(state, ControlCommand::power(false), 4, output) ==
            WriteRejection::InvalidBaseline,
        "a baseline without a stored step is not relayed");
  state.fan_speed = 3;
  state.max_fan_speed = 0;
  CHECK(encode(state, ControlCommand::power(false), 4, output) ==
            WriteRejection::InvalidBaseline,
        "a baseline without a speed range is not relayed");
  state.max_fan_speed = 2;
  CHECK(encode(state, ControlCommand::power(false), 4, output) ==
            WriteRejection::InvalidBaseline,
        "a baseline speed above its own maximum is not relayed");
}

// Fields the write relays unchanged from the reported state.
void test_relayed_bits() {
  auto state = captured_state();
  state.comm_source = true;
  state.magic_air_connected = true;
  std::uint8_t output[18]{};
  CHECK(encode(state, ControlCommand::power(false), 2, output) == WriteRejection::None &&
            output[9] == 0x16 && output[10] == 0x01 &&
            tion4s::crc16_ccitt_false(output, sizeof(output)) == 0,
        "communication source and MagicAir presence are relayed");
}

void test_gate_and_settings_encoding() {
  auto state = captured_state();
  std::uint8_t output[18]{};
  CHECK(encode(state, ControlCommand::recirculation(true), 2, output) ==
            WriteRejection::UnsafeCombination,
        "recirculation cannot preserve enabled heater");
  state.heater_allowed = false;
  CHECK(encode(state, ControlCommand::recirculation(true), 2, output) ==
            WriteRejection::None,
        "encode recirculation with heater disabled");
  CHECK(output[3] == 0x30 && output[4] == 0x32 && output[9] == 0x0F && output[10] == 0 &&
            output[11] == 1 && output[12] == 11 && output[13] == 3 && output[14] == 0 &&
            output[15] == 0 && tion4s::crc16_ccitt_false(output, sizeof(output)) == 0,
        "transient gate change preserves other state and clears reset bits");
  CHECK(encode(state, ControlCommand::recirculation(false), 2, output) ==
            WriteRejection::NoChange,
        "unchanged gate does not cause a write");

  ControlCommand settings = ControlCommand::sound(false);
  settings.led_on = false;
  CHECK(settings.group() == ChangeGroup::Settings, "sound and LED share a group");
  CHECK(encode(state, settings, 3, output) == WriteRejection::None,
        "encode persistent sound and LED settings");
  CHECK(output[3] == 0x34 && output[4] == 0x32 && output[9] == 0x09 && output[11] == 0 &&
            output[12] == 11 && output[13] == 3 &&
            tion4s::crc16_ccitt_false(output, sizeof(output)) == 0,
        "persistent frame changes only requested settings");

  ControlCommand mixed = settings;
  mixed.gate_position = 1;
  CHECK(mixed.group() == ChangeGroup::Mixed &&
            encode(state, mixed, 4, output) == WriteRejection::InvalidRequest,
        "persistent settings cannot mix with a gate change");
  state.magic_air_auto = true;
  CHECK(encode(state, settings, 4, output) == WriteRejection::MagicAirAuto,
        "MagicAir auto blocks settings write");
  state.magic_air_auto = false;
  state.errors = 1;
  CHECK(encode(state, settings, 4, output) == WriteRejection::DeviceErrors,
        "hardware error blocks settings write");
  state.errors = 0;
  state.gate_position = 1;
  state.heater_allowed = true;
  CHECK(encode(state, settings, 4, output) == WriteRejection::InvalidBaseline,
        "contradictory heater and recirculation state blocks full write");
  state.gate_position = 0;
  state.heater_allowed = false;
  state.target_temperature = 31;
  CHECK(encode(state, settings, 4, output) == WriteRejection::InvalidBaseline,
        "out-of-range target in baseline blocks full write");
  state.target_temperature = -26;
  CHECK(encode(state, settings, 4, output) == WriteRejection::InvalidBaseline,
        "negative target below upstream envelope blocks full write");
  state.target_temperature = 11;
  ControlCommand bad_gate;
  bad_gate.gate_position = 2;
  CHECK(encode(state, bad_gate, 5, output) == WriteRejection::InvalidRequest,
        "only outdoor and recirculation gate positions exist");
  state.gate_position = 2;
  CHECK(encode(state, settings, 5, output) == WriteRejection::InvalidBaseline,
        "an unknown reported gate position is not relayed");
  state.gate_position = 1;
  CHECK(encode(state, ControlCommand::recirculation(false), 5, output) ==
                WriteRejection::None &&
            output[11] == 0,
        "leaving recirculation is always allowed");
  state.gate_position = 0;
  CHECK(encode(state, ControlCommand{}, 5, output) == WriteRejection::InvalidRequest,
        "a command without changes is invalid");
}

void test_filter_reset_encoding() {
  auto state = captured_state();
  std::uint8_t output[18]{};
  state.heater_allowed = false;
  state.power_on = false;
  CHECK(encode(state, ControlCommand::filter_reset(), 5, output) == WriteRejection::None,
        "encode explicit filter reset while powered off");
  CHECK(output[3] == 0x30 && output[4] == 0x32 && output[9] == 0x8E &&
            tion4s::crc16_ccitt_false(output, sizeof(output)) == 0,
        "filter reset sets only its action bit");
  state.power_on = true;
  CHECK(encode(state, ControlCommand::filter_reset(), 6, output) ==
            WriteRejection::FilterResetNeedsStop,
        "filter reset blocked while breezer is running");
  state.power_on = false;
  state.heater_allowed = true;
  CHECK(encode(state, ControlCommand::filter_reset(), 6, output) ==
            WriteRejection::FilterResetNeedsStop,
        "filter reset blocked while heater is enabled");
}

void test_heater_encoding() {
  auto state = captured_state();
  CHECK(state.heater_present != 0, "golden frame reports a heater");
  std::uint8_t output[18]{};
  CHECK(encode(state, ControlCommand::heater(false), 2, output) == WriteRejection::None,
        "encode explicit heater disable");
  CHECK(output[3] == 0x30 && output[4] == 0x32 && output[9] == 0x0F && output[11] == 0 &&
            output[12] == 11 && output[13] == 3 &&
            tion4s::crc16_ccitt_false(output, sizeof(output)) == 0,
        "heater disable preserves power, gate, speed and target");

  CHECK(encode(state, ControlCommand::target(12), 3, output) == WriteRejection::None,
        "target change keeps an allowed heater in safe mode");
  CHECK(output[9] == 0x07 && output[12] == 12,
        "target change does not disable an already allowed heater");
  state.power_on = false;
  CHECK(encode(state, ControlCommand::target(12), 3, output) ==
            WriteRejection::UnsafeCombination,
        "target change cannot relay enabled heater while powered off");
  state.power_on = true;
  state.heater_allowed = false;
  CHECK(encode(state, ControlCommand::target(12), 3, output) == WriteRejection::None,
        "encode target-only change");
  CHECK(output[9] == 0x0F && output[12] == 12 && output[13] == 3 &&
            tion4s::crc16_ccitt_false(output, sizeof(output)) == 0,
        "target change does not enable heater or alter fan");

  CHECK(encode(state, ControlCommand::heater(true), 4, output) == WriteRejection::None,
        "encode heater enable on supported outdoor mode");
  CHECK(output[9] == 0x07 && output[11] == 0 && output[12] == 11 &&
            tion4s::crc16_ccitt_false(output, sizeof(output)) == 0,
        "heater enable changes only permission bit");
  CHECK(encode(state, ControlCommand::heater_and_target(true, 22), 4, output) ==
                WriteRejection::None &&
            output[9] == 0x07 && output[12] == 22,
        "heater and target in one write");

  const auto unsafe = [&state, &output](const char* message) {
    CHECK(encode(state, ControlCommand::heater(true), 5, output) ==
              WriteRejection::UnsafeCombination,
          message);
  };
  state.gate_position = 1;
  unsafe("recirculation blocks heater enable");
  state.gate_position = 0;
  state.power_on = false;
  unsafe("powered-off breezer blocks heater enable");
  state.power_on = true;
  state.heater_present = 0;
  unsafe("missing reported heater blocks enable");
  state.heater_present = 3;
  unsafe("unknown reported heater variant blocks enable");
  state.heater_present = 2;
  state.target_temperature = 30;
  unsafe("unconfirmed target range blocks heater enable");
  state.target_temperature = 11;
  CHECK(encode(state, ControlCommand::target(26), 5, output) ==
            WriteRejection::InvalidRequest,
        "target beyond conservative range rejected");
  CHECK(encode(state, ControlCommand::target(11), 5, output) == WriteRejection::NoChange,
        "unchanged target does not cause a write");
  CHECK(encode(state, ControlCommand::target(0), 5, output) ==
            WriteRejection::InvalidRequest,
        "target below 1 C rejected");
  state.target_temperature = 0;
  unsafe("a reported target below 1 C blocks heater enable");
  state.target_temperature = 11;

  state.heater_allowed = true;
  state.errors = 0x10;
  CHECK(encode(state, ControlCommand::heater(false), 6, output) == WriteRejection::None &&
            (output[9] & 0x08) != 0,
        "reported device error does not block disabling the heater");
  CHECK(encode(state, ControlCommand::target(12), 6, output) ==
            WriteRejection::DeviceErrors,
        "reported device error blocks a target change");
  CHECK(encode(state, ControlCommand::heater_and_target(false, 12), 6, output) ==
            WriteRejection::DeviceErrors,
        "only a plain heater disable bypasses reported device errors");
}

void test_commands() {
  CHECK(ControlCommand{}.group() == ChangeGroup::None, "empty command");
  CHECK(ControlCommand::power_and_speed(true, 3).group() == ChangeGroup::Fan,
        "fan group");
  CHECK(ControlCommand::recirculation(true).group() == ChangeGroup::Gate, "gate group");
  CHECK(ControlCommand::led(true).group() == ChangeGroup::Settings, "settings group");
  CHECK(ControlCommand::heater_and_target(true, 20).group() == ChangeGroup::Heater,
        "heater group");
  CHECK(ControlCommand::filter_reset().group() == ChangeGroup::FilterReset,
        "filter reset group");

  const auto older = ControlCommand::power(false);
  const auto newer = ControlCommand::speed_step(4);
  CHECK(tion4s::can_merge(older, newer), "fan changes merge");
  const auto merged = tion4s::merge(older, newer);
  CHECK(merged.power_on == false && merged.fan_speed == 4,
        "newer fields overlay older ones");
  CHECK(tion4s::merge(ControlCommand::heater(true), ControlCommand::target(22)).group() ==
            ChangeGroup::Heater,
        "heater permission and target merge");
  CHECK(!tion4s::can_merge(ControlCommand::sound(true),
                           ControlCommand::recirculation(true)),
        "different groups never merge");
  CHECK(
      !tion4s::can_merge(ControlCommand::filter_reset(), ControlCommand::filter_reset()),
      "filter reset never merges");
  const tion4s::CommandContext context{3, 0, false};
  CHECK(!tion4s::can_merge(ControlCommand::speed_step(2),
                           ControlCommand::speed_in_context(6, context)) &&
            !tion4s::can_merge(ControlCommand::speed_in_context(6, context),
                               ControlCommand::speed_step(2)),
        "context-bound commands never merge");
  ControlCommand mixed = ControlCommand::power(true);
  mixed.sound_on = true;
  CHECK(!tion4s::can_merge(ControlCommand{}, ControlCommand{}) &&
            !tion4s::can_merge(mixed, mixed),
        "empty and mixed commands never merge");
  const auto gate = tion4s::merge(ControlCommand::recirculation(true),
                                  ControlCommand::recirculation(false));
  CHECK(gate.gate_position == 0, "the latest gate position wins");
  const auto settings =
      tion4s::merge(ControlCommand::sound(false), ControlCommand::led(true));
  CHECK(settings.sound_on == false && settings.led_on == true &&
            tion4s::merge(settings, ControlCommand::sound(true)).sound_on == true,
        "sound and LED merge field by field");

  auto state = captured_state();
  CHECK(tion4s::command_matches(ControlCommand::power_and_speed(true, 3), state),
        "all requested fields already reported");
  CHECK(!tion4s::command_matches(ControlCommand::power_and_speed(true, 4), state),
        "one differing field is enough");
  CHECK(!tion4s::command_matches(ControlCommand::filter_reset(), state),
        "an action never matches");
  CHECK(!tion4s::command_matches(ControlCommand{}, state),
        "an empty command never matches");
  ControlCommand mixed_match = ControlCommand::power(true);
  mixed_match.sound_on = true;
  CHECK(!tion4s::command_matches(mixed_match, state), "a mixed command never matches");
}

bool named(const char* label, const char* expected) {
  return std::strcmp(label, expected) == 0;
}

void test_describe() {
  using tion4s::describe;
  const std::vector<const char*> phases = {"idle",
                                           "queued",
                                           "reading",
                                           "writing",
                                           "verifying",
                                           "applied",
                                           "unchanged, no write",
                                           "failed"};
  for (std::size_t i = 0; i < phases.size(); ++i) {
    CHECK(named(describe(static_cast<ControlPhase>(i)), phases[i]), "phase label");
  }
  const std::vector<const char*> failures = {"none",
                                             "unsupported device",
                                             "stale state",
                                             "mode changed",
                                             "refused",
                                             "read timeout",
                                             "verification timeout",
                                             "reported state did not change",
                                             "UART transport",
                                             "expired before write"};
  for (std::size_t i = 0; i < failures.size(); ++i) {
    CHECK(named(describe(static_cast<ControlFailure>(i)), failures[i]), "failure label");
  }
  const std::vector<const char*> refusals = {"none",
                                             "invalid request",
                                             "reserved request ID",
                                             "reported state cannot be relayed",
                                             "MagicAir automation active",
                                             "breezer reports errors",
                                             "nothing to change",
                                             "unsafe heat or recirculation",
                                             "filter reset needs breezer and heat off"};
  for (std::size_t i = 0; i < refusals.size(); ++i) {
    CHECK(named(describe(static_cast<WriteRejection>(i)), refusals[i]), "refusal label");
  }
  // A corrupted value still gets a label on the diagnostics page. The enums
  // have a fixed underlying type, so every byte is a valid value.
  // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
  CHECK(named(describe(static_cast<ControlPhase>(99)), "unknown") &&
            named(describe(static_cast<ControlFailure>(99)), "unknown") &&
            named(describe(static_cast<WriteRejection>(99)), "unknown"),
        "values outside the enumerations");
  // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)

  ControlCommand mixed = ControlCommand::power(true);
  mixed.sound_on = true;
  ControlCommand sound_and_led = ControlCommand::sound(true);
  sound_and_led.led_on = true;
  CHECK(
      named(describe(ControlCommand{}), "none") &&
          named(describe(ControlCommand::power_and_speed(true, 2)), "power and speed") &&
          named(describe(ControlCommand::power(true)), "power") &&
          named(describe(ControlCommand::speed_step(2)), "speed") &&
          named(describe(ControlCommand::recirculation(true)), "recirculation") &&
          named(describe(sound_and_led), "sound and LED") &&
          named(describe(ControlCommand::sound(true)), "sound") &&
          named(describe(ControlCommand::led(true)), "LED") &&
          named(describe(ControlCommand::heater_and_target(true, 20)),
                "heater and target") &&
          named(describe(ControlCommand::heater(true)), "heater permission") &&
          named(describe(ControlCommand::target(20)), "target temperature") &&
          named(describe(ControlCommand::filter_reset()), "filter reset") &&
          named(describe(mixed), "mixed groups"),
      "command labels");
}

void test_device_policy() {
  tion4s::DeviceInfo info;
  info.device_type = tion4s::kDeviceType4S;
  info.work_mode = tion4s::kWorkModeNormal;
  CHECK(tion4s::is_4s_normal_mode(info), "4S in normal mode");
  info.work_mode = 0;
  CHECK(!tion4s::is_4s_normal_mode(info), "other work mode");
  CHECK(!tion4s::firmware_allows_control(0x02D0, false) &&
            tion4s::firmware_allows_control(0x02D0, true),
        "02D0 needs the explicit opt-in");
  CHECK(!tion4s::firmware_allows_control(0x02D1, true) &&
            tion4s::firmware_allows_control(0x02D2, false) &&
            !tion4s::firmware_allows_control(0x03CD, true),
        "upstream minimum and blocked firmware");
}

void test_control_transaction_02d0() {
  FrameParser parser;
  Frame frame;
  CHECK(feed(parser, hex(kStateResponse), frame), "control test baseline");
  auto state = captured_state();
  state.heater_allowed = false;
  frame.payload[4] |= 0x10;
  const auto snapshot = snapshot_02d0(state);

  const auto command = ControlCommand::recirculation(true);
  tion4s::ControlTransaction transaction;
  CHECK(!transaction.submit(command, snapshot, false, 100) &&
            transaction.failure() == ControlFailure::UnsupportedDevice,
        "02D0 write requires explicit opt-in");
  CHECK(transaction.submit(command, snapshot, true, 100),
        "opted-in 02D0 command may enter bounded transaction");
  auto output = transaction.next(100);
  CHECK(output.step == ControlStep::Read && output.size == 7,
        "preflight read comes before legacy write");
  transaction.sent(output.step, 100);
  frame.payload[0] = 0;  // Observed 02D0 request IDs are not upstream's 1.
  transaction.observe(frame, 120);
  output = transaction.next(120);
  CHECK(output.step == ControlStep::Write && output.size == 18 && output.bytes[11] == 1 &&
            output.bytes[13] == 3,
        "one gate write preserves the fresh baseline speed");
  transaction.sent(output.step, 121);
  CHECK(transaction.next(250).step == ControlStep::None, "no early verification read");
  output = transaction.next(301);
  CHECK(output.step == ControlStep::Verify && output.size == 7,
        "write is followed by an explicit verification read");
  transaction.sent(output.step, 301);
  frame.payload[6] = 1;
  transaction.observe(frame, 320);
  CHECK(transaction.phase() == ControlPhase::Applied,
        "legacy response confirms changed field without trusting request ID");

  tion4s::ControlTransaction blocked;
  CHECK(blocked.submit(command, snapshot, true, 100), "second scenario starts");
  blocked.sent(blocked.next(100).step, 100);
  CHECK(blocked.next(1101).step == ControlStep::None &&
            blocked.phase() == ControlPhase::Failed &&
            blocked.failure() == ControlFailure::ReadTimeout,
        "missing baseline never reaches write");

  tion4s::ControlTransaction uncertain;
  CHECK(uncertain.submit(command, snapshot, true, 100), "uncertain scenario starts");
  uncertain.sent(uncertain.next(100).step, 100);
  frame.payload[6] = 0;
  uncertain.observe(frame, 120);
  uncertain.sent(uncertain.next(120).step, 121);
  uncertain.sent(uncertain.next(301).step, 301);
  uncertain.sent(uncertain.next(802).step, 802);
  uncertain.sent(uncertain.next(1303).step, 1303);
  CHECK(uncertain.next(1804).step == ControlStep::None &&
            uncertain.phase() == ControlPhase::Failed &&
            uncertain.failure() == ControlFailure::VerifyTimeout,
        "unconfirmed write expires without replay");

  auto boosted = snapshot;
  boosted.state.fan_speed = 6;
  const auto restore =
      ControlCommand::speed_in_context(3, tion4s::CommandContext{6, 0, false});
  CHECK(blocked.submit(restore, boosted, true, 2000),
        "boost restoration accepts expected reported context");
  blocked.sent(blocked.next(2000).step, 2000);
  frame.payload[8] = 5;  // A native control changed speed before preflight.
  blocked.observe(frame, 2010);
  CHECK(blocked.next(2010).step == ControlStep::None &&
            blocked.phase() == ControlPhase::Failed &&
            blocked.failure() == ControlFailure::ContextChanged,
        "native speed change cancels boost restoration before write");
}

void test_control_admission() {
  auto state = captured_state();
  tion4s::Snapshot snapshot;
  snapshot.has_state = true;
  snapshot.fresh = true;
  snapshot.state = state;
  const auto command = ControlCommand::power(false);
  tion4s::ControlTransaction transaction;
  CHECK(!transaction.submit(command, snapshot, true, 100) &&
            transaction.failure() == ControlFailure::UnsupportedDevice,
        "device information is required before any control");
  snapshot.has_device_info = true;
  snapshot.device_info.device_type = 0x8002;
  snapshot.device_info.work_mode = 1;
  snapshot.device_info.firmware_version = 0x02E0;
  CHECK(!transaction.submit(command, snapshot, false, 100),
        "wrong Tion model cannot be controlled");
  snapshot.device_info.device_type = 0x8003;
  snapshot.device_info.work_mode = 0;
  CHECK(!transaction.submit(command, snapshot, false, 100),
        "non-normal work mode cannot be controlled");
  snapshot.device_info.work_mode = 1;
  snapshot.device_info.firmware_version = 0x03CD;
  CHECK(!transaction.submit(command, snapshot, false, 100),
        "upstream-broken 03CD remains blocked");
  snapshot.device_info.firmware_version = 0x02E0;
  snapshot.age_ms = 3001;
  CHECK(!transaction.submit(command, snapshot, false, 100) &&
            transaction.failure() == ControlFailure::StaleState,
        "aged baseline cannot enter transaction");
  snapshot.age_ms = 0;
  CHECK(!transaction.submit(ControlCommand{}, snapshot, false, 100) &&
            transaction.failure() == ControlFailure::Refused &&
            transaction.refusal() == WriteRejection::InvalidRequest,
        "a command without changes is refused at admission");
  auto magic = snapshot;
  magic.state.magic_air_auto = true;
  CHECK(transaction.submit(command, magic, false, 100),
        "admission does not judge the write from the snapshot");
  transaction.next(4200);  // Let it expire.
  constexpr std::uint32_t near_wrap = 0xFFFF'FFF0u;
  CHECK(transaction.submit(command, snapshot, false, near_wrap),
        "supported firmware enters transaction");
  CHECK(transaction.next(near_wrap + 10).step == ControlStep::Read,
        "preflight scheduling survives millis wrap");
  CHECK(!transaction.submit(command, snapshot, false, near_wrap + 11),
        "busy rejection does not replace an accepted transaction");
  transaction.transport_failed();
  CHECK(transaction.phase() == ControlPhase::Failed &&
            transaction.failure() == ControlFailure::Transport,
        "transport failure ends a running transaction");
  tion4s::ControlTransaction idle;
  idle.transport_failed();
  CHECK(idle.phase() == ControlPhase::Idle, "transport failure while idle is ignored");
  CHECK(idle.submit(command, snapshot, false, near_wrap), "fresh transaction");
  CHECK(idle.next(near_wrap + 4001).step == ControlStep::None &&
            idle.failure() == ControlFailure::Expired,
        "expired mailbox intent cannot emit even its preflight request");
  CHECK(!idle.submit(command, snapshot, false, near_wrap, near_wrap + 4001) &&
            idle.failure() == ControlFailure::Expired,
        "an intent that aged out in the mailbox is refused at admission");
}

void test_unchanged_and_timeouts() {
  FrameParser parser;
  Frame frame;
  CHECK(feed(parser, hex(kStateResponse), frame), "unchanged baseline frame");
  auto snapshot = snapshot_02d0(captured_state());

  // The snapshot says speed 3 already; the fresh pre-read confirms it.
  tion4s::ControlTransaction same;
  CHECK(same.submit(ControlCommand::speed_step(3), snapshot, true, 100),
        "a snapshot match still gets a fresh pre-read");
  same.sent(same.next(100).step, 100);
  same.observe(frame, 120);
  CHECK(same.next(120).step == ControlStep::None &&
            same.phase() == ControlPhase::Unchanged &&
            same.failure() == ControlFailure::None,
        "target already reached: no write, not a failure");

  // The snapshot is stale: the device moved before the pre-read.
  tion4s::ControlTransaction moved;
  snapshot.state.fan_speed = 5;
  CHECK(moved.submit(ControlCommand::speed_step(5), snapshot, true, 200),
        "command matching an old snapshot is admitted");
  moved.sent(moved.next(200).step, 200);
  moved.observe(frame, 220);
  const auto output = moved.next(220);
  CHECK(output.step == ControlStep::Write && output.bytes[13] == 5,
        "fresh pre-read showing a different value leads to one write");
  moved.sent(output.step, 221);
  moved.next(4300);
  CHECK(moved.failure() == ControlFailure::VerifyTimeout,
        "timeout after the write is reported as verification timeout");

  // A pre-read that is too old by the time the write could be sent.
  tion4s::ControlTransaction late;
  CHECK(late.submit(ControlCommand::speed_step(4), snapshot, true, 300), "late scenario");
  late.sent(late.next(300).step, 300);
  late.observe(frame, 320);
  CHECK(late.next(900).step == ControlStep::None &&
            late.failure() == ControlFailure::StaleState,
        "a pre-read older than half a second is not written from");

  // The encoder refuses the fresh pre-read: the reason is kept.
  tion4s::ControlTransaction refused;
  Frame errors = frame;
  errors.payload[29] = 0x04;  // Synthetic device error.
  CHECK(refused.submit(ControlCommand::speed_step(4), snapshot, true, 400),
        "refusal scenario");
  refused.sent(refused.next(400).step, 400);
  refused.observe(errors, 420);
  CHECK(refused.next(420).step == ControlStep::None &&
            refused.failure() == ControlFailure::Refused &&
            refused.refusal() == WriteRejection::DeviceErrors,
        "the refusal reason survives into the failure");
}

void set_request_id(Frame& frame, std::uint32_t id) {
  for (unsigned i = 0; i < 4; ++i) {
    frame.payload[i] = static_cast<std::uint8_t>(id >> (8 * i));
  }
}

// Upstream firmware (02D2 and later) answers reads with request ID 1 and a
// write with the write's own ID; other IDs belong to earlier exchanges.
void test_transaction_request_ids() {
  FrameParser parser;
  Frame frame;
  CHECK(feed(parser, hex(kStateResponse), frame), "baseline frame");
  auto snapshot = snapshot_02d0(captured_state());
  snapshot.device_info.firmware_version = 0x02E0;
  tion4s::ControlTransaction transaction;
  CHECK(transaction.submit(ControlCommand::speed_step(4), snapshot, false, 100),
        "upstream firmware needs no opt-in");
  transaction.sent(transaction.next(100).step, 100);
  set_request_id(frame, 14);
  transaction.observe(frame, 110);
  CHECK(transaction.next(110).step == ControlStep::None &&
            transaction.phase() == ControlPhase::Reading,
        "an answer with another ID is not the baseline");
  set_request_id(frame, 1);
  transaction.observe(frame, 120);
  auto duplicate = frame;
  duplicate.payload[8] = 4;
  transaction.observe(duplicate, 120);
  auto output = transaction.next(120);
  CHECK(output.step == ControlStep::Write && output.bytes[5] == 2,
        "a duplicate does not replace the first baseline or suppress its write");
  transaction.sent(output.step, 121);
  CHECK(transaction.request_id() == 2, "the sent write owns ID 2");
  output = transaction.next(301);
  transaction.sent(output.step, 301);
  frame.payload[8] = 4;
  set_request_id(frame, 7);
  transaction.observe(frame, 310);
  CHECK(transaction.phase() == ControlPhase::Verifying,
        "an unrelated ID does not confirm");
  set_request_id(frame, 2);
  transaction.observe(frame, 320);
  CHECK(transaction.phase() == ControlPhase::Applied, "the write's own ID confirms");

  tion4s::ControlTransaction by_read;
  frame.payload[8] = 3;
  set_request_id(frame, 1);
  CHECK(by_read.submit(ControlCommand::speed_step(5), snapshot, false, 400),
        "second write");
  by_read.sent(by_read.next(400).step, 400);
  by_read.observe(frame, 410);
  by_read.sent(by_read.next(410).step, 411);
  by_read.sent(by_read.next(591).step, 591);
  frame.payload[8] = 5;
  by_read.observe(frame, 600);
  CHECK(by_read.phase() == ControlPhase::Applied && by_read.request_id() == 2,
        "an answer to the verification read confirms as well");
}

void test_transaction_guards() {
  FrameParser parser;
  Frame frame;
  CHECK(feed(parser, hex(kStateResponse), frame), "baseline frame");
  const auto snapshot = snapshot_02d0(captured_state());
  tion4s::ControlTransaction admission;
  ControlCommand mixed = ControlCommand::power(true);
  mixed.sound_on = true;
  CHECK(!admission.submit(mixed, snapshot, true, 100) &&
            admission.failure() == ControlFailure::Refused &&
            admission.refusal() == WriteRejection::InvalidRequest,
        "a command changing two groups is refused at admission");
  auto no_state = snapshot;
  no_state.has_state = false;
  CHECK(!admission.submit(ControlCommand::power(false), no_state, true, 100) &&
            admission.failure() == ControlFailure::StaleState,
        "no reported state yet");
  auto stale = snapshot;
  stale.fresh = false;
  CHECK(!admission.submit(ControlCommand::power(false), stale, true, 100) &&
            admission.failure() == ControlFailure::StaleState,
        "a stale snapshot cannot admit a command");
  // The captured breezer runs step 3 on outdoor air with heat allowed.
  const tion4s::CommandContext context{3, 0, true};
  CHECK(!admission.submit(ControlCommand::speed_in_context(6, {4, 0, true}), snapshot,
                          true, 100) &&
            admission.failure() == ControlFailure::ContextChanged,
        "a context-bound command checks its context at admission");
  auto off = snapshot;
  off.state.power_on = false;
  CHECK(!admission.submit(ControlCommand::speed_in_context(6, context), off, true, 100) &&
            admission.failure() == ControlFailure::ContextChanged,
        "a context-bound command needs a running breezer");

  // The fresh pre-read checks the context again, field by field.
  for (const unsigned field : {6u, 4u}) {
    Frame changed = frame;
    if (field == 6) {
      changed.payload[6] = 1;  // Recirculation.
    } else {
      changed.payload[4] |= 0x10;  // Heat no longer allowed.
    }
    tion4s::ControlTransaction moved;
    CHECK(moved.submit(ControlCommand::speed_in_context(6, context), snapshot, true, 100),
          "context matches at admission");
    moved.sent(moved.next(100).step, 100);
    moved.observe(changed, 110);
    CHECK(moved.next(110).step == ControlStep::None &&
              moved.failure() == ControlFailure::ContextChanged,
          "a changed gate or heat permission cancels a context-bound write");
  }

  // Reported steps must belong to the current phase; others are ignored.
  tion4s::ControlTransaction steps;
  CHECK(steps.submit(ControlCommand::speed_step(4), snapshot, true, 200),
        "steps scenario");
  steps.sent(ControlStep::Write, 200);
  steps.sent(ControlStep::Verify, 200);
  steps.sent(ControlStep::None, 200);
  CHECK(steps.phase() == ControlPhase::Queued, "nothing was sent yet");
  steps.sent(steps.next(200).step, 200);
  steps.sent(ControlStep::Read, 205);
  steps.sent(ControlStep::Write, 205);
  steps.sent(ControlStep::Verify, 205);
  CHECK(steps.phase() == ControlPhase::Reading && steps.request_id() == 1,
        "no write without a baseline, no verification without a write");
  CHECK(steps.next(210).step == ControlStep::None, "still waiting for the baseline");

  // Verification ends by time as well as by count.
  const auto verifying = [&frame, &snapshot](tion4s::ControlTransaction& transaction) {
    CHECK(transaction.submit(ControlCommand::speed_step(4), snapshot, true, 1000),
          "verification scenario");
    transaction.sent(transaction.next(1000).step, 1000);
    transaction.observe(frame, 1010);
    transaction.sent(transaction.next(1010).step, 1011);
    transaction.sent(transaction.next(1191).step, 1191);
    CHECK(transaction.phase() == ControlPhase::Verifying, "verifying");
  };
  tion4s::ControlTransaction short_of_time;
  verifying(short_of_time);
  CHECK(short_of_time.next(4600).step == ControlStep::None &&
            short_of_time.failure() == ControlFailure::VerifyTimeout,
        "no verification read that could not finish before the intent expires");
  tion4s::ControlTransaction starved;
  verifying(starved);
  CHECK(starved.next(5001).step == ControlStep::None &&
            starved.failure() == ControlFailure::VerifyTimeout,
        "an intent that expires while verifying is a verification timeout");
}

void test_filter_reset_confirmation() {
  FrameParser parser;
  Frame frame;
  CHECK(feed(parser, hex(kStateResponse), frame), "filter response baseline");
  frame.payload[4] &= static_cast<std::uint8_t>(~0x01u);  // Power off.
  frame.payload[4] |= 0x10;                               // Heater permission off.
  frame.payload[4] |= 0x40;                               // Filter warning present.
  tion4s::State state;
  CHECK(tion4s::decode_state(frame, state), "decode filter baseline");
  const auto snapshot = snapshot_02d0(state);
  tion4s::ControlTransaction transaction;
  CHECK(transaction.submit(ControlCommand::filter_reset(), snapshot, true, 100),
        "filter reset enters only while off and heater disabled");
  transaction.sent(transaction.next(100).step, 100);
  transaction.observe(frame, 110);
  auto output = transaction.next(110);
  CHECK(output.step == ControlStep::Write && (output.bytes[9] & 0x80) != 0,
        "filter reset sends one explicit action bit");
  transaction.sent(output.step, 111);
  transaction.sent(transaction.next(291).step, 291);
  frame.payload[4] &= static_cast<std::uint8_t>(~0x40u);
  const auto drifted = state.filter_seconds - 1;
  for (unsigned i = 0; i < 4; ++i) {
    frame.payload[21 + i] = static_cast<std::uint8_t>(drifted >> (8 * i));
  }
  transaction.observe(frame, 300);
  CHECK(transaction.phase() == ControlPhase::Verifying,
        "small remaining-life drift is not confirmation");
  transaction.sent(transaction.next(792).step, 792);
  const auto set_life = [&frame](std::uint32_t seconds) {
    for (unsigned i = 0; i < 4; ++i) {
      frame.payload[21 + i] = static_cast<std::uint8_t>(seconds >> (8 * i));
    }
  };
  set_life(state.filter_seconds + 30);
  transaction.observe(frame, 795);
  CHECK(transaction.phase() == ControlPhase::Verifying,
        "an increase of less than a minute is not confirmation");
  set_life(state.filter_seconds + 100);
  frame.payload[4] |= 0x40;
  transaction.observe(frame, 797);
  CHECK(transaction.phase() == ControlPhase::Verifying,
        "a filter warning that stays on is not confirmation");
  frame.payload[4] &= static_cast<std::uint8_t>(~0x40u);
  transaction.observe(frame, 800);
  CHECK(transaction.phase() == ControlPhase::Applied,
        "warning cleared and substantial remaining-life increase confirms reset");
}

}  // namespace

int main() {
  test_golden_requests();
  test_device_info();
  test_stream_and_state();
  test_noise_crc_and_resync();
  test_session_and_clock_wrap();
  test_session_retries_and_ignores();
  test_state_never_fresh_again_after_wrap();
  test_fan_mapping();
  test_fan_encoding();
  test_relayed_bits();
  test_gate_and_settings_encoding();
  test_filter_reset_encoding();
  test_heater_encoding();
  test_commands();
  test_describe();
  test_device_policy();
  test_control_transaction_02d0();
  test_control_admission();
  test_unchanged_and_timeouts();
  test_transaction_request_ids();
  test_transaction_guards();
  test_filter_reset_confirmation();
  std::cout << "protocol and control transactions: PASS\n";
}
