#include "alert.h"

namespace core {

AlertWindow computeWindow(Timestamp departure, const AlertConfig& c) {
    AlertWindow w;
    if (!departure.valid()) return w;
    w.leave = addMinutes(departure, -c.safetyBufferMin);
    w.alertStart = addMinutes(w.leave, -c.leadMin);
    w.alertEnd = addMinutes(w.leave, c.graceMin);
    w.valid = true;
    return w;
}

bool inQuietHours(Timestamp t, const AlertConfig& c) {
    if (c.quietStartMin < 0 || c.quietEndMin < 0 || !t.valid()) return false;
    if (c.quietStartMin == c.quietEndMin) return false;   // zero-width window

    int32_t tod = t.min % 1440;
    if (tod < 0) tod += 1440;

    if (c.quietStartMin < c.quietEndMin)
        return tod >= c.quietStartMin && tod < c.quietEndMin;
    // Wraps midnight.
    return tod >= c.quietStartMin || tod < c.quietEndMin;
}

AlertStatus evaluate(Timestamp now, Timestamp departure, const AlertConfig& c) {
    AlertStatus s;
    s.window = computeWindow(departure, c);
    if (!s.window.valid || !now.valid()) return s;

    s.minutesToLeave = diffMinutes(s.window.leave, now);

    if (now < s.window.alertStart) { s.state = AlertState::Idle;   return s; }
    if (now > s.window.alertEnd)   { s.state = AlertState::Passed; return s; }

    // Quiet hours suppress the takeover but not the arithmetic, so the caller
    // can still surface it on the OLED if it wants to.
    s.state = inQuietHours(now, c) ? AlertState::Idle : AlertState::Armed;
    return s;
}

} // namespace core
