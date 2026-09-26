#pragma once

#include "control.h"
#include "control_transaction.h"

namespace tion4s {

// Short English labels for diagnostics. Never null.
const char* describe(ControlPhase phase);
const char* describe(ControlFailure failure);
const char* describe(WriteRejection rejection);
// Names the fields a command changes, for example "power and speed".
const char* describe(const ControlCommand& command);

}  // namespace tion4s
