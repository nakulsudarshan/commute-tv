// The one piece of real arithmetic in the platform layer, tested on the laptop.
//
// It exists because the ESP32's newlib has no tm_gmtoff -- the field the obvious
// implementation would have used, and the one that made the first firmware build
// fail. Deriving the offset instead means arithmetic, and arithmetic across a
// day boundary at New Year is exactly the kind that looks right and is not.
#include "test.h"
#include "../firmware/src/platform/tz_offset.h"

#include <cstdlib>
#include <cstring>
#include <ctime>

using platform::utcOffsetMinutes;
using platform::utcOffsetMinutesAt;

namespace {

// Build a std::tm with every field set, so -Wmissing-field-initializers stays
// quiet and nothing depends on an uninitialised member.
std::tm at(int year, int yday, int hour, int min) {
    std::tm t{};
    std::memset(&t, 0, sizeof t);
    t.tm_year = year - 1900;
    t.tm_yday = yday;
    t.tm_hour = hour;
    t.tm_min = min;
    return t;
}

// Ask the host's real timezone database, so the DST rule is checked against
// something authoritative rather than against my reading of it.
int16_t offsetIn(const char* tz, int year, int mon1, int day, int hour) {
    setenv("TZ", tz, 1);
    tzset();
    std::tm local{};
    std::memset(&local, 0, sizeof local);
    local.tm_year = year - 1900;
    local.tm_mon = mon1 - 1;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_isdst = -1;
    const std::time_t t = mktime(&local);
    return utcOffsetMinutesAt(t);
}

} // namespace

void test_tz_offset() {
    t::suite("tz offset / the same day");
    // Berlin in winter and in summer -- the two the device actually lives in.
    CHECK_EQ(utcOffsetMinutes(at(2026, 14, 13, 0), at(2026, 14, 12, 0)), 60);
    CHECK_EQ(utcOffsetMinutes(at(2026, 236, 14, 0), at(2026, 236, 12, 0)), 120);
    CHECK_EQ(utcOffsetMinutes(at(2026, 100, 12, 0), at(2026, 100, 12, 0)), 0);
    // Half-hour and three-quarter-hour zones exist and are not exotic.
    CHECK_EQ(utcOffsetMinutes(at(2026, 100, 17, 30), at(2026, 100, 12, 0)), 330);
    CHECK_EQ(utcOffsetMinutes(at(2026, 100, 17, 45), at(2026, 100, 12, 0)), 345);

    t::suite("tz offset / across a day boundary");
    // Local is already tomorrow.
    CHECK_EQ(utcOffsetMinutes(at(2026, 101, 1, 0), at(2026, 100, 23, 0)), 120);
    // Local is still yesterday -- a negative offset.
    CHECK_EQ(utcOffsetMinutes(at(2026, 100, 20, 0), at(2026, 101, 1, 0)), -300);

    t::suite("tz offset / across New Year, where the day numbers jump 364");
    {
        // Local 1 Jan 00:30, UTC still 31 Dec 11:30 -- Auckland in January.
        // A naive yday subtraction gives -364 days, not -1, and the answer
        // comes out roughly a year wrong.
        CHECK_EQ(utcOffsetMinutes(at(2026, 0, 0, 30), at(2025, 364, 11, 30)), 780);
        // And the other way: local 31 Dec 23:00, UTC already 1 Jan 09:00.
        CHECK_EQ(utcOffsetMinutes(at(2026, 364, 23, 0), at(2027, 0, 9, 0)), -600);
        // A leap year, where 31 December is day 365 rather than 364.
        CHECK_EQ(utcOffsetMinutes(at(2029, 0, 1, 0), at(2028, 365, 12, 0)), 780);
    }

    t::suite("tz offset / against a real timezone database");
    {
        // Berlin. The device's own zone, either side of both switches.
        CHECK_EQ(offsetIn("CET-1CEST,M3.5.0/2,M10.5.0/3", 2026, 1, 15, 12), 60);
        CHECK_EQ(offsetIn("CET-1CEST,M3.5.0/2,M10.5.0/3", 2026, 8, 25, 12), 120);
        // Summer time starts on the last Sunday in March (29 March 2026) and
        // ends on the last Sunday in October (25 October 2026).
        CHECK_EQ(offsetIn("CET-1CEST,M3.5.0/2,M10.5.0/3", 2026, 3, 28, 12), 60);
        CHECK_EQ(offsetIn("CET-1CEST,M3.5.0/2,M10.5.0/3", 2026, 3, 30, 12), 120);
        CHECK_EQ(offsetIn("CET-1CEST,M3.5.0/2,M10.5.0/3", 2026, 10, 24, 12), 120);
        CHECK_EQ(offsetIn("CET-1CEST,M3.5.0/2,M10.5.0/3", 2026, 10, 26, 12), 60);

        // A negative offset, and one that changes with DST.
        CHECK_EQ(offsetIn("EST5EDT,M3.2.0,M11.1.0", 2026, 1, 15, 12), -300);
        CHECK_EQ(offsetIn("EST5EDT,M3.2.0,M11.1.0", 2026, 8, 25, 12), -240);

        // UTC itself, and a half-hour zone with no DST at all.
        CHECK_EQ(offsetIn("UTC0", 2026, 6, 1, 12), 0);
        CHECK_EQ(offsetIn("IST-5:30", 2026, 6, 1, 12), 330);

        // Midnight local, which is where the day-boundary arithmetic bites.
        CHECK_EQ(offsetIn("CET-1CEST,M3.5.0/2,M10.5.0/3", 2026, 8, 25, 0), 120);
        CHECK_EQ(offsetIn("EST5EDT,M3.2.0,M11.1.0", 2026, 8, 25, 0), -240);

        // New Year's Eve in a zone far enough east to have already turned over.
        CHECK_EQ(offsetIn("NZST-12NZDT,M9.5.0,M4.1.0/3", 2026, 1, 1, 0), 780);

        unsetenv("TZ");
        tzset();
    }
}
