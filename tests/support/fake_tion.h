#pragma once

// Host-only simulated Tion 4S behind a 9600-baud line. It answers heartbeat,
// state and device-info requests and applies state writes like the device
// observed on 02D0. It never talks to real hardware.

#include "tion4s/crc.h"
#include "tion4s/protocol.h"
#include "tion4s/uart_core.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <utility>
#include <vector>

namespace fake {

class NoLock final : public tion4s::StatusLock {
 public:
  void lock() override { ++depth; }
  void unlock() override { --depth; }
  int depth = 0;
};

class FakeTion final : public tion4s::UartPort {
 public:
  explicit FakeTion(std::uint32_t& clock) : clock_(clock) {
    state.power_on = true;
    state.sound_on = true;
    state.led_on = true;
    state.heater_allowed = false;
    state.heater_present = 1;
    state.gate_position = 0;
    state.target_temperature = 20;
    state.fan_speed = 3;
    state.max_fan_speed = 6;
    state.outdoor_temperature = -12;
    state.current_temperature = 18;
    state.filter_seconds = 100000;
  }

  // UartPort ---------------------------------------------------------------
  int read_byte() override {
    if (inbound_.empty() || inbound_.front().first > clock_) {
      return -1;
    }
    const auto value = inbound_.front().second;
    inbound_.pop_front();
    return value;
  }

  bool write_all(const std::uint8_t* bytes, std::size_t size) override {
    // Another task may run while the UART task blocks in a write.
    if (on_write) {
      on_write();
    }
    if (fail_writes) {
      clock_ += 100;  // uart_wait_tx_done timeout
      return false;
    }
    clock_ += static_cast<std::uint32_t>((size * 10 * 1000 + 9599) / 9600);
    handle(std::vector<std::uint8_t>(bytes, bytes + size));
    return true;
  }

  std::uint32_t now_ms() const override {
    if (on_clock) {
      on_clock(clock_);
    }
    return clock_;
  }

  // Simulation -------------------------------------------------------------
  // Queues raw bytes as if they arrived on the line now.
  void inject(const std::vector<std::uint8_t>& bytes) {
    for (const auto byte : bytes) {
      inbound_.emplace_back(clock_, byte);
    }
  }

  static std::vector<std::uint8_t> frame(std::uint16_t type,
                                         const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> bytes{0x3A, static_cast<std::uint8_t>(payload.size() + 7),
                                    0, static_cast<std::uint8_t>(type),
                                    static_cast<std::uint8_t>(type >> 8)};
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    const auto crc = tion4s::crc16_ccitt_false(bytes.data(), bytes.size());
    bytes.push_back(static_cast<std::uint8_t>(crc >> 8));
    bytes.push_back(static_cast<std::uint8_t>(crc));
    return bytes;
  }

  std::vector<std::uint8_t> state_frame(std::uint32_t request_id) const {
    std::vector<std::uint8_t> p(35, 0);
    for (unsigned i = 0; i < 4; ++i) {
      p[i] = static_cast<std::uint8_t>(request_id >> (8 * i));
    }
    p[4] = static_cast<std::uint8_t>(
        (state.power_on ? 0x01 : 0) | (state.sound_on ? 0x02 : 0) |
        (state.led_on ? 0x04 : 0) | (state.heater_allowed ? 0 : 0x10) |
        (state.filter_warning ? 0x40 : 0) | ((state.heater_present & 1) << 7));
    p[5] = static_cast<std::uint8_t>((state.heater_present >> 1) & 0x03);
    p[6] = state.gate_position;
    p[7] = static_cast<std::uint8_t>(state.target_temperature);
    p[8] = state.fan_speed;
    p[9] = static_cast<std::uint8_t>(state.outdoor_temperature);
    p[10] = static_cast<std::uint8_t>(state.current_temperature);
    for (unsigned i = 0; i < 4; ++i) {
      p[21 + i] = static_cast<std::uint8_t>(state.filter_seconds >> (8 * i));
      p[29 + i] = static_cast<std::uint8_t>(state.errors >> (8 * i));
    }
    p[33] = state.max_fan_speed;
    p[34] = state.heater_percent;
    return frame(0x3231, p);
  }

