#pragma once

#include <Arduino.h>
#include <tion4s/uart_core.h>

namespace tion_homekit {

// HomeKit writes are available whenever the supported device has fresh state.
// A failed transaction does not require manual rearming; the UART owner still
// rejects stale submissions and drops commands queued before a failure.
class ControlAccess {
 public:
  explicit ControlAccess(tion4s::ControlChannel& channel) : channel_(channel) {}
  ~ControlAccess() = default;
  ControlAccess(const ControlAccess&) = delete;
  ControlAccess& operator=(const ControlAccess&) = delete;
  ControlAccess(ControlAccess&&) = delete;
  ControlAccess& operator=(ControlAccess&&) = delete;

  bool available(const tion4s::UartStatus& status) const {
    const auto& snapshot = status.device;
    return snapshot.has_device_info && snapshot.has_state && snapshot.fresh &&
           snapshot.age_ms <= tion4s::ControlTransaction::kMaxSnapshotAgeMs &&
           tion4s::is_4s_normal_mode(snapshot.device_info) &&
           tion4s::firmware_allows_control(snapshot.device_info.firmware_version, true);
  }

  // Returns the command's ticket, or 0 when it was refused.
  std::uint32_t submit(const tion4s::ControlCommand& command) {
    const auto status = channel_.status();
    if (!available(status)) {
      return 0;
    }
    const auto& snapshot = status.device;
    // Never answer "already set" from the snapshot: it can be seconds old.
    // The UART pre-read decides, and a match ends as Unchanged without a write.
    return channel_.submit(command,
                           snapshot.device_info.firmware_version == tion4s::kFirmware02D0,
                           status.control_failed);
  }

  tion4s::UartStatus status() const { return channel_.status(); }

 private:
  tion4s::ControlChannel& channel_;
};

}  // namespace tion_homekit
