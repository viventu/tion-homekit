// The actual adapter and task loop, with doubles only for the ESP-IDF/RTOS
// boundary. Reboots create new services while retaining the adapter's RTC data.
#include <tion4s/esp32_uart_service.h>
#include "esp32_test_platform.h"
#include "support/scenarios.h"

namespace {
struct Boot {
  Boot() = default;
  Boot(const Boot&) = delete;
  Boot& operator=(const Boot&) = delete;
  Boot(Boot&&) = delete;
  Boot& operator=(Boot&&) = delete;
  tion4s::Esp32UartService service;
  ~Boot() { fake::esp().stop_task(); }
  void run(std::uint32_t duration) {
    const auto end = fake_millis + duration;
    while (static_cast<std::int32_t>(fake_millis - end) < 0) {
      fake_millis += 10;
      fake::esp().pump();
      service.supervise();
    }
  }
};

void normal_and_status_race() {
  Boot boot;
  auto& board = fake::esp();
  CHECK(boot.service.begin() && boot.service.begin(), "begin is idempotent");
  CHECK(board.uart_begins == 1 && board.task_creations == 1 && board.usb_disables == 1,
        "UART opens once after freeing USB pads");
  boot.service.service_fallback();
  CHECK(board.tion.heartbeats == 0, "fallback does not run beside the task");
  boot.run(4000);
  auto status = boot.service.status();
  const auto platform = boot.service.platform_status();
  CHECK(status.device.fresh && status.device.has_device_info && platform.task_running &&
            platform.stack_free_bytes == 1234 && board.stack_samples > 1 &&
            board.watchdog_feeds > 100 && !board.watchdog_expired(),
        "real task polls, measures its stack and feeds its watchdog");
  board.before_lock = [&] { ++fake_millis; board.pump(); };
  status = boot.service.status();
  CHECK(status.device.fresh && status.device.heartbeat_age_ms < 3100,
        "publication while status waits for its lock cannot underflow age");
  board.before_lock = [&] { ++fake_millis; board.pump(); };
  boot.service.supervise();
  CHECK(board.software_resets == 0, "supervision does not restart a healthy UART");
  CHECK(boot.service.submit(tion4s::ControlCommand::power(false), true,
                             status.control_failed) != 0, "real adapter accepts control");
  boot.run(2000);
  CHECK(!board.tion.state.power_on && boot.service.status().control_applied == 1,
        "command reaches the driver, device and confirmation path");
}

void failed_starts() {
  auto& board = fake::esp();
  {
    Boot boot;
    board.uart_opens = false;
    CHECK(!boot.service.begin() && !boot.service.begin(), "failed UART is not retried");
    boot.service.service_fallback();
    boot.service.supervise();
    CHECK(board.task_creations == 0 && !boot.service.platform_status().task_running,
          "no task or fallback accesses a closed port");
  }
  {
    Boot boot;
    board.uart_opens = true;
    board.task_starts = false;
    CHECK(!boot.service.begin(), "task allocation failure");
    for (unsigned i = 0; i < 400; ++i) {
      fake_millis += 10;
      boot.service.service_fallback();
      boot.service.supervise();
    }
    CHECK(boot.service.status().device.fresh && board.tion.heartbeats >= 2,
          "task failure retains bounded polling through fallback");
  }
}

void watchdog_registration_failure() {
  Boot boot;
  auto& board = fake::esp();
  board.watchdog_add_succeeds = false;
  CHECK(boot.service.begin(), "task starts when watchdog enrollment fails");
  boot.run(4000);
  CHECK(board.watchdog_feeds == 0 && board.tion.heartbeats >= 2,
        "no reset calls for an unsubscribed task; supervision still runs");
}

void transport_errors() {
  Boot boot;
  auto& board = fake::esp();
  CHECK(boot.service.begin(), "UART starts");
  for (int result : {-1, 0}) {
    board.tx_write_result = result;
    board.pump();
  }
  board.tx_write_result = -2;
  board.tx_done_result = ESP_FAIL;
  board.pump();
  CHECK(boot.service.status().tx_failures == 3, "negative, short and TX-done failures");
  board.tx_done_result = ESP_OK;
  boot.run(2000);
  CHECK(boot.service.status().device.fresh, "polling recovers after driver failures");
}

void software_restart_limit() {
  auto& board = fake::esp();
  board.tion.fail_writes = true;
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    Boot boot;
    fake_millis = 0;
    CHECK(boot.service.begin(), "recovery attempt starts");
    for (unsigned pass = 0; pass < 100 && board.software_resets == attempt; ++pass) {
      boot.run(100);
    }
    CHECK(board.software_resets == attempt + 1, "stalled TX requests a bounded restart");
    CHECK(boot.service.platform_status().stall_restarts == attempt + 1,
          "one software reset spends one attempt");
  }
  Boot locked;
  CHECK(!locked.service.begin(), "next boot latches the exhausted budget");
  locked.service.service_fallback();
  locked.service.supervise();
  fake_millis += 100000;
  CHECK(board.uart_begins == 3 && board.task_creations == 3 &&
            !board.watchdog_expired() && locked.service.platform_status().stall_gave_up,
        "safe boot never starts the faulting driver or another watchdog task");
}

