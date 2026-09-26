#include <Arduino.h>
#include <HomeSpan.h>
#include <Tion4SCore.h>
#include <tion4s/esp32_uart_service.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Apa102StatusLed.h"
#include "HomeKitControls.h"
#include "PairingBootstrap.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"

namespace {

#if defined(TION_ENABLE_WEB_DIAGNOSTICS)
constexpr char kVersion[] = "0.2.0-gm1-diag";
#else
constexpr char kVersion[] = "0.2.0-gm1";
#endif
tion4s::Esp32UartService tion_uart;
tion_homekit::ControlAccess control_access(tion_uart);
Apa102StatusLed status_led;
char serial_number[24];
char ap_ssid[32];
char host_name[32];
bool uart_task_started = false;
bool pairing_code_ready = false;
// HomeSpan resets its watchdog at the end of every poll() and inside its own
// blocking loops (Setup AP). The margin covers pair-setup SRP computations.
constexpr std::uint16_t kPollWatchdogSeconds = 10;

tion_homekit::FanService* fan = nullptr;
tion_homekit::AuxiliarySwitch* recirculation = nullptr;
tion_homekit::AuxiliarySwitch* sound = nullptr;
tion_homekit::AuxiliarySwitch* led = nullptr;
tion_homekit::HeaterService* heater = nullptr;
tion_homekit::TemperatureService* intake_temperature = nullptr;
tion_homekit::TemperatureService* outlet_temperature = nullptr;
tion_homekit::FilterService* filter = nullptr;
tion_homekit::FilterResetSwitch* filter_reset = nullptr;
tion_homekit::BoostSwitch* boost = nullptr;
std::uint32_t displayed_state_count = 0;
bool displayed_fresh = false;

#if defined(TION_ENABLE_WEB_DIAGNOSTICS)
// One table row per call keeps every value bounded by its own small buffer,
// so a new row can never silently truncate the rest of the page. The last
// byte is never written, so the value stays terminated even if formatting
// fails. C varargs keep the compiler's printf format checks.
// NOLINTNEXTLINE(cert-dcl50-cpp)
__attribute__((format(printf, 3, 4))) void append_row(String& html, const char* label,
                                                      const char* format, ...) {
  char value[64] = {};
  va_list arguments;
  va_start(arguments, format);
  static_cast<void>(vsnprintf(value, sizeof(value) - 1, format, arguments));
  va_end(arguments);
  html += "<tr><td>";
  html += label;
  html += ":</td><td>";
  html += value;
  html += "</td></tr>";
}

void append_uart_diagnostics(String& html) {
  const auto status = tion_uart.status();
  const auto platform = tion_uart.platform_status();
  const auto& device = status.device;
  html.reserve(html.length() + 3072);
  append_row(html, "Firmware version", "%s", kVersion);
  append_row(html, "Uptime (s)", "%llu",
             static_cast<unsigned long long>(esp_timer_get_time() / 1000000));
  append_row(html, "Free heap (bytes)", "%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
  append_row(html, "Minimum free heap (bytes)", "%u",
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)));
  append_row(html, "Largest free block (bytes)", "%u",
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  append_row(html, "UART task", "%u", static_cast<unsigned>(platform.task_running));
  append_row(html, "UART stack free (bytes)", "%lu",
             static_cast<unsigned long>(platform.stack_free_bytes));
  append_row(html, "UART stall restarts", "%lu / %s",
             static_cast<unsigned long>(platform.stall_restarts),
             platform.stall_gave_up ? "limit reached" : "armed");
  append_row(html, "First heartbeat (ms)", "%lu",
             static_cast<unsigned long>(status.first_heartbeat_after_boot_ms));
  append_row(html, "Heartbeat responses", "%lu",
             static_cast<unsigned long>(device.heartbeat_count));
  append_row(html, "Heartbeat TX age (ms)", "%lu",
             static_cast<unsigned long>(device.heartbeat_age_ms));
  append_row(html, "Maximum heartbeat TX gap (ms)", "%lu",
             static_cast<unsigned long>(device.max_heartbeat_gap_ms));
  append_row(html, "Valid states", "%lu", static_cast<unsigned long>(device.state_count));
  append_row(html, "Pending state replies", "%lu",
             static_cast<unsigned long>(status.state_reads_pending));
  append_row(html, "State age (ms)", "%lu", static_cast<unsigned long>(device.age_ms));
  append_row(html, "State fresh", "%u", static_cast<unsigned>(device.fresh));
  append_row(html, "Rejected frames", "%lu", static_cast<unsigned long>(status.rejected_frames));
  append_row(html, "UART TX failures", "%lu", static_cast<unsigned long>(status.tx_failures));

  append_row(html, "Control available", "%u",
             static_cast<unsigned>(control_access.available(status)));
  append_row(html, "Control phase / failure", "%s / %s",
             tion4s::describe(status.control_phase),
             tion4s::describe(status.control_failure));
  append_row(html, "Control refusal", "%s", tion4s::describe(status.control_refusal));
  append_row(html, "Last control kind / request ID", "%s / %lu",
             tion4s::describe(status.control_command),
             static_cast<unsigned long>(status.control_request_id));
  append_row(html, "Control applied / failed", "%lu / %lu",
             static_cast<unsigned long>(status.control_applied),
             static_cast<unsigned long>(status.control_failed));
  append_row(html, "Control unchanged", "%lu",
             static_cast<unsigned long>(status.control_unchanged));

  if (device.has_device_info) {
    const auto& info = device.device_info;
    append_row(html, "Device type", "0x%08lX", static_cast<unsigned long>(info.device_type));
    append_row(html, "Device work mode", "%u", static_cast<unsigned>(info.work_mode));
    append_row(html, "Tion firmware", "%04X", static_cast<unsigned>(info.firmware_version));
    append_row(html, "Tion hardware", "%04X", static_cast<unsigned>(info.hardware_version));
  }
  if (device.has_state) {
    const auto& state = device.state;
    append_row(html, "Raw request ID", "%lu", static_cast<unsigned long>(state.request_id));
    append_row(html, "Power", "%u", static_cast<unsigned>(state.power_on));
    append_row(html, "Fan speed / maximum", "%u / %u", static_cast<unsigned>(state.fan_speed),
               static_cast<unsigned>(state.max_fan_speed));
    append_row(html, "Sound enabled", "%u", static_cast<unsigned>(state.sound_on));
    append_row(html, "LED enabled", "%u", static_cast<unsigned>(state.led_on));
    append_row(html, "Gate position (0=outdoor, 1=recirculation)", "%u",
               static_cast<unsigned>(state.gate_position));
    append_row(html, "Filter warning", "%u", static_cast<unsigned>(state.filter_warning));
    append_row(html, "Heater mode allows / raw bit", "%u / %u",
               static_cast<unsigned>(state.heater_allowed),
               static_cast<unsigned>(state.heater_state_bit));
    append_row(html, "Raw heater percentage", "%u", static_cast<unsigned>(state.heater_percent));
    append_row(html, "Target temperature (C)", "%d", static_cast<int>(state.target_temperature));
    append_row(html, "Outdoor temperature (C)", "%d",
               static_cast<int>(state.outdoor_temperature));
    append_row(html, "Raw current temperature (C)", "%d",
               static_cast<int>(state.current_temperature));
    append_row(html, "Raw filter counter", "%lu", static_cast<unsigned long>(state.filter_seconds));
    append_row(html, "Raw errors", "%lu", static_cast<unsigned long>(state.errors));
  }
}
#endif

void make_names() {
  std::uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  // Fixed-length results that always fit their buffers.
  static_cast<void>(snprintf(serial_number, sizeof(serial_number),
                             "TION4S-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2],
                             mac[3], mac[4], mac[5]));
  static_cast<void>(
      snprintf(ap_ssid, sizeof(ap_ssid), "Tion-Setup-%02X%02X%02X", mac[3], mac[4], mac[5]));
  static_cast<void>(
      snprintf(host_name, sizeof(host_name), "tion-4s-%02x%02x%02x", mac[3], mac[4], mac[5]));
}

}  // namespace

