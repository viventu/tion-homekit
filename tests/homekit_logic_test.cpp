// HomeKit mapping and the local boost timer, against the real UartCore and a
// simulated breezer. HomeSpan itself is not involved.

#include "HomeKitLogic.h"
#include "support/check.h"
#include "support/fake_tion.h"
#include "support/host_channel.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

using tion4s::ControlCommand;
using tion_homekit::BoostTimer;

struct Bench {
  fake::FakeTion tion{fake_millis};
  fake::NoLock lock;
  tion4s::UartCore core{lock};
  fake::HostChannel channel{core, fake_millis};
  tion_homekit::ControlAccess access{channel};
  BoostTimer boost{access};
  bool boost_on = false;

  tion4s::UartStatus status() const { return core.status(tion); }
  // The sketch loop: one UART pass, then the HomeKit refresh.
  void run(std::uint32_t ms) {
    const auto end = fake_millis + ms;
    while (static_cast<std::int32_t>(fake_millis - end) < 0) {
      core.service(tion);
      boost_on = boost.tick(status(), fake_millis);
      fake_millis += 10;
    }
  }
  void run_uart_only(std::uint32_t ms) { fake::run(core, tion, fake_millis, ms); }
  // Loop passes until a command is in the mailbox: the next UART pass
  // starts its pre-read.
  bool run_until_submitted() {
    for (int pass = 0; pass < 20000; ++pass) {
      if (status().waiting_ticket != 0) return true;
      run(10);
    }
    return false;
  }
};

tion4s::Snapshot valid_snapshot() {
  tion4s::Snapshot snapshot;
  snapshot.has_device_info = true;
  snapshot.device_info.device_type = tion4s::kDeviceType4S;
  snapshot.device_info.work_mode = tion4s::kWorkModeNormal;
  snapshot.fresh = true;
  snapshot.has_state = true;
  snapshot.state.power_on = true;
  snapshot.state.fan_speed = 3;
  snapshot.state.max_fan_speed = 6;
  return snapshot;
}

void test_fan_command() {
  auto snapshot = valid_snapshot();
  auto command = tion_homekit::fan_command(true, false, false, 0, snapshot);
  CHECK(command && command->power_on == false && !command->fan_speed,
        "Active alone switches power");
  command = tion_homekit::fan_command(false, true, true, 0, snapshot);
  CHECK(command && command->power_on == false && !command->fan_speed,
        "zero speed turns the breezer off");
  command = tion_homekit::fan_command(false, true, true, 67, snapshot);
  CHECK(command && !command->power_on && command->fan_speed == 4,
        "speed alone on a running breezer keeps power untouched");
  snapshot.state.power_on = false;
  command = tion_homekit::fan_command(false, true, true, 17, snapshot);
  CHECK(command && command->power_on == true && command->fan_speed == 1,
        "a positive speed on a stopped breezer also starts it");
  command = tion_homekit::fan_command(true, false, true, 50, snapshot);
  CHECK(command && command->power_on == false && command->fan_speed == 3,
        "Active and speed together keep both requests");
  CHECK(!tion_homekit::fan_command(false, true, true, 101, snapshot) &&
            !tion_homekit::fan_command(false, true, true, -1, snapshot) &&
            !tion_homekit::fan_command(false, true, true,
                                       std::numeric_limits<float>::quiet_NaN(), snapshot),
        "percent outside 0-100 or NaN is refused");
  CHECK(!tion_homekit::fan_command(false, true, false, 50, snapshot),
        "nothing updated means nothing to send");
  snapshot.fresh = false;
  CHECK(!tion_homekit::fan_command(true, true, false, 0, snapshot),
        "a stale snapshot refuses the write");
}

void test_fan_snapshot_validity() {
  using tion_homekit::fan_snapshot_valid;
  CHECK(fan_snapshot_valid(valid_snapshot()), "a fresh 4S state is shown");
  auto snapshot = valid_snapshot();
  snapshot.has_device_info = false;
  CHECK(!fan_snapshot_valid(snapshot), "an unidentified device is not shown");
  snapshot = valid_snapshot();
  snapshot.device_info.work_mode = 2;
  CHECK(!fan_snapshot_valid(snapshot), "another work mode is not shown");
  snapshot = valid_snapshot();
  snapshot.fresh = false;
  CHECK(!fan_snapshot_valid(snapshot), "a stale state is not shown");
  snapshot = valid_snapshot();
  snapshot.state.max_fan_speed = 0;
  CHECK(!fan_snapshot_valid(snapshot), "no speed range is not shown");
  snapshot.state.max_fan_speed = 7;
  CHECK(!fan_snapshot_valid(snapshot), "more than six steps is not shown");
  snapshot = valid_snapshot();
  snapshot.state.fan_speed = 7;
  CHECK(!fan_snapshot_valid(snapshot), "a step above the maximum is not shown");
}

