// alert.h - when to take over the screen, and what to count down to.
//
//   leave_time  = effective departure of the chosen trip - safety buffer
//   alert_start = leave_time - leadMin        (default 30)
//   alert_end   = leave_time + graceMin       (default 5)
//
// State is derived from the wall clock on every tick rather than driven by
// timers. That is what lets the device recover correctly from a reboot or a
// missed tick: it recomputes where it should be, instead of trying to
// remember where it was.
#pragma once
#include "types.h"

namespace core {

enum class AlertState : uint8_t { Idle, Armed, Passed };

struct AlertConfig {
    int32_t leadMin = 30;        // how long before leave_time to take the screen
    int32_t graceMin = 5;        // how long after leave_time to keep showing it
    int32_t safetyBufferMin = 5; // padding subtracted from the departure
    int32_t quietStartMin = -1;  // minutes past midnight; -1 disables quiet hours
    int32_t quietEndMin = -1;
};

struct AlertWindow {
    Timestamp leave, alertStart, alertEnd;
    bool valid = false;
};

AlertWindow computeWindow(Timestamp departure, const AlertConfig& c);

// True when `t` falls inside the configured quiet window. Handles windows that
// wrap past midnight (e.g. 23:00 -> 07:00), which is the normal case.
bool inQuietHours(Timestamp t, const AlertConfig& c);

struct AlertStatus {
    AlertState state = AlertState::Idle;
    int32_t minutesToLeave = 0;  // negative once leave_time has passed
    AlertWindow window;
};

AlertStatus evaluate(Timestamp now, Timestamp departure, const AlertConfig& c);

} // namespace core
