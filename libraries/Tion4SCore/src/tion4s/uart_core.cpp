#include "uart_core.h"

#include <limits>

namespace tion4s {
namespace {

class Guard {
 public:
  explicit Guard(StatusLock& lock) : lock_(lock) { lock_.lock(); }
  ~Guard() { lock_.unlock(); }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
  Guard(Guard&&) = delete;
  Guard& operator=(Guard&&) = delete;

 private:
  StatusLock& lock_;
};

std::uint32_t saturating_add(std::uint32_t value, std::uint32_t addend) {
  constexpr auto kMax = std::numeric_limits<std::uint32_t>::max();
  return value > kMax - addend ? kMax : value + addend;
}

}  // namespace

CommandOutcome outcome_of(const UartStatus& status, std::uint32_t ticket) {
  if (ticket == 0) {
    return CommandOutcome::Unknown;
  }
  if (ticket == status.running_ticket || ticket == status.waiting_ticket) {
    return CommandOutcome::Pending;
  }
  for (const auto& result : status.recent) {
    if (result.ticket == ticket) {
      return result.outcome;
    }
  }
  return CommandOutcome::Unknown;
}

bool UartCore::can_wait(const ControlCommand& command) {
  return !command.reset_filter && !command.context.has_value();
}

std::uint32_t UartCore::issue_ticket() {
  // Cycles through 1..2^32-1: 0 means "refused".
  last_ticket_ = last_ticket_ % std::numeric_limits<std::uint32_t>::max() + 1;
  return last_ticket_;
}

void UartCore::record(const CommandResult& result) {
  for (std::size_t i = UartStatus::kRecentResults - 1; i != 0; --i) {
    recent_[i] = recent_[i - 1];
  }
  recent_[0] = result;
}

std::uint32_t UartCore::submit(const ControlCommand& command, bool allow_legacy_02d0,
                               std::uint32_t expected_failures, std::uint32_t now_ms) {
  const auto group = command.group();
  if (group == ChangeGroup::None || group == ChangeGroup::Mixed) {
    return 0;
  }
  const Guard guard(lock_);
  if (expected_failures != failures_) {
    return 0;
  }
  const bool idle = running_ticket_ == 0 && !has_waiting_;
  if (idle || (can_wait(command) && !has_waiting_)) {
    waiting_.command = command;
  } else if (can_wait(command) && can_merge(waiting_.command, command)) {
    record({waiting_.ticket, CommandOutcome::Merged, ControlFailure::None,
            WriteRejection::None});
    waiting_.command = merge(waiting_.command, command);
  } else {
    return 0;
  }
  waiting_.ticket = issue_ticket();
  waiting_.queued_ms = now_ms;
  waiting_.allow_legacy_02d0 = allow_legacy_02d0;
  has_waiting_ = true;
  // Showing more work early is safe; showing less work early is not.
  status_.control_pending = true;
  status_.waiting_ticket = waiting_.ticket;
  for (std::size_t i = 0; i < UartStatus::kRecentResults; ++i) {
    status_.recent[i] = recent_[i];
  }
  return waiting_.ticket;
}

UartStatus UartCore::status(const Clock& clock) const {
  UartStatus result;
  std::uint32_t published_ms = 0;
  std::uint32_t now_ms = 0;
  {
    const Guard guard(lock_);
    result = status_;
    published_ms = published_ms_;
    // A caller may have been preempted before acquiring the lock. Its clock
    // sample must not precede the publication whose age it is measuring.
    now_ms = clock.now_ms();
  }
  const auto extra_age = now_ms - published_ms;
  if (result.device.has_state) {
    result.device.age_ms = saturating_add(result.device.age_ms, extra_age);
    result.device.fresh = result.device.fresh &&
                          result.device.age_ms <= PollingSession::kStateFreshnessMs;
  }
  if (result.device.heartbeat_sent) {
    result.device.heartbeat_age_ms =
        saturating_add(result.device.heartbeat_age_ms, extra_age);
  }
  return result;
}

void UartCore::service(UartPort& port) {
  session_.tick(port.now_ms());
  flush_idle_input(port.now_ms());
  receive(port);
  settle();
  transmit_requests(port);
  start_waiting_command(port.now_ms());
  transmit_control(port);
  settle();
  publish(port.now_ms());
}

void UartCore::flush_idle_input(std::uint32_t now_ms) {
  if (!input_pending_ || now_ms - last_rx_ms_ <= kPartialFrameTimeoutMs) {
    return;
  }
  // Complete frames stuck behind a stale candidate arrived by last_rx_ms_.
  Frame frame;
  while (parser_.flush(frame)) {
    dispatch(frame, last_rx_ms_);
  }
  input_pending_ = false;
}

void UartCore::receive(UartPort& port) {
  Frame frame;
  for (unsigned budget = kReceiveBudget; budget != 0; --budget) {
    const int value = port.read_byte();
    if (value < 0) {
      break;
    }
    last_rx_ms_ = port.now_ms();
    bool received = parser_.push(static_cast<std::uint8_t>(value), frame);
    while (received) {
      dispatch(frame, last_rx_ms_);
      received = parser_.next(frame);
    }
    input_pending_ = !parser_.empty();
  }
}

void UartCore::dispatch(const Frame& frame, std::uint32_t now_ms) {
  State state;
  if (decode_state(frame, state) && state_reads_pending_ != 0) {
    const auto snapshot = session_.snapshot(now_ms);
    // Before identification only read requests have been sent. On 02D0 the
    // read IDs are unknown; on newer firmware do not count write ACKs.
    if (!snapshot.has_device_info ||
        snapshot.device_info.firmware_version == kFirmware02D0 || state.request_id == 1) {
      --state_reads_pending_;
    }
  }
  control_.observe(frame, now_ms);
  session_.accept(frame, now_ms);
}

bool UartCore::command_waiting() {
  const Guard guard(lock_);
  return has_waiting_;
}

void UartCore::transmit_requests(UartPort& port) {
  for (unsigned sent_count = 0; sent_count < 2; ++sent_count) {
    const auto request = session_.next_request(port.now_ms());
    if (request == Request::None) {
      break;
    }
    // A command owns state polls while it runs or waits.
    // Heartbeat remains the highest-priority request.
    if (request != Request::Heartbeat && (control_.busy() || command_waiting())) {
      break;
    }
    std::uint8_t bytes[kRequestFrameSize];
    encode_request(static_cast<FrameType>(request), bytes);
    if (request == Request::State) {
      // Count even a failed TX: it may have reached the device in full.
      state_reads_pending_ = saturating_add(state_reads_pending_, 1);
    }
    if (!port.write_all(bytes, sizeof(bytes))) {
      ++tx_failures_;
      break;
    }
    const auto completed_ms = port.now_ms();
    if (request == Request::Heartbeat && !session_.heartbeat_sent()) {
      first_heartbeat_ms_ = completed_ms;
    }
    session_.sent(request, completed_ms);
  }
}

bool UartCore::take_waiting_command(Pending& next) {
  const Guard guard(lock_);
  if (!has_waiting_) {
    return false;
  }
  next = waiting_;
  has_waiting_ = false;
  running_ticket_ = next.ticket;
  command_running_ = true;
  return true;
}

void UartCore::start_waiting_command(std::uint32_t now_ms) {
  Pending next;
  if (control_.busy() || !take_waiting_command(next)) {
    return;
  }
  // The mailbox time counts toward the intent's lifetime.
  control_.submit(next.command, session_.snapshot(now_ms), next.allow_legacy_02d0,
                  next.queued_ms, now_ms);
  settle();
}

void UartCore::transmit_control(UartPort& port) {
  const auto output = control_.next(port.now_ms());
  if (output.step == ControlStep::None) {
    return;
  }
  if (output.step == ControlStep::Read || output.step == ControlStep::Verify) {
    // All state reads share one stream, including periodic polls and retries
    // from earlier commands. Never let their delayed answers become the
    // baseline or confirmation of a new read. next() still ages the intent.
    if (state_reads_pending_ != 0) {
      return;
    }
    state_reads_pending_ = 1;
  }
  if (!port.write_all(output.bytes, output.size)) {
    ++tx_failures_;
    control_.transport_failed();
    return;
  }
  control_.sent(output.step, port.now_ms());
}

void UartCore::settle() {
  if (!command_running_ || control_.busy()) {
    return;
  }
  command_running_ = false;
  CommandResult result;
  result.failure = control_.failure();
  result.refusal = control_.refusal();
  const Guard guard(lock_);
  result.ticket = running_ticket_;
  running_ticket_ = 0;
  // Not busy: the transaction ended as Applied, Unchanged or Failed.
  if (control_.phase() == ControlPhase::Applied) {
    result.outcome = CommandOutcome::Applied;
    ++applied_;
  } else if (control_.phase() == ControlPhase::Unchanged) {
    result.outcome = CommandOutcome::Unchanged;
    ++unchanged_;
  } else {
    result.outcome = CommandOutcome::Failed;
    ++failures_;
  }
  record(result);
  if (result.outcome == CommandOutcome::Failed && has_waiting_) {
    // The HomeKit lease closes on any failure; nothing queued may follow it.
    record({waiting_.ticket, CommandOutcome::Dropped, ControlFailure::None,
            WriteRejection::None});
    has_waiting_ = false;
  }
  // status_ is left to publish(): "not pending" must never be visible
  // before the device state that the finished command produced.
}

void UartCore::publish(std::uint32_t now_ms) {
  const auto device = session_.snapshot(now_ms);
  const Guard guard(lock_);
  status_.device = device;
  status_.rejected_frames = parser_.rejected_frames();
  status_.tx_failures = tx_failures_;
  status_.first_heartbeat_after_boot_ms = first_heartbeat_ms_;
  status_.state_reads_pending = state_reads_pending_;
  status_.control_command = control_.command();
  status_.control_phase = control_.phase();
  status_.control_failure = control_.failure();
  status_.control_refusal = control_.refusal();
  status_.control_request_id = control_.request_id();
  status_.control_applied = applied_;
  status_.control_unchanged = unchanged_;
  status_.control_failed = failures_;
  status_.control_pending = running_ticket_ != 0 || has_waiting_;
  status_.running_ticket = running_ticket_;
  status_.waiting_ticket = has_waiting_ ? waiting_.ticket : 0;
  for (std::size_t i = 0; i < UartStatus::kRecentResults; ++i) {
    status_.recent[i] = recent_[i];
  }
  published_ms_ = now_ms;
}

}  // namespace tion4s