void test_heater_command() {
  auto command = tion_homekit::heater_command(true, true, false, 20.2f);
  CHECK(command && command->heater_allowed == true && !command->target_temperature,
        "Active alone allows heat");
  command = tion_homekit::heater_command(false, false, true, 21.6f);
  CHECK(command && !command->heater_allowed && command->target_temperature == 22,
        "threshold alone rounds to whole degrees");
  command = tion_homekit::heater_command(true, false, true, 18.0f);
  CHECK(command && command->heater_allowed == false && command->target_temperature == 18,
        "both changes travel together");
  CHECK(!tion_homekit::heater_command(true, true, false, 0.5f) &&
            !tion_homekit::heater_command(false, true, true, 25.5f) &&
            !tion_homekit::heater_command(false, true, true, NAN) &&
            !tion_homekit::heater_command(false, false, false, 20.0f),
        "out-of-range, NaN or empty heater writes are refused");
}

void test_auxiliary_and_temperatures() {
  tion4s::State state;
  state.gate_position = 1;
  state.sound_on = false;
  state.led_on = true;
  using tion_homekit::kLed;
  using tion_homekit::kRecirculation;
  using tion_homekit::kSound;
  CHECK(kRecirculation.reported(state) && !kSound.reported(state) && kLed.reported(state),
        "switch values follow the reported state");
  state.gate_position = 0;
  CHECK(!kRecirculation.reported(state), "outdoor air is not recirculation");
  CHECK(kRecirculation.command(false).gate_position == 0 &&
            kSound.command(true).sound_on == true && kLed.command(false).led_on == false,
        "each switch writes its own field");
  CHECK(tion_homekit::temperature_in_range(-50) &&
            tion_homekit::temperature_in_range(100) &&
            !tion_homekit::temperature_in_range(-51),
        "temperatures from -50 to 100 C are published");
}

void test_boost_runs_and_restores() {
  fake_millis = 1000;
  Bench bench;
  bench.run(4000);
  CHECK(bench.boost.start(fake_millis), "boost starts from speed 3");
  CHECK(!bench.boost.start(fake_millis), "a running boost cannot start again");
  bench.run(2000);
  CHECK(bench.boost_on && bench.tion.state.fan_speed == 6, "fan at maximum");
  bench.run(BoostTimer::kDurationMs);
  CHECK(!bench.boost_on && bench.tion.state.fan_speed == 3,
        "after five minutes the original speed returns");
  CHECK(bench.tion.writes == 2, "one write up and one write back");
}

void test_boost_manual_stop_and_native_change() {
  fake_millis = 1000;
  Bench bench;
  bench.run(4000);
  CHECK(bench.boost.start(fake_millis), "boost starts");
  bench.run(2000);
  bench.boost.stop();
  bench.run(2000);
  CHECK(!bench.boost_on && bench.tion.state.fan_speed == 3,
        "switching off restores at once");

  CHECK(bench.boost.start(fake_millis), "boost starts again");
  bench.run(2000);
  bench.tion.state.fan_speed = 2;  // Native remote.
  bench.run(BoostTimer::kDurationMs + 3000);
  CHECK(!bench.boost_on && bench.tion.state.fan_speed == 2 && bench.tion.writes == 3,
        "a native change cancels the timer and nothing is restored");
}

void test_boost_refusals_and_failure() {
  fake_millis = 1000;
  Bench bench;
  bench.tion.state.fan_speed = 6;
  bench.run(4000);
  CHECK(!bench.boost.start(fake_millis), "already at maximum");
  bench.tion.state.fan_speed = 3;
  bench.tion.state.power_on = false;
  bench.run(3000);
  CHECK(!bench.boost.start(fake_millis), "breezer off");
  bench.tion.state.power_on = true;
  bench.run(3000);
  CHECK(bench.boost.start(fake_millis), "boost starts");
  bench.run(2000);
  bench.tion.apply_writes = false;
  CHECK(bench.access.submit(ControlCommand::sound(false)), "unrelated command starts");
  bench.run(3000);
  CHECK(bench.status().control_failed == 1 && bench.boost_on && bench.tion.writes == 2,
        "another command's failure does not end the boost");
  bench.tion.apply_writes = true;
  bench.run(BoostTimer::kDurationMs);
  CHECK(!bench.boost_on && bench.tion.state.fan_speed == 3 && bench.tion.writes == 3,
        "the previous speed still returns after five minutes");
  CHECK(bench.boost.start(fake_millis), "a new boost works after the failure");
}

