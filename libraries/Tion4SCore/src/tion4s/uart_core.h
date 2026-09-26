#pragma once

#include "control.h"
#include "control_transaction.h"
#include "protocol.h"
#include "session.h"

#include <cstddef>
#include <cstdint>

namespace tion4s {

// Sampling this clock must be nonblocking: status() reads it under its lock.
class Clock {
 public:
  virtual std::uint32_t now_ms() const = 0;

 protected:
  ~Clock() = default;
};

// Serial line and clock seen by the single UART owner.
class UartPort : public Clock {
 public:
  // Next received byte, or -1 when nothing is buffered.
  virtual int read_byte() = 0;
  // Sends every byte and waits, bounded, until they have left the UART.
  virtual bool write_all(const std::uint8_t* bytes, std::size_t size) = 0;

 protected:
  ~UartPort() = default;
};

// Short critical section shared by the UART owner and HomeKit callbacks.
class StatusLock {
 public:
  virtual void lock() = 0;
  virtual void unlock() = 0;

 protected:
  ~StatusLock() = default;
};

// Terminal result of one submitted command, identified by its ticket.
enum class CommandOutcome : std::uint8_t {
  Unknown,    // Ticket 0, or too old to be among the recent results.
  Pending,    // Running or waiting.
  Applied,    // Written and confirmed.
  Unchanged,  // The fresh pre-read already had the requested values.
  Failed,     // See failure and refusal.
  Merged,     // Absorbed by a newer command of the same group; that one's
              // ticket carries the result.
  Dropped,    // Discarded unstarted because an earlier command failed.
};

struct CommandResult {
  std::uint32_t ticket = 0;
  CommandOutcome outcome = CommandOutcome::Unknown;
  ControlFailure failure = ControlFailure::None;
  WriteRejection refusal = WriteRejection::None;
};

struct UartStatus {
  static constexpr std::size_t kRecentResults = 4;

  Snapshot device{};
  std::uint32_t rejected_frames = 0;
  std::uint32_t tx_failures = 0;
  std::uint32_t first_heartbeat_after_boot_ms = 0;
  // Replies owed by state reads. Commands cannot start another read until
  // these are drained; a missing reply keeps writes closed, even after retry.
  std::uint32_t state_reads_pending = 0;
  // The last command that reached the transaction and where it stands.
  ControlCommand control_command{};
  ControlPhase control_phase = ControlPhase::Idle;
  ControlFailure control_failure = ControlFailure::None;
  WriteRejection control_refusal = WriteRejection::None;
  std::uint32_t control_request_id = 0;
  std::uint32_t control_applied = 0;
  std::uint32_t control_unchanged = 0;
  // Terminal failures. Every one of them closes the HomeKit lease.
  std::uint32_t control_failed = 0;
  // A command is running or waiting. It turns false only together with the
  // device state read after the command finished.
  bool control_pending = false;
  std::uint32_t running_ticket = 0;
  std::uint32_t waiting_ticket = 0;
  // Newest first.
  CommandResult recent[kRecentResults]{};
};

// Where a submitted command stands in `status`. Unknown means the result is
// no longer tracked; callers must treat it like a failure.
CommandOutcome outcome_of(const UartStatus& status, std::uint32_t ticket);

// What the HomeKit adapter needs from the UART owner.
class ControlChannel {
 public:
  virtual UartStatus status() const = 0;
  // Returns a nonzero ticket when the command was accepted, 0 when refused.
  // `expected_failures` is UartStatus::control_failed from the status the
  // caller based its decision on. A failure the caller has not seen yet
  // refuses the command, so a revoked lease can never queue another write.
  virtual std::uint32_t submit(const ControlCommand& command, bool allow_legacy_02d0,
                               std::uint32_t expected_failures) = 0;

 protected:
  ~ControlChannel() = default;
};

// Owns framing, polling and the control transaction. service() runs on one
// task; submit() and status() may run on any task and only hold the lock
// for short copies. publish() is the only writer of the visible status.
//
// Mailbox: one command runs at a time and one more may wait. A newer command
// of the same group merges into the waiting one (the latest slider position
// wins); any other command is refused. Filter reset and context-bound
// commands never wait behind another command. A terminal failure drops the
// waiting command, because the HomeKit lease closes.
class UartCore {
 public:
  static constexpr std::uint32_t kPartialFrameTimeoutMs = 250;
  static constexpr unsigned kReceiveBudget = 128;

  explicit UartCore(StatusLock& lock) : lock_(lock) {}
  ~UartCore() = default;
  UartCore(const UartCore&) = delete;
  UartCore& operator=(const UartCore&) = delete;
  UartCore(UartCore&&) = delete;
  UartCore& operator=(UartCore&&) = delete;

  // One bounded pass. Heartbeat is sent before anything else that is due.
  void service(UartPort& port);
  std::uint32_t submit(const ControlCommand& command, bool allow_legacy_02d0,
                       std::uint32_t expected_failures, std::uint32_t now_ms);
  UartStatus status(const Clock& clock) const;

 private:
  struct Pending {
    ControlCommand command{};
    std::uint32_t ticket = 0;
    std::uint32_t queued_ms = 0;
    bool allow_legacy_02d0 = false;
  };

  static bool can_wait(const ControlCommand& command);

  void flush_idle_input(std::uint32_t now_ms);
  void receive(UartPort& port);
  void dispatch(const Frame& frame, std::uint32_t now_ms);
  void transmit_requests(UartPort& port);
  bool take_waiting_command(Pending& next);
  void start_waiting_command(std::uint32_t now_ms);
  void transmit_control(UartPort& port);
  void settle();
  bool command_waiting();
  void publish(std::uint32_t now_ms);
  // Callers hold the lock.
  std::uint32_t issue_ticket();
  void record(const CommandResult& result);

  StatusLock& lock_;

  // Owned by the service task.
  FrameParser parser_;
  PollingSession session_;
  ControlTransaction control_;
  std::uint32_t last_rx_ms_ = 0;
  std::uint32_t tx_failures_ = 0;
  std::uint32_t first_heartbeat_ms_ = 0;
  std::uint32_t state_reads_pending_ = 0;
  bool input_pending_ = false;
  bool command_running_ = false;

  // Guarded by lock_.
  UartStatus status_{};
  std::uint32_t published_ms_ = 0;
  Pending waiting_{};
  bool has_waiting_ = false;
  std::uint32_t running_ticket_ = 0;
  std::uint32_t last_ticket_ = 0;
  std::uint32_t applied_ = 0;
  std::uint32_t unchanged_ = 0;
  std::uint32_t failures_ = 0;
  CommandResult recent_[UartStatus::kRecentResults]{};
};

}  // namespace tion4s
