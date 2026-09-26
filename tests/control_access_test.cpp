// Automatic HomeKit access rules against the real UartCore and a simulated breezer.

#include "ControlAccess.h"
#include "support/check.h"
#include "support/fake_tion.h"
#include "support/host_channel.h"

#include <iostream>

namespace {

struct Bench {
  fake::FakeTion tion{fake_millis};
  fake::NoLock lock;
  tion4s::UartCore core{lock};
  fake::HostChannel channel{core, fake_millis};
  tion_homekit::ControlAccess access{channel};

  void run(std::uint32_t ms) { fake::run(core, tion, fake_millis, ms); }
  void settle() {
    for (int i = 0; i < 1000 && core.status(tion).control_pending; ++i) {
      run(10);
    }
  }
};

void test_automatic_access_and_mailbox() {
  fake_millis = 1000;
  Bench bench;
  bench.run(4000);
  CHECK(bench.access.submit(tion4s::ControlCommand::heater(true)),
        "heater enable is queued");
  CHECK(
      bench.access.submit(tion4s::ControlCommand::heater(false)) &&
          bench.channel.submitted == 2,
      "opposite command during a write is sent to the mailbox, not answered as a no-op");
  bench.settle();
  CHECK(!bench.tion.state.heater_allowed && bench.access.available(bench.channel.status()),
        "the latest HomeKit value wins and control stays available");

  const auto writes = bench.tion.writes;
  const auto unchanged = bench.core.status(bench.tion).control_unchanged;
  CHECK(bench.access.submit(tion4s::ControlCommand::heater(false)),
        "a command matching the snapshot is still accepted");
  bench.settle();
  CHECK(bench.tion.writes == writes &&
            bench.core.status(bench.tion).control_unchanged == unchanged + 1 &&
            bench.access.available(bench.channel.status()),
        "the fresh pre-read confirms it without a write");
}

// Heat ON is running; heat OFF arrives from another core right after ON
// finished, while the UART task is still in the same pass.
void test_opposite_command_right_after_completion_is_written() {
  fake_millis = 20000;
  Bench bench;
  bench.run(4000);
  CHECK(bench.access.submit(tion4s::ControlCommand::heater(true)), "heat on accepted");
  bool tapped = false;
  bool accepted = false;
  bench.tion.on_write = [&] {
    if (tapped || !bench.tion.state.heater_allowed) {
      return;
    }
    // First UART write after heat ON reached the device.
    const auto status = bench.core.status(bench.tion);
    if (status.control_pending && status.control_applied == 0) {
      return;
    }
    tapped = true;
    accepted = bench.access.submit(tion4s::ControlCommand::heater(false)) != 0;
  };
  bench.run(3000);
  bench.settle();
  CHECK(tapped && accepted, "heat off was submitted after heat on finished");
  CHECK(!bench.tion.state.heater_allowed,
        "heat off is written instead of being acknowledged as a no-op");
}

void test_new_commands_after_failures() {
  fake_millis = 5000;
  Bench bench;
  bench.run(4000);
  bench.tion.apply_writes = false;
  CHECK(bench.access.submit(tion4s::ControlCommand::sound(false)), "command accepted");
  bench.settle();
  CHECK(bench.core.status(bench.tion).control_failed == 1, "first failure recorded");
  CHECK(bench.access.submit(tion4s::ControlCommand::sound(false)), "second command");
  bench.settle();
  CHECK(bench.core.status(bench.tion).control_failed == 2, "second failure recorded");
  bench.tion.apply_writes = true;
  CHECK(bench.access.submit(tion4s::ControlCommand::sound(false)), "new explicit retry");
  bench.settle();
  CHECK(!bench.tion.state.sound_on && bench.core.status(bench.tion).control_applied == 1,
        "a new command succeeds without a gate or an automatic write retry");
}

void test_concurrent_failure_cannot_slip_through() {
  fake_millis = 9000;
  Bench bench;
  bench.run(4000);
  bench.tion.apply_writes = false;
  CHECK(bench.access.submit(tion4s::ControlCommand::led(false)),
        "first command in flight");
  bench.run(20);
  // The status read sees a command pending. Before the
  // submit lands, the UART task finishes that command with a failure.
  bench.channel.after_status = [&bench] {
    bench.settle();
    bench.tion.apply_writes = true;
  };
  const auto writes = bench.tion.writes + 1;  // The in-flight write still goes out.
  CHECK(!bench.access.submit(tion4s::ControlCommand::led(true)),
        "a failure between status read and submit rejects the command");
  bench.settle();
  CHECK(bench.tion.writes == writes,
        "no stale submission follows the concurrent failure");
}

void test_access_does_not_expire() {
  fake_millis = 0xFFFF'FFF0u - 5000;
  Bench bench;
  bench.run(5000);
  bench.run(31 * 60 * 1000);
  CHECK(bench.access.submit(tion4s::ControlCommand::sound(false)),
        "control remains available after thirty minutes and clock wrap");
  bench.settle();
  CHECK(!bench.tion.state.sound_on, "the new command is applied");
}

void test_access_needs_a_known_device() {
  fake_millis = 40000;
  Bench bench;
  bench.tion.answer_device_info = false;
  bench.run(4000);
  CHECK(!bench.access.submit(tion4s::ControlCommand::sound(false)),
        "no control before the device identified itself");
  bench.tion.answer_device_info = true;
  bench.tion.work_mode = 2;
  bench.run(11000);
  CHECK(bench.core.status(bench.tion).device.has_device_info &&
            !bench.access.submit(tion4s::ControlCommand::sound(false)),
        "no control outside the normal work mode");
}

void test_submit_needs_a_fresh_state() {
  fake_millis = 60000;
  Bench bench;
  bench.run(4000);
  bench.tion.respond = false;
  bench.run(4000);
  CHECK(
      !bench.access.submit(tion4s::ControlCommand::sound(false)),
      "a state older than three seconds cannot start a command");
  bench.run(8000);
  CHECK(
      !bench.access.submit(tion4s::ControlCommand::sound(false)),
      "a stale state cannot start a command");
  CHECK(bench.channel.submitted == 0, "nothing reached the UART owner");
}

// ControlChannel is an interface; ControlAccess must not trust that an
// implementation keeps device information once it reported it.
class ScriptedChannel final : public tion4s::ControlChannel {
 public:
  tion4s::UartStatus status() const override { return next; }
  std::uint32_t submit(const tion4s::ControlCommand&, bool, std::uint32_t) override {
    ++submitted;
    return 1;
  }

