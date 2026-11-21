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

CommandResult result_of(const UartStatus& status, std::uint32_t ticket) {
  CommandResult result;
  result.ticket = ticket;
  if (ticket == 0) {
    return result;
  }
  if (ticket == status.running_ticket || ticket == status.waiting_ticket) {
    result.outcome = CommandOutcome::Pending;
    return result;
  }
  for (const auto& recent : status.recent) {
    if (recent.ticket == ticket) {
      return recent;
    }
  }
  return result;
}

CommandOutcome outcome_of(const UartStatus& status, std::uint32_t ticket) {
  return result_of(status, ticket).outcome;
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

bool UartCore::cancel(std::uint32_t ticket) {
  if (ticket == 0) return false;
  const Guard guard(lock_);
  if (has_waiting_ && waiting_.ticket == ticket) {
    has_waiting_ = false;
    record({ticket, CommandOutcome::Dropped, ControlFailure::None, WriteRejection::None});
    // The ticket's own result may show at once. control_pending is left to
    // publish(): a command that finished in this pass may not be published.
    status_.waiting_ticket = 0;
    for (std::size_t i = 0; i < UartStatus::kRecentResults; ++i) {
      status_.recent[i] = recent_[i];
    }
    return true;
  }
  if (running_ticket_ == ticket) {
    cancel_requested_ticket_ = ticket;
    return true;
  }
  return false;
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
  expire_lost_state_reads(port.now_ms());
  cancel_running_before_write();
  settle();
  transmit_requests(port);
  start_waiting_command(port.now_ms());
  cancel_running_before_write();
  settle();
  transmit_control(port);
  settle();
  publish(port.now_ms());
}

void UartCore::cancel_running_before_write() {
  std::uint32_t requested = 0;
  {
    const Guard guard(lock_);
    requested = cancel_requested_ticket_ == running_ticket_ ? cancel_requested_ticket_ : 0;
    cancel_requested_ticket_ = 0;
  }
  if (requested != 0) control_.cancel_before_write();
}

void UartCore::expire_lost_state_reads(std::uint32_t now_ms) {
  if (state_reads_pending_ != 0 && now_ms - last_state_read_ms_ > kLostStateReplyMs) {
    lost_state_replies_ = saturating_add(lost_state_replies_, state_reads_pending_);
    state_reads_pending_ = 0;
  }
}

void UartCore::count_state_read(std::uint32_t now_ms) {
  // Count even a failed TX: it may have reached the device in full.
  state_reads_pending_ = saturating_add(state_reads_pending_, 1);
  last_state_read_ms_ = now_ms;
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
    // Before identification only read requests have been sent. 02D0 and 04C7
    // answer reads with IDs that do not identify the request, so their replies
    // are counted in order. Other firmware answers reads with ID 1 and a write
    // with the write's own ID; do not count write ACKs there.
    if (!snapshot.has_device_info ||
        firmware_has_unknown_read_ids(snapshot.device_info.firmware_version) ||
        state.request_id == 1) {
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
      count_state_read(port.now_ms());
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
    // baseline of a new pre-read; next() still ages the intent. Verification
    // may overlap its own earlier reads: every answer to them was produced
    // after the write, so a late one is still valid evidence.
    if (output.step == ControlStep::Read && state_reads_pending_ != 0) {
      return;
    }
    count_state_read(port.now_ms());
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
  } else if (control_.phase() == ControlPhase::Cancelled) {
    result.outcome = CommandOutcome::Dropped;
  } else {
    result.outcome = CommandOutcome::Failed;
  }
  // Automation follows its own ticket. Its failure neither refuses stale
  // callers nor drops the person's command waiting behind it.
  const bool failed = result.outcome == CommandOutcome::Failed;
  // A refusal never reached the breezer: it is the project's own safety or
  // validity decision, not a failed transaction. Counting it apart keeps a
  // deliberate refusal from refusing later commands or failing a soak run.
  const bool refused = failed && result.failure == ControlFailure::Refused;
  const bool context_bound = control_.command().context.has_value();
  const bool counted = failed && !refused && !context_bound;
  if (refused) {
    ++refused_;
    if (result.refusal == WriteRejection::DeviceErrors ||
        result.refusal == WriteRejection::InvalidBaseline) {
      ++device_refused_;
    }
  } else if (counted) {
    ++failures_;
  } else if (failed && result.failure != ControlFailure::ContextChanged) {
    ++context_failures_;
  }
  record(result);
  if (counted && has_waiting_) {
    // Queued while the device still looked controllable.
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
  status_.lost_state_replies = lost_state_replies_;
  status_.control_command = control_.command();
  status_.control_phase = control_.phase();
  status_.control_failure = control_.failure();
  status_.control_refusal = control_.refusal();
  status_.control_request_id = control_.request_id();
  status_.control_applied = applied_;
  status_.control_unchanged = unchanged_;
  status_.control_failed = failures_;
  status_.context_failed = context_failures_;
  status_.control_refused = refused_;
  status_.device_refused = device_refused_;
  status_.control_pending = running_ticket_ != 0 || has_waiting_;
  status_.running_ticket = running_ticket_;
  status_.waiting_ticket = has_waiting_ ? waiting_.ticket : 0;
  for (std::size_t i = 0; i < UartStatus::kRecentResults; ++i) {
    status_.recent[i] = recent_[i];
  }
  published_ms_ = now_ms;
}

}  // namespace tion4s
