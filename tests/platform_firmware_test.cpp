// Full HomeKit sketch -> real ESP32 adapter/task -> driver doubles -> device.
// HAP and physical peripherals are the only simulated boundaries.
#include "../firmware/TionHomeKit/TionHomeKit.ino"
#include "esp32_test_platform.h"
#include "support/scenarios.h"

namespace {
SpanCharacteristic* tile(const char* service_name, const char* characteristic_name) {
  for (auto* service : homeSpan.services) {
    bool matches = false;
    for (auto* characteristic : service->characteristics) {
      if (std::strcmp(characteristic->type, "ConfiguredName") == 0 &&
          characteristic->text == service_name) matches = true;
    }
    if (matches) {
      for (auto* characteristic : service->characteristics) {
        if (std::strcmp(characteristic->type, characteristic_name) == 0) return characteristic;
      }
    }
  }
  CHECK(false, "characteristic exists");
  return nullptr;
}
void run_platform(std::uint32_t ms) {
  const auto end = fake_millis + ms;
  while (static_cast<std::int32_t>(fake_millis - end) < 0) {
    delay(10);
    loop();
    CHECK(!fake::esp().watchdog_expired(), "UART watchdog is fed while HomeKit runs");
  }
}
void write_tile(const char* name, const char* characteristic, double value) {
  auto request = homeSpan.write({{tile(name, characteristic), value}});
  run_platform(10);
  CHECK(request->done && request->statuses[0] == fake::HapStatus::Ok, "HAP write accepted");
}
void boot_platform(bool upstream = false) {
  auto& device = fake::esp().tion;
  if (upstream) {
    device.firmware = 0x02D2;
    device.echo_request_ids = true;
  }
  fake::background = [] { fake::esp().pump(); };
  setup();
  run_platform(4000);
  CHECK(tion_uart.status().device.fresh, "real platform sketch receives state");
}
void wait_for_periodic_request() {
  const auto initial = fake::esp().tion.state_requests;
  for (unsigned pass = 0; pass < 500 && fake::esp().tion.state_requests == initial; ++pass) {
    run_platform(10);
  }
  CHECK(fake::esp().tion.state_requests > initial, "periodic polling makes progress");
}

void delayed_poll(bool upstream, bool change_sound) {
  boot_platform(upstream);
  auto& device = fake::esp().tion;
  device.response_delay_ms = 200;
  wait_for_periodic_request();
  CHECK(tion_uart.status().state_reads_pending == 1, "old read is still outstanding");
  device.state.heater_allowed = true;  // Native change before command's own pre-read.
  if (change_sound) write_tile("Звук Tion", "On", 0);
  else write_tile("Нагрев притока", "Active", 0);
  run_platform(2000);
  const auto status = tion_uart.status();
  CHECK(status.control_applied == 1 && status.control_unchanged == 0 &&
            device.writes == 1 && !status.control_pending && control_access.available(tion_uart.status()),
        "a delayed poll never substitutes for the command's own pre-read");
  CHECK(device.state.heater_allowed == change_sound,
        "heat is either explicitly disabled or preserved when changing only sound");
  if (change_sound) CHECK(!device.state.sound_on, "requested sound change applied");
  CHECK(homeSpan.violations == 0, "no HomeSpan misuse");
  fake::esp().stop_task();
}
void legacy_heat_off() { delayed_poll(false, false); }
void upstream_heat_off() { delayed_poll(true, false); }
void legacy_preserves_native() { delayed_poll(false, true); }
void upstream_preserves_native() { delayed_poll(true, true); }

void lost_read_stays_fail_closed() {
  boot_platform();
  auto& device = fake::esp().tion;
  device.respond = false;
  wait_for_periodic_request();
  device.state.heater_allowed = true;
  write_tile("Нагрев притока", "Active", 0);
  run_platform(4500);
  CHECK(tion_uart.status().control_failure == tion4s::ControlFailure::Expired &&
            device.writes == 0,
        "missing old reply expires the intent without writing from another response");
  device.respond = true;
  run_platform(3500);
  CHECK(tion_uart.status().device.fresh && tion_uart.status().state_reads_pending != 0,
        "read-only telemetry recovers without pretending the lost reply was received");
  write_tile("Нагрев притока", "Active", 0);
  run_platform(4500);
  CHECK(device.writes == 0,
        "a new command cannot bypass uncertain response ordering");
  fake::esp().stop_task();
}

void safe_boot_after_watchdogs() {
  auto& board = fake::esp();
  for (unsigned i = 0; i < 3; ++i) {
    tion4s::Esp32UartService failed_boot;
    fake_millis = 0;
    CHECK(failed_boot.begin(), "recovery attempt starts");
    board.pump();
    fake_millis += 10000;
    CHECK(board.watchdog_expired(), "task hangs until watchdog reset");
    board.stop_task();
  }
  setup();  // Actual sketch sees the persisted watchdog budget.
  for (unsigned i = 0; i < 100; ++i) loop();
  const auto status = tion_uart.platform_status();
  CHECK(!uart_task_started && !homeSpan.began && homeSpan.polls == 0 &&
            status.stall_gave_up && status.stall_restarts == 3 &&
            board.uart_begins == 3 && board.task_creations == 3 &&
            board.watchdog_resets == 3 && !board.watchdog_expired(),
        "safe sketch boot keeps both HAP and the faulting task off without another reset");
  CHECK(!fake::pin_writes.empty(), "safe boot still drives the fault LED");
}

const test_support::Scenario cases[] = {
  {"02D0 old poll before heater off", legacy_heat_off},
  {"02D2 old poll before heater off", upstream_heat_off},
  {"02D0 preserves native fields", legacy_preserves_native},
  {"02D2 preserves native fields", upstream_preserves_native},
  {"lost reply remains fail closed", lost_read_stays_fail_closed},
  {"watchdog safe boot through sketch", safe_boot_after_watchdogs},
};
}  // namespace
int main() { return test_support::scenarios(cases); }