void setup() {
  status_led.begin();
  uart_task_started = tion_uart.begin();
  if (!uart_task_started) {
    // Degraded: the loop keeps the heartbeat, HomeKit stays off.
    status_led.fault();
    return;
  }

  make_names();
  homeSpan.setStatusDevice(&status_led);
  homeSpan.setControlPin(0);
  homeSpan.setSerialInputDisable(true);
  homeSpan.setLogLevel(-1);
  homeSpan.setApSSID(ap_ssid);
  homeSpan.setApTimeout(300);
  homeSpan.enableAutoStartAP();
  homeSpan.setSketchVersion(kVersion);
#if defined(TION_ENABLE_WEB_DIAGNOSTICS)
  homeSpan.enableWebLog(0, nullptr, "UTC", "tion-status");
  homeSpan.setWebLogCallback(append_uart_diagnostics);
#endif
  homeSpan.begin(Category::Fans, "Tion 4S", host_name, "Tion 4S bridge");

  pairing_code_ready = tion_homekit::ensure_private_pairing_code();
  if (!pairing_code_ready) {
    // Fail closed: without a private verifier HAP would fall back to the
    // shared default code. The UART task keeps the heartbeat.
    status_led.fault();
    return;
  }

  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Name("Tion 4S");
  new Characteristic::Manufacturer("DIY");
  new Characteristic::Model("T-Dongle-S3 / Tion 4S");
  new Characteristic::SerialNumber(serial_number);
  new Characteristic::FirmwareRevision(kVersion);
  fan = new tion_homekit::FanService(control_access);
  // Retire the old control gate (IIDs 13-15) without reusing its identifiers.
  // Existing tiles and controller automations keep their original IIDs.
  homeSpan.resetIID(16);
  recirculation = new tion_homekit::AuxiliarySwitch(
      control_access, tion_homekit::kRecirculation, "Рециркуляция");
  sound = new tion_homekit::AuxiliarySwitch(
      control_access, tion_homekit::kSound, "Звук Tion");
  led = new tion_homekit::AuxiliarySwitch(
      control_access, tion_homekit::kLed, "Подсветка Tion");
  heater = new tion_homekit::HeaterService(control_access);
  intake_temperature = new tion_homekit::TemperatureService("Температура притока");
  outlet_temperature = new tion_homekit::TemperatureService("Температура выхода");
  filter = new tion_homekit::FilterService(control_access);
  filter_reset = new tion_homekit::FilterResetSwitch(control_access);
  boost = new tion_homekit::BoostSwitch(control_access);
  homeSpan.enableWatchdog(kPollWatchdogSeconds);
}

void loop() {
  if (!uart_task_started) {
    tion_uart.service_fallback();
    tion_uart.supervise();
    delay(10);
    return;
  }
  tion_uart.supervise();
  if (!pairing_code_ready) {
    delay(1000);
    return;
  }

  homeSpan.poll();
  // One status copy per pass feeds every refresh below.
  const auto status = tion_uart.status();
  const auto& snapshot = status.device;
  if (snapshot.state_count != displayed_state_count || snapshot.fresh != displayed_fresh) {
    fan->refresh(snapshot);
    recirculation->refresh(snapshot);
    sound->refresh(snapshot);
    led->refresh(snapshot);
    heater->refresh(snapshot);
    intake_temperature->refresh(snapshot.fresh, snapshot.state.outdoor_temperature);
    outlet_temperature->refresh(snapshot.fresh, snapshot.state.current_temperature);
    filter->refresh(snapshot);
    displayed_state_count = snapshot.state_count;
    displayed_fresh = snapshot.fresh;
  }
  filter_reset->refresh(status);
  boost->refresh(status);
}