void watchdog_restart_limit_for(esp_reset_reason_t reason) {
  auto& board = fake::esp();
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    Boot boot;
    fake_millis = 0;
    CHECK(boot.service.begin(), "watchdog recovery attempt starts");
    board.pump();
    fake_millis += board.watchdog_timeout_ms;
    CHECK(board.watchdog_expired(), "a stuck task reaches the watchdog timeout");
    board.reset_reason = reason;
  }
  {
    Boot locked;
    CHECK(!locked.service.begin() && locked.service.platform_status().stall_restarts == 3,
          "watchdog/panic reset counts toward the same budget");
    locked.service.supervise();
    locked.service.service_fallback();
    CHECK(board.uart_begins == 3 && !board.watchdog_expired(), "no fourth faulty task");
  }
  {
    Boot still_locked;
    CHECK(!still_locked.service.begin(), "repeated reset never wraps the saturated count");
  }
  board.reset_reason = ESP_RST_POWERON;
  board.tion.respond = true;
  Boot recovered;
  CHECK(recovered.service.begin() && recovered.service.platform_status().stall_restarts == 0,
        "an explicit power cycle permits recovery");
}
void task_watchdog_limit() { watchdog_restart_limit_for(ESP_RST_TASK_WDT); }
void interrupt_watchdog_limit() { watchdog_restart_limit_for(ESP_RST_INT_WDT); }
void other_watchdog_limit() { watchdog_restart_limit_for(ESP_RST_WDT); }
void panic_limit() { watchdog_restart_limit_for(ESP_RST_PANIC); }

void healthy_run_clears_budget() {
  auto& board = fake::esp();
  // An uninitialized RTC record is not trusted even on a non-power reset.
  board.reset_reason = ESP_RST_UNKNOWN;
  { Boot first; CHECK(first.service.begin(), "invalid RTC marker starts at zero"); }
  board.reset_reason = ESP_RST_TASK_WDT;
  Boot recovered;
  CHECK(recovered.service.begin() && recovered.service.platform_status().stall_restarts == 1,
        "watchdog failure recorded");
  recovered.run(62000);
  CHECK(recovered.service.platform_status().stall_restarts == 0 && board.software_resets == 0,
        "a sustained healthy minute clears previous failures");
}

void no_first_heartbeat() {
  Boot boot;
  CHECK(boot.service.begin(), "task allocated");
  fake_millis += 7000;  // Task never scheduled.
  boot.service.supervise();
  CHECK(fake::esp().software_resets == 1, "missing first TX also triggers supervision");
}

const test_support::Scenario cases[] = {
  {"normal and clock race", normal_and_status_race}, {"failed starts", failed_starts},
  {"watchdog enrollment", watchdog_registration_failure}, {"TX failures", transport_errors},
  {"software reset limit", software_restart_limit}, {"task watchdog", task_watchdog_limit},
  {"interrupt watchdog", interrupt_watchdog_limit}, {"other watchdog", other_watchdog_limit},
  {"panic", panic_limit}, {"healthy recovery", healthy_run_clears_budget},
  {"no first heartbeat", no_first_heartbeat},
};
}  // namespace
int main() { return test_support::scenarios(cases); }
