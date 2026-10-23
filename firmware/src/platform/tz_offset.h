// tz_offset.h - how many minutes east of UTC are we, right now.
//
// WHY THIS IS NOT ONE LINE
// ------------------------
// The obvious answer is `localtime_r(...).tm_gmtoff`. That field is a glibc/BSD
// extension, and the ESP32's newlib does NOT have it:
//
//     error: 'struct tm' has no member named 'tm_gmtoff'
//                                            (xtensa-esp32 gcc 8.4, Arduino 2.0.17)
//
// So the offset is derived instead, by asking for the same instant twice --
// once local, once UTC -- and subtracting. That works on every libc, and it
// keeps the DST rule where it belongs: in the TZ string, not in our arithmetic.
//
// This header deliberately pulls in nothing but <ctime>, so the one piece of
// real reasoning in the platform layer can be tested on the laptop against a
// genuine timezone database. test/test_tz_offset.cpp does exactly that,
// including both days of the year when Germany changes clocks.
#pragma once
#include <cstdint>
#include <ctime>

namespace platform {

// Minutes east of UTC, given the same instant broken down both ways.
//
// The subtlety is that the two can fall on different DAYS -- and, on New Year's
// Eve, in different years, where the day-of-year numbers are 364 apart rather
// than 1. Both are handled by treating any large difference as a single day in
// the obvious direction.
inline int16_t utcOffsetMinutes(const std::tm& local, const std::tm& utc) {
    const int localMin = local.tm_hour * 60 + local.tm_min;
    const int utcMin = utc.tm_hour * 60 + utc.tm_min;

    int dayDelta = local.tm_yday - utc.tm_yday;
    if (dayDelta > 1) dayDelta = -1;         // local is 31 Dec, UTC is already 1 Jan
    else if (dayDelta < -1) dayDelta = 1;    // local is 1 Jan, UTC is still 31 Dec

    return static_cast<int16_t>(localMin - utcMin + dayDelta * 1440);
}

// The same, for a given instant, using the process's configured zone.
inline int16_t utcOffsetMinutesAt(std::time_t t) {
    std::tm lt{};
    std::tm gt{};
    localtime_r(&t, &lt);
    gmtime_r(&t, &gt);
    return utcOffsetMinutes(lt, gt);
}

} // namespace platform
