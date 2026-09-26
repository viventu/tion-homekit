#pragma once

#include "control.h"
#include "session.h"

#include <cstddef>
#include <cstdint>

namespace tion4s {

// Applied: the write was confirmed. Unchanged: the fresh pre-read already had
// the requested values, so nothing was written. Both are terminal successes.
enum class ControlPhase : std::uint8_t {
  Idle,
  Queued,
  Reading,
  Writing,
  Verifying,
  Applied,
  Unchanged,
  Failed,
};

enum class ControlStep : std::uint8_t { None, Read, Write, Verify };

enum class ControlFailure : std::uint8_t {
  None,
  UnsupportedDevice,  // Not a 4S in normal mode, or firmware without opt-in.
  StaleState,         // The snapshot or the pre-read is too old to write from.
  ContextChanged,     // A context-bound command found other reported values.
  Refused,            // The encoder refused the write; see refusal().
  ReadTimeout,        // The pre-read got no response.
  VerifyTimeout,      // The write was sent but never confirmed.
  NotApplied,         // Verification reads still show the old values.
  Transport,          // UART transmission failed.
  Expired,            // The intent aged out before its write was sent.
};

struct ControlOutput {
  ControlStep step = ControlStep::None;
  std::uint8_t bytes[kStateWriteFrameSize]{};
  std::size_t size = 0;
};

// One full-state write at a time. A preflight read supplies the complete
// baseline. The write is sent at most once, then verified by a fresh read.
// Firmware 02D0 has unknown response IDs: its explicit opt-in uses ordered
// read responses, never an assumed request-ID match.
//
// Driving loop: call next(); transmit a non-None output, then report it with
// sent(), or call transport_failed() if transmission failed; feed every
// received frame to observe().
class ControlTransaction {
 public:
  // Oldest reported state accepted when an intent is admitted.
  static constexpr std::uint32_t kMaxSnapshotAgeMs = 3000;
  // Lifetime of an intent, including time spent waiting in the mailbox.
  static constexpr std::uint32_t kIntentMaxAgeMs = 4000;

  // `queued_ms` is when the intent entered the mailbox; its lifetime starts
  // there. The short form admits an intent that was queued just now. The
  // write itself is decided by the fresh pre-read, not by the snapshot.
  bool submit(const ControlCommand& command, const Snapshot& snapshot,
              bool allow_legacy_02d0, std::uint32_t queued_ms, std::uint32_t now_ms);
  bool submit(const ControlCommand& command, const Snapshot& snapshot,
              bool allow_legacy_02d0, std::uint32_t now_ms) {
    return submit(command, snapshot, allow_legacy_02d0, now_ms, now_ms);
  }
  ControlOutput next(std::uint32_t now_ms);
  void sent(ControlStep step, std::uint32_t now_ms);
  void observe(const Frame& frame, std::uint32_t now_ms);
  void transport_failed();

  bool busy() const;
  ControlPhase phase() const { return phase_; }
  ControlFailure failure() const { return failure_; }
  WriteRejection refusal() const { return refusal_; }
  const ControlCommand& command() const { return command_; }
  std::uint32_t request_id() const { return request_id_; }

 private:
  static constexpr std::uint32_t kReadTimeoutMs = 1000;
  static constexpr std::uint32_t kBaselineMaxAgeMs = 500;
  static constexpr std::uint32_t kWriteSettleMs = 180;
  static constexpr std::uint32_t kVerifyIntervalMs = 500;
  static constexpr std::uint8_t kMaxVerifyReads = 3;
  static constexpr std::uint32_t kFilterResetEvidenceSeconds = 60;

  bool matches(const State& state) const;
  bool context_matches(const State& state) const;
  bool write_sent() const;
  ControlOutput read_request(ControlStep step) const;
  ControlOutput write_from_baseline(std::uint32_t now_ms);
  void fail(ControlFailure reason, WriteRejection refusal = WriteRejection::None);

  ControlCommand command_{};
  State baseline_{};
  ControlPhase phase_ = ControlPhase::Idle;
  ControlFailure failure_ = ControlFailure::None;
  WriteRejection refusal_ = WriteRejection::None;
  std::uint32_t queued_ms_ = 0;
  std::uint32_t sent_ms_ = 0;
  std::uint32_t baseline_ms_ = 0;
  std::uint32_t request_id_ = 1;
  bool legacy_02d0_ = false;
  bool baseline_ready_ = false;
  std::uint8_t verify_reads_ = 0;
};

}  // namespace tion4s
