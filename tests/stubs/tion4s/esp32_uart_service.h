#pragma once

#if defined(TION_TEST_REAL_ADAPTER)
#include "../../../libraries/Tion4SCore/src/tion4s/esp32_uart_service.h"
#else

// Host double of the ESP32-S3 adapter. The host include path finds it before
// libraries/Tion4SCore/src/tion4s/esp32_uart_service.h, so the unchanged
// sketch builds on the host. It keeps the adapter's public interface and its
// begin()/service_fallback() rules; the UART task is the real UartCore
// talking to a simulated breezer, run on every 10 ms of delay(). The real
// adapter (FreeRTOS task, UART driver, task watchdog, restart) is not built.

#include <Arduino.h>

#include "support/fake_tion.h"
#include "tion4s/uart_core.h"

#include <cstdint>

namespace tion4s {

struct PlatformStatus {
  bool task_running = false;
  std::uint32_t stack_free_bytes = 0;
  std::uint32_t stall_restarts = 0;
  bool stall_gave_up = false;
};

class Esp32UartService final : public ControlChannel {
 public:
  Esp32UartService() {
    instance_pointer() = this;
    fake::background = [this] {
      if (task_running_) {
        core.service(tion);
      }
    };
  }
  ~Esp32UartService() = default;
  Esp32UartService(const Esp32UartService&) = delete;
  Esp32UartService& operator=(const Esp32UartService&) = delete;
  Esp32UartService(Esp32UartService&&) = delete;
  Esp32UartService& operator=(Esp32UartService&&) = delete;

  // The sketch's single instance.
  static Esp32UartService& instance() { return *instance_pointer(); }

  bool begin() {
    ++begin_calls;
    if (!begun_) {
      begun_ = true;
      uart_ready_ = uart_opens;
      task_running_ = uart_ready_ && task_starts;
    }
    return task_running_;
  }

  void service_fallback() {
    if (!task_running_ && uart_ready_) {
      core.service(tion);
    }
  }

  void supervise() { ++supervise_calls; }

  UartStatus status() const override { return core.status(tion); }

  PlatformStatus platform_status() const {
    PlatformStatus result;
    result.task_running = task_running_;
    result.stack_free_bytes = 1234;
    result.stall_restarts = stall_restarts;
    result.stall_gave_up = stall_gave_up;
    return result;
  }

  std::uint32_t submit(const ControlCommand& command, bool allow_legacy_02d0,
                       std::uint32_t expected_failures) override {
    ++submitted;
    return core.submit(command, allow_legacy_02d0, expected_failures, millis());
  }

  // Set before setup().
  bool uart_opens = true;
  bool task_starts = true;
  std::uint32_t stall_restarts = 0;
  bool stall_gave_up = false;

  unsigned begin_calls = 0;
  unsigned supervise_calls = 0;
  unsigned submitted = 0;
  fake::FakeTion tion{fake_millis};
  fake::NoLock lock;
  UartCore core{lock};

 private:
  static Esp32UartService*& instance_pointer() {
    static Esp32UartService* pointer = nullptr;
    return pointer;
  }

  bool begun_ = false;
  bool uart_ready_ = false;
  bool task_running_ = false;
};

}  // namespace tion4s

#endif
