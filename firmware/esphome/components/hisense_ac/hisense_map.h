#pragma once
// The pure layer of the component in one include: everything here is plain integers and structs,
// no ESPHome types, so it compiles and is tested on a host (firmware/test/).
//
//   hisense_mapping.h    climate enums, mode, swing, fan ladder, setpoint, command shadow rules
//   hisense_special.h    special modes: presets, the plan of writes, the queue
//   hisense_confirm.h    confirm and retry: did a command take
//   hisense_telemetry.h  power estimate, publish pacing, the outdoor temperature placeholder
//
// The scheduler's own command rules (COMMAND_SENDS_MAX, command_resend_allowed,
// command_reply_verdict) are in hisense_bus.h, and the command tracker that carries the decisions
// out is in hisense_commands.h.

#include "hisense_bus.h"
#include "hisense_confirm.h"
#include "hisense_mapping.h"
#include "hisense_protocol.h"
#include "hisense_special.h"
#include "hisense_telemetry.h"
