// UartCore with two real threads, built with ThreadSanitizer: the UART owner
// runs service() while a "HomeKit" thread submits commands and reads status.
// TSan reports data races; the checks below catch logical interleavings.

#include "support/check.h"
#include "support/fake_tion.h"
#include "tion4s/uart_core.h"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

class MutexLock final : public tion4s::StatusLock {
 public:
  void lock() override { mutex_.lock(); }
  void unlock() override { mutex_.unlock(); }

 private:
  std::mutex mutex_;
};

constexpr std::uint32_t kSimulatedMs = 600000;

class AtomicClock final : public tion4s::Clock {
 public:
  explicit AtomicClock(const std::atomic<std::uint32_t>& value) : value_(value) {}
  std::uint32_t now_ms() const override { return value_.load(); }

 private:
  const std::atomic<std::uint32_t>& value_;
};

}  // namespace

int main() {
  MutexLock lock;
  tion4s::UartCore core{lock};
  std::atomic<std::uint32_t> clock_mirror{1000};
  AtomicClock shared_clock(clock_mirror);
  std::atomic<bool> heater_mirror{false};
  std::atomic<bool> stop{false};

  // Everything the fake device owns stays on the UART thread.
  std::thread uart([&] {
    std::uint32_t clock = clock_mirror.load();
    fake::FakeTion tion{clock};
    tion.on_clock = [&](std::uint32_t now) { clock_mirror.store(now); };
    while (!stop.load()) {
      core.service(tion);
      clock += 10;
      heater_mirror.store(tion.state.heater_allowed);
      clock_mirror.store(clock);
      std::this_thread::yield();
    }
  });

  unsigned idle_checks = 0;
  unsigned submitted = 0;
  bool heat = true;
  while (clock_mirror.load() < 1000 + kSimulatedMs) {
    const auto now = clock_mirror.load();
    const auto status = core.status(shared_clock);
    CHECK(status.device.heartbeat_age_ms < 5000, "concurrent status never invents a stall");
    if (!status.control_pending && status.device.has_state &&
        status.control_applied > 0) {
      ++idle_checks;
      CHECK(status.device.state.heater_allowed == heater_mirror.load(),
            "an idle status always shows the device state after the last command");
    }
    if (!status.control_pending && status.device.fresh && status.device.has_device_info &&
        core.submit(tion4s::ControlCommand::heater(heat), true, status.control_failed,
                    now) != 0) {
      ++submitted;
      heat = !heat;
    }
    std::this_thread::yield();
  }
  stop.store(true);
  uart.join();
  CHECK(submitted > 20 && idle_checks > 100, "both threads made progress");
  std::cout << "UART core under two threads: PASS\n";
}
