#include "../firmware/TionReadOnly/TionReadOnly.ino"
#include "esp32_test_platform.h"
#include "support/scenarios.h"

namespace {
void task_runs() {
  fake::background = [] { fake::esp().pump(); };
  setup();
  for (unsigned i = 0; i < 4; ++i) loop();
  CHECK(tion_uart.status().device.fresh && fake::esp().tion.heartbeats >= 2 &&
            fake::esp().tion.writes == 0, "read-only sketch maintains heartbeat and state");
  fake::esp().stop_task();
}
void fallback_runs() {
  fake::esp().task_starts = false;
  setup();
  for (unsigned i = 0; i < 400; ++i) loop();
  CHECK(tion_uart.status().device.fresh && !uart_task_started &&
            fake::esp().tion.writes == 0, "read-only fallback services the real adapter");
}
const test_support::Scenario cases[] = {{"task", task_runs}, {"fallback", fallback_runs}};
}
int main() { return test_support::scenarios(cases); }
