// local_clock.h - the one place that talks to the platform about time.
//
// The whole firmware works in LOCAL time on purpose (see core/timeutil.h), so
// this configures the ESP32's clock for Europe/Berlin and hands back
// core::Timestamp values. Nothing above it converts a timezone.
//
// The POSIX TZ string carries the summer-time rule, so the offset is right
// across the March and October switches without anyone doing arithmetic. That
// offset is also what the Google Calendar request needs -- see tz_offset.h for
// why getting it is not the one-liner it looks like.
#pragma once
// Arduino.h for configTzTime(); <ctime> for localtime_r and tm_gmtoff, which
// ESP32's newlib does provide.
#include <Arduino.h>
#include <ctime>

#include "core/timeutil.h"
#include "tz_offset.h"

namespace platform {

// CET/CEST: +1, +2 in summer, last Sunday of March to last Sunday of October.
constexpr const char* kBerlinTz = "CET-1CEST,M3.5.0/2,M10.5.0/3";

class LocalClock {
    public:
    void begin(const char* tz = kBerlinTz) {
        // Three servers because one of them will be unreachable eventually.
        configTzTime(tz, "pool.ntp.org", "time.nist.gov", "de.pool.ntp.org");
    }

    // True once NTP has answered at least once. Before that the ESP32's clock
    // reads 1970, and a countdown computed from it would be wildly wrong --
    // which is why the health panel prints "no clock" rather than a time.
    bool synced() const {
        return std::time(nullptr) > 1700000000;   // any plausible recent date
    }

    core::Timestamp now() const {
        if (!synced()) return core::invalidTs();
        std::time_t t = std::time(nullptr);
        std::tm lt{};
        localtime_r(&t, &lt);
        const int32_t days = core::daysFromCivil(lt.tm_year + 1900,
                                                 static_cast<uint32_t>(lt.tm_mon + 1),
                                                 static_cast<uint32_t>(lt.tm_mday));
        return core::makeTs(days * 1440 + lt.tm_hour * 60 + lt.tm_min);
    }

    // Minutes east of UTC, right now. Google's Calendar API wants this in
    // timeMin/timeMax, and it is the only place the offset is needed.
    int16_t utcOffsetMin() const {
        return utcOffsetMinutesAt(std::time(nullptr));
    }

    // Real UTC epoch seconds, for the Google JWT's iat/exp -- the only UTC in
    // the whole firmware, because Google checks them against Google's clock.
    int64_t utcSeconds() const { return static_cast<int64_t>(std::time(nullptr)); }

    void formatHHMM(char* buf) const {
        core::formatHHMM(now(), buf);
    }
};

} // namespace platform
