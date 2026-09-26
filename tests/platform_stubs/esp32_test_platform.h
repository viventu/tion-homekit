#pragma once

// Driver/RTOS boundary for the real ESP32 adapter. One actual worker thread
// executes its unchanged task_entry(). Tests advance it one complete pass at
// a time; a condition variable makes device state safe to inspect between
// passes. The worker exits only at the fake RTOS delay when a boot ends.
#include <Arduino.h>
#include <esp_err.h>
#include "support/fake_tion.h"
#include "support/check.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <pthread.h>
#include <thread>

using TickType_t = std::uint32_t;
using UBaseType_t = unsigned;
using BaseType_t = int;
using TaskFunction_t = void (*)(void*);
using TaskHandle_t = void*;
using uart_port_t = int;
using portMUX_TYPE = std::mutex;
constexpr uart_port_t UART_NUM_1 = 1;
constexpr BaseType_t pdPASS = 1;
constexpr std::uint32_t SERIAL_8N1 = 0x800001C;
constexpr unsigned USB_SERIAL_JTAG_CONF0_REG = 1;
constexpr unsigned USB_SERIAL_JTAG_USB_PAD_ENABLE = 1;
#define portMUX_INITIALIZER_UNLOCKED {}
#define pdMS_TO_TICKS(ms) (ms)
#define RTC_NOINIT_ATTR

// Keep the native ESP-IDF enum representation at the driver boundary.
// NOLINTNEXTLINE(performance-enum-size)
enum esp_reset_reason_t {
  ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_SW, ESP_RST_PANIC,
  ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_BROWNOUT,
};

namespace fake {

class EspPlatform {
 public:
  EspPlatform() = default;
  ~EspPlatform() { stop_task(); }
  EspPlatform(const EspPlatform&) = delete;
  EspPlatform& operator=(const EspPlatform&) = delete;
  EspPlatform(EspPlatform&&) = delete;
  EspPlatform& operator=(EspPlatform&&) = delete;

  bool launch(TaskFunction_t entry, void* argument) {
    ++task_creations;
    if (!task_starts) {
      return false;
    }
    stopping_ = false;
    waiting_ = false;
    worker_ = std::thread([this, entry, argument] {
      park();
      entry(argument);
    });
    std::unique_lock<std::mutex> guard(scheduler_);
    CHECK(changed_.wait_for(guard, std::chrono::seconds(5), [this] { return waiting_; }),
          "RTOS worker starts within wall-clock deadline");
    return true;
  }

  void park() {
    std::unique_lock<std::mutex> guard(scheduler_);
    waiting_ = true;
    changed_.notify_all();
    changed_.wait(guard, [this] { return run_pass_ || stopping_; });
    if (stopping_) {
      guard.unlock();
      pthread_exit(nullptr);
    }
    run_pass_ = false;
  }

  void pump() {
    if (!worker_.joinable()) {
      return;
    }
    std::unique_lock<std::mutex> guard(scheduler_);
    waiting_ = false;
    run_pass_ = true;
    changed_.notify_all();
    CHECK(changed_.wait_for(guard, std::chrono::seconds(5), [this] { return waiting_; }),
          "UART task pass reaches its RTOS delay within wall-clock deadline");
  }

  void stop_task() {
    if (worker_.joinable()) {
      {
        const std::lock_guard<std::mutex> guard(scheduler_);
        stopping_ = true;
        changed_.notify_all();
      }
      worker_.join();
    }
    watchdog_added = false;
  }

  bool watchdog_expired() {
    if (watchdog_added && fake_millis - watchdog_fed_at >= watchdog_timeout_ms) {
      ++watchdog_resets;
      reset_reason = ESP_RST_TASK_WDT;
      return true;
    }
    return false;
  }

  FakeTion tion{fake_millis};
  bool uart_opens = true;
  bool task_starts = true;
  bool watchdog_add_succeeds = true;
  bool watchdog_added = false;
  esp_err_t tx_done_result = ESP_OK;
  // -2: pass to device; -1: driver error; nonnegative: partial/full count.
  int tx_write_result = -2;
  esp_reset_reason_t reset_reason = ESP_RST_POWERON;
  unsigned uart_begins = 0;
  unsigned task_creations = 0;
  unsigned software_resets = 0;
  unsigned watchdog_resets = 0;
  unsigned watchdog_feeds = 0;
  unsigned stack_samples = 0;
  unsigned usb_disables = 0;
  std::uint32_t watchdog_fed_at = 0;
  std::uint32_t watchdog_timeout_ms = 5000;
  std::function<void()> before_lock;

 private:
  std::thread worker_;
  std::mutex scheduler_;
  std::condition_variable changed_;
  bool waiting_ = false;
  bool run_pass_ = false;
  bool stopping_ = false;
};

inline EspPlatform& esp() {
  static EspPlatform platform;
  return platform;
}

}  // namespace fake

inline void portENTER_CRITICAL(portMUX_TYPE* mutex) {
  if (fake::esp().before_lock) {
    auto hook = std::move(fake::esp().before_lock);
    fake::esp().before_lock = nullptr;
    hook();
  }
  mutex->lock();
}
inline void portEXIT_CRITICAL(portMUX_TYPE* mutex) { mutex->unlock(); }
inline void CLEAR_PERI_REG_MASK(unsigned, unsigned) { ++fake::esp().usb_disables; }
inline esp_reset_reason_t esp_reset_reason() { return fake::esp().reset_reason; }
inline void esp_restart() {
  ++fake::esp().software_resets;
  fake::esp().reset_reason = ESP_RST_SW;
}
inline BaseType_t xTaskCreate(TaskFunction_t entry, const char*, std::uint32_t,
                              void* argument, UBaseType_t, TaskHandle_t*) {
  return fake::esp().launch(entry, argument) ? pdPASS : 0;
}
inline void vTaskDelay(TickType_t) { fake::esp().park(); }
inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) {
  ++fake::esp().stack_samples;
  return 1234;
}
inline esp_err_t esp_task_wdt_add(TaskHandle_t) {
  fake::esp().watchdog_added = fake::esp().watchdog_add_succeeds;
  fake::esp().watchdog_fed_at = fake_millis;
  return fake::esp().watchdog_added ? ESP_OK : ESP_FAIL;
}
inline esp_err_t esp_task_wdt_reset() {
  fake::esp().watchdog_fed_at = fake_millis;
  ++fake::esp().watchdog_feeds;
  return ESP_OK;
}
inline int uart_write_bytes(uart_port_t, const void* bytes, std::size_t size) {
  if (fake::esp().tx_write_result != -2) {
    return fake::esp().tx_write_result;
  }
  return fake::esp().tion.write_all(static_cast<const std::uint8_t*>(bytes), size)
             ? static_cast<int>(size) : -1;
}
inline esp_err_t uart_wait_tx_done(uart_port_t, TickType_t) {
  return fake::esp().tx_done_result;
}

class HardwareSerial {
 public:
  explicit HardwareSerial(std::uint8_t) {}
  void begin(unsigned long, std::uint32_t, int, int) {
    ++fake::esp().uart_begins;
    ready_ = fake::esp().uart_opens;
  }
  explicit operator bool() const { return ready_; }
  int available() {
    pending_ = fake::esp().tion.read_byte();
    return pending_ >= 0 ? 1 : 0;
  }
  int read() { return pending_; }

 private:
  bool ready_ = false;
  int pending_ = -1;
};
