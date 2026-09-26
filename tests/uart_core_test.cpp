// UartCore scheduling, mailbox and recovery against a simulated breezer.

#include "support/check.h"
#include "support/fake_tion.h"
#include "tion4s/stall_guard.h"
#include "tion4s/uart_core.h"

#include <iostream>

namespace {

using tion4s::CommandOutcome;
using tion4s::ControlCommand;
using tion4s::ControlFailure;
using tion4s::ControlPhase;
using tion4s::StallGuard;
using tion4s::UartCore;
using tion4s::WriteRejection;

struct Bench {
  std::uint32_t clock = 0xFFFF'0000u;  // Crosses the millis() wrap.
  fake::FakeTion tion{clock};
  fake::NoLock lock;
  UartCore core{lock};

  tion4s::UartStatus status() const { return core.status(tion); }
  void run(std::uint32_t ms) { fake::run(core, tion, clock, ms); }
  std::uint32_t submit(const ControlCommand& command) {
    return core.submit(command, true, status().control_failed, clock);
  }
  CommandOutcome outcome(std::uint32_t ticket) const {
    return tion4s::outcome_of(status(), ticket);
  }
  void settle_until_idle() {
    for (int i = 0; i < 1000 && status().control_pending; ++i) {
      run(10);
    }
  }
};

// HomeKit callbacks run on another core while the UART task blocks in a write.
// Whatever they observe, "not pending" must come with the device state that
// the finished command produced.
void test_idle_status_is_never_older_than_the_command() {
  Bench bench;
  bench.run(4000);
  unsigned observed_idle = 0;
  bench.tion.on_write = [&bench, &observed_idle] {
    const auto status = bench.status();
    if (!status.control_pending && status.device.has_state) {
      ++observed_idle;
      CHECK(status.device.state.heater_allowed == bench.tion.state.heater_allowed,
            "idle status never shows the state from before the last command");
    }
  };
  for (int i = 0; i < 20; ++i) {
    CHECK(bench.submit(ControlCommand::heater(i % 2 == 0)), "heater toggle accepted");
    bench.settle_until_idle();
    bench.run(2500);  // Idle polls must agree with the device as well.
  }
  CHECK(observed_idle > 20 && bench.status().control_applied == 20,
        "writes after each completion were observed");
}

void test_boot_and_heartbeat_priority() {
  Bench bench;
  bench.run(30);
  CHECK(bench.tion.heartbeats == 1, "first heartbeat leaves in the first pass");
  bench.run(5000);
  const auto status = bench.status();
  CHECK(status.device.has_device_info && status.device.has_state && status.device.fresh,
        "device info and state are polled after the heartbeat");
  CHECK(status.device.state.outdoor_temperature == -12, "negative intake is decoded");
  CHECK(bench.lock.depth == 0, "every critical section is released");

  // Commands through the whole run must not delay heartbeats.
  for (int i = 0; i < 20; ++i) {
    bench.submit(ControlCommand::speed_step(static_cast<std::uint8_t>(1 + i % 6)));
    bench.run(1500);
  }
  CHECK(bench.tion.max_heartbeat_gap_ms <= 3100,
        "heartbeat gap stays near 3 s while transactions run");
}

void test_transaction_owns_state_polls() {
  Bench bench;
  bench.run(4000);
  CHECK(bench.submit(ControlCommand::power(false)), "idle mailbox accepts a command");
  bench.settle_until_idle();
  auto status = bench.status();
  CHECK(!bench.tion.state.power_on && bench.tion.writes == 1 &&
            status.control_phase == ControlPhase::Applied && status.control_applied == 1,
        "one read-write-verify turns the breezer off");

  // Keep the channel busy for several poll intervals. Every state request in
  // that window must belong to a transaction: one pre-read, then one verify.
  const auto requests = bench.tion.state_requests;
  const auto applied = status.control_applied;
  const auto unchanged = status.control_unchanged;
  for (int i = 0; i < 800; ++i) {
    bench.submit(
        ControlCommand::power_and_speed(true, static_cast<std::uint8_t>(1 + i % 6)));
    bench.run(10);
  }
  bench.settle_until_idle();
  status = bench.status();
  const auto transaction_reads =
      2 * (status.control_applied - applied) + (status.control_unchanged - unchanged);
  CHECK(status.control_applied - applied >= 8, "the channel stayed busy");
  // At most one poll: the one due in the pass where the channel became idle.
  CHECK(bench.tion.state_requests - requests <= transaction_reads + 1,
        "periodic polls pause while commands run or wait");
  CHECK(bench.tion.max_heartbeat_gap_ms <= 3100, "heartbeat is never paused");
}

void test_mailbox_merges_same_group() {
  Bench bench;
  bench.run(4000);
  const auto first = bench.submit(ControlCommand::speed_step(2));
  CHECK(first != 0, "first slider value starts");
  bench.run(20);
  CHECK(bench.outcome(first) == CommandOutcome::Pending, "first command is in flight");
  const auto second = bench.submit(ControlCommand::speed_step(4));
  CHECK(second != 0 && bench.outcome(second) == CommandOutcome::Pending,
        "second value waits and is visible at once");
  const auto third = bench.submit(ControlCommand::speed_step(5));
  CHECK(third != 0 && bench.outcome(second) == CommandOutcome::Merged,
        "third value merges into the waiting one");
  CHECK(bench.submit(ControlCommand::sound(false)) == 0,
        "a different group is refused while one waits");
  CHECK(bench.submit(ControlCommand::filter_reset()) == 0, "filter reset never waits");
  CHECK(bench.submit(ControlCommand::speed_in_context(6, {3, 0, false})) == 0,
        "context-bound command never waits");
  bench.settle_until_idle();
  CHECK(bench.tion.state.fan_speed == 5 && bench.tion.writes == 2,
        "only the first and the latest slider values are written");
  CHECK(bench.outcome(first) == CommandOutcome::Applied &&
            bench.outcome(third) == CommandOutcome::Applied,
        "each executed ticket reports its own result");
  CHECK(bench.status().control_failed == 0, "merging is not a failure");

  // Power and speed from different HomeKit writes become one fan write.
  const auto running = bench.submit(ControlCommand::sound(false));
  bench.run(20);
  CHECK(bench.outcome(running) == CommandOutcome::Pending, "sound command in flight");
  CHECK(bench.submit(ControlCommand::power(false)) != 0, "power off waits");
  const auto merged = bench.submit(ControlCommand::speed_step(2));
  bench.settle_until_idle();
  CHECK(bench.outcome(merged) == CommandOutcome::Applied && !bench.tion.state.power_on &&
            bench.tion.state.fan_speed == 2 && bench.tion.writes == 4,
        "power and speed merge into one write");
}

void test_recent_results_are_bounded() {
  Bench bench;
  bench.run(4000);
  const auto oldest = bench.submit(ControlCommand::speed_step(2));
  bench.settle_until_idle();
  CHECK(bench.outcome(oldest) == CommandOutcome::Applied, "fresh result is known");
  for (std::uint8_t step = 3; step < 3 + tion4s::UartStatus::kRecentResults; ++step) {
    CHECK(bench.submit(ControlCommand::speed_step(step)) != 0, "later command");
    bench.settle_until_idle();
  }
  CHECK(bench.outcome(oldest) == CommandOutcome::Unknown,
        "a result pushed out of the recent list is unknown, never a success");
  CHECK(bench.outcome(0) == CommandOutcome::Unknown, "ticket 0 means refused");
}

void test_failure_drops_waiting_and_stale_callers() {
  Bench bench;
  bench.run(4000);
  bench.tion.apply_writes = false;  // Device ignores the write.
  const auto seen = bench.status().control_failed;
  const auto failing = bench.submit(ControlCommand::speed_step(2));
  CHECK(failing != 0, "command starts");
  bench.run(20);
  const auto waiting = bench.submit(ControlCommand::speed_step(5));
  CHECK(waiting != 0, "next value waits");
  bench.settle_until_idle();
  auto status = bench.status();
  CHECK(status.control_failed == seen + 1 &&
            status.control_failure == ControlFailure::NotApplied,
        "unconfirmed write fails once");
  CHECK(bench.tion.writes == 1 && bench.outcome(failing) == CommandOutcome::Failed &&
            bench.outcome(waiting) == CommandOutcome::Dropped,
        "failure drops the waiting command");
  bench.tion.apply_writes = true;
  CHECK(bench.core.submit(ControlCommand::speed_step(4), true, seen, bench.clock) == 0,
        "a caller that has not seen the failure cannot submit");
  CHECK(bench.core.submit(ControlCommand::speed_step(4), true, status.control_failed,
                          bench.clock) != 0,
        "a caller aware of the failure may submit again");
  bench.settle_until_idle();
  CHECK(bench.tion.state.fan_speed == 4, "new command applies after the failure");
}

void test_unchanged_is_not_failure() {
  Bench bench;
  bench.run(4000);
  const auto ticket = bench.submit(ControlCommand::speed_step(3));
  CHECK(ticket != 0, "command matching the device is queued");
  bench.settle_until_idle();
  const auto status = bench.status();
  CHECK(bench.outcome(ticket) == CommandOutcome::Unchanged, "ticket reports Unchanged");
  CHECK(status.control_phase == ControlPhase::Unchanged &&
            status.control_unchanged == 1 && status.control_failed == 0 &&
            bench.tion.writes == 0,
        "fresh pre-read already at target: no write and no failure");
}

void test_expired_before_write() {
  Bench bench;
  bench.run(4000);
  CHECK(bench.submit(ControlCommand::power(false)), "command accepted");
  bench.clock += tion4s::ControlTransaction::kIntentMaxAgeMs + 100;  // Task starved.
  bench.run(50);
  const auto status = bench.status();
  CHECK(status.control_failure == ControlFailure::Expired && bench.tion.writes == 0 &&
            bench.tion.state.power_on,
        "an intent that aged out in the mailbox is reported as expired, never sent");
}

void test_safe_direction_with_device_errors() {
  Bench bench;
  bench.tion.state.errors = 0x4;
  bench.run(4000);
  CHECK(bench.submit(ControlCommand::speed_step(5)), "speed command accepted");
  bench.settle_until_idle();
  CHECK(bench.status().control_failure == ControlFailure::Refused &&
            bench.status().control_refusal == WriteRejection::DeviceErrors &&
            bench.tion.writes == 0,
        "reported device errors block a speed change and say why");
  const auto failed = bench.status().control_failed;
  CHECK(bench.core.submit(ControlCommand::power(false), true, failed, bench.clock),
        "power-off accepted");
  bench.settle_until_idle();
  CHECK(!bench.tion.state.power_on && bench.status().control_failed == failed,
        "reported device errors do not block power-off");
}

void test_idle_flush_recovers_frame_behind_stale_header() {
  Bench bench;
  bench.run(4000);
  const auto before = bench.status().device.heartbeat_count;
  // A spurious magic byte with a long plausible length, then a real frame and
  // silence: the frame is complete but hidden inside the stale candidate.
  auto bytes = std::vector<std::uint8_t>{0x3A, 0x2A, 0x00};
  const auto heartbeat = fake::FakeTion::frame(0x3931, {0});
  bytes.insert(bytes.end(), heartbeat.begin(), heartbeat.end());
  bench.tion.respond = false;
  bench.tion.inject(bytes);
  bench.run(20);
  CHECK(bench.status().device.heartbeat_count == before, "frame still hidden");
  bench.run(300);
  CHECK(bench.status().device.heartbeat_count == before + 1,
        "idle flush recovers the hidden frame instead of discarding it");
}

void test_tx_failure_triggers_bounded_restart() {
  Bench bench;
  bench.run(4000);
  bench.tion.fail_writes = true;
  StallGuard guard;
  std::uint32_t restarts = 0;
  auto action = StallGuard::Action::None;
  for (int i = 0; i < 200 && action == StallGuard::Action::None; ++i) {
    bench.run(100);
    action =
        guard.evaluate(bench.status().device.heartbeat_age_ms, bench.clock, restarts);
  }
  CHECK(action == StallGuard::Action::Restart && restarts == 1 &&
            bench.status().tx_failures > 0,
        "persistent TX failure is detected as a heartbeat stall");
}

void test_submit_needs_one_group() {
  Bench bench;
  bench.run(4000);
  ControlCommand mixed = ControlCommand::power(false);
  mixed.led_on = false;
  CHECK(bench.submit(ControlCommand{}) == 0 && bench.submit(mixed) == 0 &&
            !bench.status().control_pending,
        "commands without exactly one group never enter the mailbox");
}

void test_receive_budget_bounds_a_pass() {
  Bench bench;
  bench.run(4000);
  bench.tion.respond = false;
  const auto rejected = bench.status().rejected_frames;
  // Every byte is a magic byte with an impossible size behind it.
  bench.tion.inject(std::vector<std::uint8_t>(200, 0x3A));
  bench.core.service(bench.tion);
  CHECK(bench.status().rejected_frames == rejected + UartCore::kReceiveBudget - 2,
        "one pass reads at most the receive budget");
  bench.core.service(bench.tion);
  CHECK(bench.status().rejected_frames == rejected + 198,
        "the rest follows in the next pass");
}

void test_transport_failure_during_control() {
  Bench bench;
  bench.run(4000);
  const auto failed = bench.status().control_failed;
  CHECK(bench.submit(ControlCommand::speed_step(5)) != 0, "command accepted");
  for (int i = 0; i < 100 && bench.status().control_phase != ControlPhase::Reading; ++i) {
    bench.core.service(bench.tion);
    ++bench.clock;
  }
  CHECK(bench.status().control_phase == ControlPhase::Reading, "pre-read sent");
  bench.tion.fail_writes = true;
  bench.run(300);
  const auto status = bench.status();
  CHECK(status.control_failure == ControlFailure::Transport &&
            status.control_failed == failed + 1 && status.tx_failures > 0 &&
            bench.tion.writes == 0,
        "a failed state write ends the command as a transport failure");
}

void test_device_info_is_retried() {
  Bench bench;
  bench.tion.answer_device_info = false;
  bench.run(25000);
  CHECK(bench.tion.device_info_requests == 3 && !bench.status().device.has_device_info,
        "unanswered device info is requested again every ten seconds");
}

void test_stall_guard_limits() {
  StallGuard guard;
  std::uint32_t restarts = 0;
  std::uint32_t now = 0xFFFF'FF00u;
  CHECK(guard.evaluate(1000, now, restarts) == StallGuard::Action::None, "healthy");
  for (std::uint32_t i = 1; i <= StallGuard::kMaxConsecutiveRestarts; ++i) {
    StallGuard after_reboot;
    CHECK(after_reboot.evaluate(StallGuard::kHeartbeatStallMs, now, restarts) ==
                  StallGuard::Action::Restart &&
              restarts == i,
          "restarts are counted across reboots");
  }
  CHECK(guard.evaluate(7000, now, restarts) == StallGuard::Action::GiveUp &&
            guard.gave_up(),
        "limit reached: keep running degraded");
  CHECK(guard.evaluate(8000, now, restarts) == StallGuard::Action::None,
        "give-up is reported once");
  CHECK(guard.evaluate(100, now, restarts) == StallGuard::Action::None &&
            guard.evaluate(100, now + 1000, restarts) == StallGuard::Action::None &&
            restarts == StallGuard::kMaxConsecutiveRestarts,
        "a short recovery keeps the count");
  now += StallGuard::kHealthyResetMs;
  CHECK(guard.evaluate(100, now, restarts) == StallGuard::Action::None && restarts == 0 &&
            !guard.gave_up(),
        "a sustained healthy minute clears the count across millis wrap");
}

void test_status_samples_clock_under_lock() {
  Bench bench;
  bench.run(4000);
  bench.tion.on_clock = [&](std::uint32_t) {
    CHECK(bench.lock.depth == 1, "status samples the clock with publication locked");
  };
  const auto status = bench.status();
  CHECK(status.device.fresh && status.device.heartbeat_age_ms < 3100,
        "a fresh publication has a nonnegative bounded age");
  bench.tion.on_clock = nullptr;
  // No service for almost a full clock cycle: saturating age stays stale.
  bench.clock += 0xFFFF'FFF0u;
  const auto old = bench.status();
  CHECK(!old.device.fresh && old.device.age_ms == 0xFFFF'FFFFu &&
            old.device.heartbeat_age_ms == 0xFFFF'FFFFu,
        "long stalls saturate rather than wrapping back to fresh");
}

void test_unsolicited_state_does_not_underflow_read_count() {
  Bench bench;
  bench.tion.firmware = 0x02D2;
  bench.tion.echo_request_ids = true;
  bench.run(4000);
  for (unsigned pass = 0; pass < 100 && bench.status().state_reads_pending != 0; ++pass) {
    bench.run(10);
  }
  CHECK(bench.status().state_reads_pending == 0, "periodic reply drains before injection");
  bench.tion.inject(bench.tion.state_frame(7));
  bench.run(10);
  CHECK(bench.status().state_reads_pending == 0,
        "unsolicited state never invents outstanding reads");
  CHECK(bench.submit(ControlCommand::speed_step(4)), "command starts after unsolicited frame");
  bench.settle_until_idle();
  bench.run(100);  // Drain the periodic poll resumed at transaction completion.
  CHECK(bench.tion.state.fan_speed == 4 && bench.status().control_applied == 1 &&
            bench.status().state_reads_pending == 0,
        "early write ACK neither consumes nor creates a read response");
}

}  // namespace

int main() {
  test_idle_status_is_never_older_than_the_command();
  test_boot_and_heartbeat_priority();
  test_transaction_owns_state_polls();
  test_mailbox_merges_same_group();
  test_recent_results_are_bounded();
  test_failure_drops_waiting_and_stale_callers();
  test_unchanged_is_not_failure();
  test_expired_before_write();
  test_safe_direction_with_device_errors();
  test_idle_flush_recovers_frame_behind_stale_header();
  test_tx_failure_triggers_bounded_restart();
  test_submit_needs_one_group();
  test_receive_budget_bounds_a_pass();
  test_transport_failure_during_control();
  test_device_info_is_retried();
  test_stall_guard_limits();
  test_status_samples_clock_under_lock();
  test_unsolicited_state_does_not_underflow_read_count();
  std::cout << "UART core scheduling and recovery: PASS\n";
}