// The boost result must come from its own command even when another command
// finishes before the next HomeKit refresh.
void test_boost_follows_its_own_ticket() {
  fake_millis = 1000;
  Bench bench;
  bench.run(4000);
  CHECK(bench.boost.start(fake_millis), "boost submitted");
  bench.run_uart_only(20);
  // Sound is already on, so this command ends Unchanged right after boost.
  CHECK(bench.access.submit(ControlCommand::sound(true)) != 0, "second command waits");
  bench.run_uart_only(3000);  // Both finish before the timer looks.
  bench.run(20);
  CHECK(bench.boost_on && bench.tion.state.fan_speed == 6,
        "boost keeps running although the last result was Unchanged");
  bench.run(BoostTimer::kDurationMs);
  CHECK(bench.tion.state.fan_speed == 3, "and it still restores the original speed");
}

void test_boost_start_conditions() {
  fake_millis = 1000;
  Bench bench;
  bench.run(4000);
  bench.boost.stop();
  bench.run(100);
  CHECK(!bench.boost_on && bench.tion.writes == 0, "stopping an idle boost does nothing");
  bench.tion.state.fan_speed = 0;
  bench.run(3000);
  CHECK(!bench.boost.start(fake_millis), "a stopped fan motor is not boosted");
  bench.tion.state.fan_speed = 3;
  bench.run(3000);
  CHECK(bench.access.submit(ControlCommand::sound(false)) != 0, "another command waits");
  CHECK(!bench.boost.start(fake_millis),
        "boost is refused while another command waits: it cannot wait itself");
  bench.run(3000);
  bench.tion.respond = false;
  bench.run(11000);
  CHECK(!bench.boost.start(fake_millis),
        "a stale state is not boosted");
}

// Starts a boost and runs until it holds the maximum step.
void start_boost(Bench& bench) {
  CHECK(bench.boost.start(fake_millis), "boost starts");
  bench.run(2000);
  CHECK(bench.boost_on && bench.tion.state.fan_speed == 6, "boost runs");
}

// Only airflow changed elsewhere ends a boost without its restore.
void test_boost_ends_when_the_airflow_changes() {
  const std::vector<void (*)(tion4s::State&)> changes = {
      [](tion4s::State& state) { state.power_on = false; },
      [](tion4s::State& state) { state.gate_position = 1; },
  };
  for (const auto change : changes) {
    fake_millis = 1000;
    Bench bench;
    bench.run(4000);
    start_boost(bench);
    change(bench.tion.state);
    bench.run(BoostTimer::kDurationMs + 3000);
    CHECK(!bench.boost_on && bench.tion.writes == 1,
          "power or gate changed natively: the boost ends without a restore");
  }

  fake_millis = 1000;
  Bench heat;
  heat.run(4000);
  start_boost(heat);
  heat.tion.state.heater_allowed = true;
  heat.run(BoostTimer::kDurationMs + 3000);
  CHECK(!heat.boost_on && heat.tion.state.fan_speed == 3 && heat.tion.state.heater_allowed &&
            heat.tion.writes == 2,
        "heat is not airflow: the previous speed returns and heat stays");

  fake_millis = 1000;
  Bench stale;
  stale.run(4000);
  start_boost(stale);
  stale.tion.respond = false;
  stale.run(11000);
  CHECK(stale.boost_on && stale.tion.state.fan_speed == 6,
        "stale telemetry decides nothing: the boost keeps running");
  stale.tion.respond = true;
  stale.run(BoostTimer::kDurationMs);
  CHECK(!stale.boost_on && stale.tion.state.fan_speed == 3,
        "the restore follows once the state is fresh again");
}

