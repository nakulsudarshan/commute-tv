#include "test.h"
#include "../src/core/alert.h"
using namespace core;

static Timestamp T(const char* hhmm) { return parseDateTime("2026-08-13", hhmm); }

void test_alert() {
    t::suite("alert window");
    AlertConfig c;   // lead 30, grace 5, buffer 5

    {
        AlertWindow w = computeWindow(T("14:50"), c);
        CHECK(w.valid);
        CHECK_EQ(w.leave.min,      T("14:45").min);   // departure - buffer
        CHECK_EQ(w.alertStart.min, T("14:15").min);   // leave - 30
        CHECK_EQ(w.alertEnd.min,   T("14:50").min);   // leave + 5
    }
    CHECK(!computeWindow(invalidTs(), c).valid);

    t::suite("alert state machine");
    // Before the window.
    CHECK(evaluate(T("14:00"), T("14:50"), c).state == AlertState::Idle);
    // Exactly at alert_start the screen is taken -- boundaries are inclusive.
    CHECK(evaluate(T("14:15"), T("14:50"), c).state == AlertState::Armed);
    CHECK(evaluate(T("14:30"), T("14:50"), c).state == AlertState::Armed);
    // Grace period still counts as armed.
    CHECK(evaluate(T("14:50"), T("14:50"), c).state == AlertState::Armed);
    // Past the grace period the screen goes back.
    CHECK(evaluate(T("14:51"), T("14:50"), c).state == AlertState::Passed);

    // Countdown is measured to leave time, not departure, and goes negative
    // during the grace period rather than clamping to zero.
    CHECK_EQ(evaluate(T("14:33"), T("14:50"), c).minutesToLeave, 12);
    CHECK_EQ(evaluate(T("14:48"), T("14:50"), c).minutesToLeave, -3);

    // A reboot mid-window must land back in Armed purely from the clock.
    CHECK(evaluate(T("14:40"), T("14:50"), c).state == AlertState::Armed);

    t::suite("quiet hours");
    AlertConfig q = c;
    q.quietStartMin = 23 * 60;   // 23:00
    q.quietEndMin   = 7 * 60;    // 07:00  (wraps midnight)
    CHECK(inQuietHours(T("23:30"), q));
    CHECK(inQuietHours(T("02:00"), q));
    CHECK(inQuietHours(T("06:59"), q));
    CHECK(!inQuietHours(T("07:00"), q));
    CHECK(!inQuietHours(T("14:00"), q));

    // Non-wrapping window.
    AlertConfig q2 = c;
    q2.quietStartMin = 9 * 60;
    q2.quietEndMin = 17 * 60;
    CHECK(inQuietHours(T("12:00"), q2));
    CHECK(!inQuietHours(T("08:00"), q2));
    CHECK(!inQuietHours(T("18:00"), q2));

    // Disabled by default, and a zero-width window is not "always quiet".
    CHECK(!inQuietHours(T("03:00"), c));
    AlertConfig q3 = c; q3.quietStartMin = q3.quietEndMin = 600;
    CHECK(!inQuietHours(T("10:00"), q3));

    // Quiet hours suppress the takeover but the arithmetic still runs, so the
    // OLED can show it even while the SmallTV is left alone.
    {
        AlertStatus s = evaluate(T("02:00"), T("02:20"), q);
        CHECK(s.state == AlertState::Idle);
        CHECK(s.window.valid);
        CHECK_EQ(s.minutesToLeave, 15);
    }
}