  tion4s::State state{};
  std::uint16_t firmware = 0x02D0;
  std::uint32_t device_type = 0x8003;
  std::uint8_t work_mode = 1;
  bool answer_device_info = true;
  // 02D0 answers reads with IDs unrelated to any request. Upstream firmware
  // (02D2 and later) answers reads with ID 1 and a write with its own ID.
  std::uint32_t response_request_id = 0;
  bool echo_request_ids = false;
  std::uint32_t response_delay_ms = 20;
  bool fail_writes = false;
  bool respond = true;
  std::function<void()> on_write;
  std::function<void(std::uint32_t)> on_clock;
  bool apply_writes = true;

  unsigned heartbeats = 0;
  unsigned state_requests = 0;
  unsigned device_info_requests = 0;
  unsigned writes = 0;
  std::uint32_t max_heartbeat_gap_ms = 0;
  std::vector<std::uint8_t> last_write;

 private:
  void handle(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 7 || bytes[0] != 0x3A ||
        tion4s::crc16_ccitt_false(bytes.data(), bytes.size()) != 0) {
      return;
    }
    const auto type = static_cast<std::uint16_t>(bytes[3] | (bytes[4] << 8));
    if (type == 0x3932) {
      if (heartbeats != 0 && clock_ - last_heartbeat_ms_ > max_heartbeat_gap_ms) {
        max_heartbeat_gap_ms = clock_ - last_heartbeat_ms_;
      }
      last_heartbeat_ms_ = clock_;
      ++heartbeats;
      reply(frame(0x3931, {0}));
    } else if (type == 0x3232) {
      ++state_requests;
      reply(state_frame(echo_request_ids ? 1 : response_request_id));
    } else if (type == 0x3332) {
      ++device_info_requests;
      if (!answer_device_info) {
        return;
      }
      std::vector<std::uint8_t> p(25, 0);
      p[0] = work_mode;
      for (unsigned i = 0; i < 4; ++i) {
        p[1 + i] = static_cast<std::uint8_t>(device_type >> (8 * i));
      }
      p[5] = static_cast<std::uint8_t>(firmware);
      p[6] = static_cast<std::uint8_t>(firmware >> 8);
      reply(frame(0x3331, p));
    } else if ((type == 0x3230 || type == 0x3234) && bytes.size() == 18) {
      ++writes;
      last_write = bytes;
      if (apply_writes) {
        const auto flags = bytes[9];
        state.power_on = flags & 0x01;
        state.sound_on = flags & 0x02;
        state.led_on = flags & 0x04;
        state.heater_allowed = (flags & 0x08) == 0;
        if (flags & 0x80) {
          state.filter_seconds += 10'000'000;
          state.filter_warning = false;
        }
        state.gate_position = bytes[11];
        state.target_temperature = static_cast<std::int8_t>(bytes[12]);
        state.fan_speed = bytes[13];
      }
      if (echo_request_ids) {
        std::uint32_t request_id = 0;
        for (unsigned i = 0; i < 4; ++i) {
          request_id |= std::uint32_t{bytes[5 + i]} << (8 * i);
        }
        reply(state_frame(request_id));
      }
    }
  }

  void reply(const std::vector<std::uint8_t>& bytes) {
    if (!respond) {
      return;
    }
    for (const auto byte : bytes) {
      inbound_.emplace_back(clock_ + response_delay_ms, byte);
    }
  }

  std::uint32_t& clock_;
  std::uint32_t last_heartbeat_ms_ = 0;
  std::deque<std::pair<std::uint32_t, std::uint8_t>> inbound_;
};

// Runs the UART task loop: one service pass, then the 10 ms task delay.
inline void run(tion4s::UartCore& core, FakeTion& tion, std::uint32_t& clock,
                std::uint32_t duration_ms) {
  const auto end = clock + duration_ms;
  while (static_cast<std::int32_t>(clock - end) < 0) {
    core.service(tion);
    clock += 10;
  }
}

}  // namespace fake