void test_boost_restore_rules() {
  fake_millis = 1000;
  Bench bench;
  bench.run(4000);
  start_boost(bench);
  CHECK(bench.access.submit(ControlCommand::led(false)) != 0, "another command");
  bench.boost.stop();
  bench.run(20);
  CHECK(bench.status().control_pending && bench.tion.state.fan_speed == 6,
        "the restore waits for the other command");
  bench.run(3000);
  CHECK(!bench.boost_on && bench.tion.state.fan_speed == 3 && !bench.tion.state.led_on,
        "then the previous step returns");

  start_boost(bench);
  bench.boost.stop();
  auto stale = bench.status();
  stale.device.fresh = false;
  CHECK(!bench.boost.tick(stale, fake_millis) && bench.status().waiting_ticket == 0,
        "no restore is sent from stale telemetry");
  bench.run_uart_only(11000);  // The loop stalled.
  bench.run(3000);
  CHECK(!bench.boost_on && bench.tion.state.fan_speed == 3,
        "a late restore still returns the previous speed");

  CHECK(bench.boost.start(fake_millis), "boost starts");
  bench.boost.stop();
  bench.run(3000);
  CHECK(!bench.boost_on && bench.tion.state.fan_speed == 3,
        "switching off before the speed-up is confirmed still restores");

  bench.tion.state.fan_speed = 3;
  bench.run(3000);
  start_boost(bench);
  bench.boost.stop();
  bench.tion.state.fan_speed = 4;  // Native change before the restore.
  bench.run_uart_only(3000);
  bench.run(3000);
  CHECK(!bench.boost_on && bench.tion.state.fan_speed == 4,
        "a restore never overrides a native change");
}

// A failed restore is tried again; a restore that keeps failing stops.
void test_boost_restore_retries() {
  fake_millis = 1000;
  Bench once;
  once.run(4000);
  start_boost(once);
  once.boost.stop();
  once.tion.apply_writes = false;
  for (int pass = 0; pass < 300 && once.tion.writes < 2; ++pass) once.run(10);
  once.tion.apply_writes = true;
  once.run(10000);
  CHECK(!once.boost_on && once.tion.state.fan_speed == 3 && once.tion.writes == 3,
        "an unconfirmed restore is sent again");

  fake_millis = 1000;
  Bench never;
  never.run(4000);
  start_boost(never);
  never.boost.stop();
  never.tion.apply_writes = false;
  never.run(30000);
  CHECK(never.tion.state.fan_speed == 6 &&
            never.tion.writes == 1 + BoostTimer::kRestoreAttempts,
        "the restore is attempted a bounded number of times");
  never.tion.apply_writes = true;
  never.run(30000);
  CHECK(never.tion.writes == 1 + BoostTimer::kRestoreAttempts, "and then left alone");

  fake_millis = 1000;
  Bench busy;
  busy.run(4000);
  start_boost(busy);
  busy.boost.stop();
  auto idle = busy.status();
  CHECK(busy.access.submit(ControlCommand::sound(false)) != 0, "one command runs");
  busy.run_uart_only(20);
  CHECK(busy.access.submit(ControlCommand::power(true)) != 0, "another one waits");
  CHECK(!busy.boost.tick(idle, fake_millis) && busy.tion.state.fan_speed == 6,
        "a restore refused by a full mailbox waits");
  busy.run(5000);
  CHECK(busy.tion.state.fan_speed == 3, "and is sent when the mailbox frees");

  fake_millis = 1000;
  Bench native;
  native.run(4000);
  start_boost(native);
  native.boost.stop();
  CHECK(native.run_until_submitted(), "the restore is queued");
  native.tion.state.fan_speed = 5;  // Changed on the remote before the pre-read.
  native.run(10000);
  CHECK(native.tion.state.fan_speed == 5 && native.tion.writes == 1,
        "a restore that meets changed airflow is not repeated");
}

// A boost whose result is no longer tracked is not assumed to run.
void test_boost_with_lost_outcome() {
  fake_millis = 1000;
  Bench bench;
  bench.run(4000);
  CHECK(bench.boost.start(fake_millis), "boost submitted");
  auto status = bench.status();
  status.running_ticket = 0;
  status.waiting_ticket = 0;
  CHECK(!bench.boost.tick(status, fake_millis), "unknown outcome: the switch turns off");
}

}  // namespace

int main() {
  test_fan_command();
  test_fan_snapshot_validity();
  test_heater_command();
  test_auxiliary_and_temperatures();
  test_boost_runs_and_restores();
  test_boost_manual_stop_and_native_change();
  test_boost_refusals_and_failure();
  test_boost_follows_its_own_ticket();
  test_boost_start_conditions();
  test_boost_ends_when_the_airflow_changes();
  test_boost_restore_rules();
  test_boost_restore_retries();
  test_boost_with_lost_outcome();
  std::cout << "HomeKit mapping and boost timer: PASS\n";
}
