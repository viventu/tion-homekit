// Arduino compiles every source of a library. On other targets, or with USB
// CDC on boot, this adapter compiles to nothing so the portable part of the
// library stays usable; its header reports the conflict to sketches using it.
#include <sdkconfig.h>

#if defined(CONFIG_IDF_TARGET_ESP32S3) && !ARDUINO_USB_CDC_ON_BOOT

#include "esp32_uart_service.h"

#include "esp_attr.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "soc/soc.h"
#include "soc/usb_serial_jtag_reg.h"

namespace tion4s {
namespace {

constexpr int kUartRxPin = 19;
constexpr int kUartTxPin = 20;
constexpr unsigned long kUartBaud = 9600;
constexpr std::uint32_t kTaskStackBytes = 4096;
constexpr UBaseType_t kTaskPriority = 2;
constexpr TickType_t kTxDoneTimeout = pdMS_TO_TICKS(100);
constexpr unsigned kStackSampleInterval = 100;  // About once per second.
constexpr std::uint32_t kStallMagic = 0x54494F4E;

// Survive esp_restart() but not power loss; validated by magic and reason.
RTC_NOINIT_ATTR std::uint32_t stall_magic;
RTC_NOINIT_ATTR std::uint32_t stall_restarts;

}  // namespace

int Esp32UartService::Port::read_byte() {
  return serial_.available() > 0 ? serial_.read() : -1;
}

bool Esp32UartService::Port::write_all(const std::uint8_t* bytes, std::size_t size) {
  const int written = uart_write_bytes(kUartPort, bytes, size);
  return written >= 0 && static_cast<std::size_t>(written) == size &&
         uart_wait_tx_done(kUartPort, kTxDoneTimeout) == ESP_OK;
}

bool Esp32UartService::begin() {
  if (begun_) {
    return task_running_.load();
  }
  begun_ = true;
  const auto reason = esp_reset_reason();
  if (reason == ESP_RST_POWERON || stall_magic != kStallMagic) {
    stall_magic = kStallMagic;
    stall_restarts = 0;
  } else if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT ||
             reason == ESP_RST_WDT || reason == ESP_RST_PANIC) {
    // A watchdog reset bypasses supervise(). Account for it on the next boot.
    if (stall_restarts < StallGuard::kMaxConsecutiveRestarts) {
      ++stall_restarts;
    }
  }
  recovery_locked_ = stall_restarts >= StallGuard::kMaxConsecutiveRestarts;
  if (recovery_locked_) {
    // Do not enter a potentially hanging driver or subscribe another task to
    // TWDT. The sketch keeps HAP off and shows a fault. Power cycle to retry.
    return false;
  }
  // GPIO19/20 are also USB D-/D+. IDF 5.5 names this register differently
  // from the older esp32_usb_dis component used by the Tion upstream.
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
  uart_.begin(kUartBaud, SERIAL_8N1, kUartRxPin, kUartTxPin);
  uart_ready_ = static_cast<bool>(uart_);
  started_ms_ = millis();
  if (!uart_ready_) {
    return false;
  }

  const bool started = xTaskCreate(task_entry, "tion-uart", kTaskStackBytes, this,
                                   kTaskPriority, nullptr) == pdPASS;
  task_running_.store(started);
  return started;
}

void Esp32UartService::service_fallback() {
  if (!task_running_.load() && uart_ready_) {
    core_.service(port_);
  }
}

void Esp32UartService::task_entry(void* argument) {
  auto* service = static_cast<Esp32UartService*>(argument);
  // A hung pass stops watchdog resets; the TWDT then reboots the chip.
  const bool watched = esp_task_wdt_add(nullptr) == ESP_OK;
  for (unsigned pass = 0;; ++pass) {
    service->core_.service(service->port_);
    if (watched) {
      esp_task_wdt_reset();
    }
    if (pass % kStackSampleInterval == 0) {
      service->stack_free_bytes_.store(
          static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(nullptr)));
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void Esp32UartService::supervise() {
  if (!uart_ready_) {
    return;
  }
  const auto current = core_.status(port_);
  const auto now = millis();
  // TX failures keep the task alive but stop heartbeats; so does a stuck task.
  const auto heartbeat_age =
      current.device.heartbeat_sent ? current.device.heartbeat_age_ms : now - started_ms_;
  if (stall_guard_.evaluate(heartbeat_age, now, stall_restarts) ==
      StallGuard::Action::Restart) {
    esp_restart();
  }
}

UartStatus Esp32UartService::status() const { return core_.status(port_); }

PlatformStatus Esp32UartService::platform_status() const {
  PlatformStatus result;
  result.task_running = task_running_.load();
  result.stack_free_bytes = stack_free_bytes_.load();
  result.stall_restarts = stall_restarts;
  result.stall_gave_up = recovery_locked_;
  return result;
}

std::uint32_t Esp32UartService::submit(const ControlCommand& command,
                                       bool allow_legacy_02d0,
                                       std::uint32_t expected_failures) {
  return core_.submit(command, allow_legacy_02d0, expected_failures, millis());
}

}  // namespace tion4s

#endif  // CONFIG_IDF_TARGET_ESP32S3 && !ARDUINO_USB_CDC_ON_BOOT
