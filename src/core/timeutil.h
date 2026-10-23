// timeutil.h - civil date/time handling for HAFAS payloads.
//
// Deliberately free of <ctime>, Arduino, and platform headers so it compiles
// and unit-tests on the host exactly as it will on the ESP32.
//
// All timestamps are LOCAL Aachen time. We never do timezone maths here:
// HAFAS returns local times, the event start is local, and the ESP32's clock
// is set to local time via configTzTime(). Introducing UTC conversion would
// add a class of bug for no benefit.
#pragma once
#include <cstdint>

namespace core {

// Minutes since 1970-01-01 00:00 local. int32 covers ~4000 years.
struct Timestamp {
    int32_t min = kInvalid;
    static constexpr int32_t kInvalid = INT32_MIN;

    bool valid() const { return min != kInvalid; }
    bool operator==(const Timestamp& o) const { return min == o.min; }
    bool operator!=(const Timestamp& o) const { return min != o.min; }
    bool operator<(const Timestamp& o)  const { return min <  o.min; }
    bool operator<=(const Timestamp& o) const { return min <= o.min; }
    bool operator>(const Timestamp& o)  const { return min >  o.min; }
    bool operator>=(const Timestamp& o) const { return min >= o.min; }
};

inline Timestamp makeTs(int32_t minutes) { Timestamp t; t.min = minutes; return t; }
inline Timestamp invalidTs() { return Timestamp{}; }

// Difference in minutes (a - b). Caller must check both are valid.
inline int32_t diffMinutes(Timestamp a, Timestamp b) { return a.min - b.min; }
inline Timestamp addMinutes(Timestamp t, int32_t m) {
    return t.valid() ? makeTs(t.min + m) : t;
}

// Howard Hinnant's days_from_civil. Exact for any proleptic Gregorian date.
int32_t daysFromCivil(int32_t y, uint32_t m, uint32_t d);
void    civilFromDays(int32_t z, int32_t& y, uint32_t& m, uint32_t& d);

// Parse "YYYY-MM-DD". Returns days since epoch, or INT32_MIN on failure.
int32_t parseDate(const char* s);

// Parse "HH:MM" or "HH:MM:SS". Returns minutes past midnight, or -1.
// HAFAS legitimately emits hours >= 24 for journeys crossing midnight
// (e.g. "25:10:00" meaning 01:10 the next day), so hours up to 47 are valid.
int32_t parseTimeOfDay(const char* s);

// Combine a HAFAS date+time pair into a Timestamp.
// Hours >= 24 roll the date forward, which is the whole point of allowing them.
Timestamp parseDateTime(const char* date, const char* time);

// Parse an ISO-8601 local timestamp as HAFAS emits it in planRtTs:
// "2026-08-22T18:46:45.000+02:00". The zone offset is read past and ignored --
// see the header note above. HAFAS reports local time alongside its own local
// offset, every other time in this system is local, and converting would only
// create a way to be an hour wrong twice a year.
Timestamp parseIso8601Local(const char* s);

// Parse an ISO-8601 duration as HAFAS emits it: "PT01H30M00S", "PT45M",
// "P1DT02H". Returns minutes, or -1 on failure.
int32_t parseIsoDuration(const char* s);

// Format helpers for rendering. buf must hold at least 6 bytes.
void formatHHMM(Timestamp t, char* buf);

// Format as RFC 3339 with an explicit offset: "2026-08-24T13:00:00+02:00".
// buf must hold at least 26 bytes.
//
// The offset is passed in rather than derived, because this file has no
// timezone database and should not grow one. On the ESP32 it comes from
// localtime_r()'s tm_gmtoff, which configTzTime() has already made correct
// including the summer-time switch.
//
// Needed because Google's Calendar API requires timeMin/timeMax as RFC 3339
// "with mandatory time zone offset". Our own reasoning stays in local time; the
// offset is a wire-format detail at the boundary.
void formatRfc3339(Timestamp t, int16_t utcOffsetMin, char* buf);

} // namespace core
