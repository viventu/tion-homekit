#pragma once

// ESP32-S3 adapter. Only sketches that include this header need the S3 and
// the free GPIO19/20; the portable part of Tion4SCore builds anywhere.
#include <sdkconfig.h>

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "Esp32UartService targets ESP32-S3 only."
#endif

#if ARDUINO_USB_CDC_ON_BOOT
#error "USB CDC on boot conflicts with the Tion UART on GPIO19/20."
#endif

#include "stall_guard.h"
#include "uart_core.h"

#include <Arduino.h>
#include <HardwareSerial.h>
#include <atomic>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace tion4s {

// Adapter health that has no meaning on the host.
struct PlatformStatus {
  bool task_running = false;
  std::uint32_t stack_free_bytes = 0;
  std::uint32_t stall_restarts = 0;
  bool stall_gave_up = false;
};

// ESP32-S3 owner of the Tion UART: one FreeRTOS task runs UartCore, which is
// portable and host-tested. This adapter adds the serial port, the lock, the
// task watchdog and a bounded restart when heartbeats stop leaving the UART.
class Esp32UartService final : public ControlChannel {
 public:
  Esp32UartService() = default;
  ~Esp32UartService() = default;
  Esp32UartService(const Esp32UartService&) = delete;
  Esp32UartService& operator=(const Esp32UartService&) = delete;
  Esp32UartService(Esp32UartService&&) = delete;
  Esp32UartService& operator=(Esp32UartService&&) = delete;

  // Starts UART before HomeSpan polling/provisioning. After three failed
  // recovery attempts, starts neither UART nor its task until a power cycle.
  // On task failure, run
  // service_fallback() frequently from its own loop. Later calls only
  // report the result of the first one.
  bool begin();
  void service_fallback();
  // Call from the sketch loop. Restarts the chip after a heartbeat stall, at
  // most StallGuard::kMaxConsecutiveRestarts times in a row.
  void supervise();
  UartStatus status() const override;
  PlatformStatus platform_status() const;
  std::uint32_t submit(const ControlCommand& command, bool allow_legacy_02d0,
                       std::uint32_t expected_failures) override;

 private:
  static constexpr uart_port_t kUartPort = UART_NUM_1;

  class Port final : public UartPort {
   public:
    explicit Port(HardwareSerial& serial) : serial_(serial) {}
    int read_byte() override;
    bool write_all(const std::uint8_t* bytes, std::size_t size) override;
    std::uint32_t now_ms() const override { return millis(); }

   private:
    HardwareSerial& serial_;
  };

  class Lock final : public StatusLock {
   public:
    void lock() override { portENTER_CRITICAL(&mux_); }
    void unlock() override { portEXIT_CRITICAL(&mux_); }

   private:
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  };

  static void task_entry(void* argument);

  HardwareSerial uart_{static_cast<std::uint8_t>(kUartPort)};
  Port port_{uart_};
  Lock lock_;
  UartCore core_{lock_};
  StallGuard stall_guard_;
  std::atomic<bool> task_running_{false};
  std::atomic<std::uint32_t> stack_free_bytes_{0};
  std::uint32_t started_ms_ = 0;
  bool begun_ = false;
  bool uart_ready_ = false;
  bool recovery_locked_ = false;
};

}  // namespace tion4s