  tion4s::UartStatus next{};
  unsigned submitted = 0;
};

void test_channel_that_forgets_the_device() {
  fake_millis = 80000;
  ScriptedChannel channel;
  auto& device = channel.next.device;
  device.has_device_info = true;
  device.device_info.device_type = tion4s::kDeviceType4S;
  device.device_info.work_mode = tion4s::kWorkModeNormal;
  device.device_info.firmware_version = tion4s::kFirmware02D0;
  device.has_state = true;
  device.fresh = true;
  tion_homekit::ControlAccess access(channel);
  CHECK(access.submit(tion4s::ControlCommand::sound(false)) == 1,
        "a scripted device can be controlled immediately");
  device.has_state = false;
  CHECK(!access.submit(tion4s::ControlCommand::sound(false)), "no state means no write");
  device.has_state = true;
  device.device_info.firmware_version = 0x03CD;
  CHECK(!access.submit(tion4s::ControlCommand::sound(false)), "unsupported firmware");
  device.device_info.firmware_version = tion4s::kFirmware02D0;
  device.has_device_info = false;
  CHECK(access.submit(tion4s::ControlCommand::sound(false)) == 0 &&
            channel.submitted == 1,
        "without device information no command is submitted");
}

}  // namespace

int main() {
  test_automatic_access_and_mailbox();
  test_opposite_command_right_after_completion_is_written();
  test_new_commands_after_failures();
  test_concurrent_failure_cannot_slip_through();
  test_access_does_not_expire();
  test_access_needs_a_known_device();
  test_submit_needs_a_fresh_state();
  test_channel_that_forgets_the_device();
  std::cout << "HomeKit control access: PASS\n";
}
