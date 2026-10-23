#include "test.h"
#include "../src/core/timeutil.h"
using namespace core;

void test_timeutil() {
    t::suite("timeutil");

    // Civil date round-trip across a leap day and a century boundary.
    CHECK_EQ(daysFromCivil(1970, 1, 1), 0);
    CHECK_EQ(daysFromCivil(2026, 8, 13), 20678);
    {
        int32_t y; uint32_t m, d;
        civilFromDays(daysFromCivil(2024, 2, 29), y, m, d);
        CHECK_EQ(y, 2024); CHECK_EQ(m, 2); CHECK_EQ(d, 29);
        civilFromDays(daysFromCivil(2000, 1, 1), y, m, d);
        CHECK_EQ(y, 2000); CHECK_EQ(m, 1); CHECK_EQ(d, 1);
    }

    CHECK_EQ(parseDate("2026-08-13"), 20678);
    CHECK_EQ(parseDate("bad"), INT32_MIN);
    CHECK_EQ(parseDate("2026-13-01"), INT32_MIN);   // month out of range
    CHECK_EQ(parseDate(nullptr), INT32_MIN);

    CHECK_EQ(parseTimeOfDay("14:50"), 890);
    CHECK_EQ(parseTimeOfDay("14:50:00"), 890);      // seconds accepted, ignored
    CHECK_EQ(parseTimeOfDay("00:00"), 0);
    // HAFAS emits hours >= 24 for journeys crossing midnight.
    CHECK_EQ(parseTimeOfDay("25:10:00"), 1510);
    CHECK_EQ(parseTimeOfDay("48:00"), -1);
    CHECK_EQ(parseTimeOfDay("14:99"), -1);
    CHECK_EQ(parseTimeOfDay("nope"), -1);

    // 25:10 must roll into the following day, not stay on the same one.
    Timestamp a = parseDateTime("2026-08-13", "23:50");
    Timestamp b = parseDateTime("2026-08-13", "25:10");
    CHECK(a.valid() && b.valid());
    CHECK_EQ(diffMinutes(b, a), 80);

    CHECK(!parseDateTime("2026-08-13", "bad").valid());
    CHECK(!parseDateTime(nullptr, "10:00").valid());

    CHECK_EQ(parseIsoDuration("PT01H30M00S"), 90);
    CHECK_EQ(parseIsoDuration("PT45M"), 45);
    CHECK_EQ(parseIsoDuration("PT2H"), 120);
    CHECK_EQ(parseIsoDuration("P1DT02H30M"), 1590);
    CHECK_EQ(parseIsoDuration("garbage"), -1);
    CHECK_EQ(parseIsoDuration("PT"), -1);

    char buf[8];
    formatHHMM(parseDateTime("2026-08-13", "09:05"), buf);
    CHECK_STR(buf, "09:05");
    formatHHMM(parseDateTime("2026-08-13", "25:10"), buf);
    CHECK_STR(buf, "01:10");    // displayed as wall-clock on the next day
    formatHHMM(invalidTs(), buf);
    CHECK_STR(buf, "--:--");

    CHECK_EQ(addMinutes(makeTs(100), -30).min, 70);
    CHECK(!addMinutes(invalidTs(), 10).valid());

    t::suite("timeutil / dates that would overflow the minute count");
    // Timestamps are minutes in an int32, so days * 1440 must stay in range.
    // A calendar can hold an event dated 9999-12-31 -- somebody can type one --
    // and that multiplication is undefined behaviour, not a large number.
    CHECK_EQ(core::parseDate("9999-12-31"), INT32_MIN);
    CHECK_EQ(core::parseDate("1969-12-31"), INT32_MIN);
    CHECK_EQ(core::parseDate("3000-01-01"), INT32_MIN);
    CHECK(core::parseDate("1970-01-01") != INT32_MIN);
    CHECK(core::parseDate("2999-12-31") != INT32_MIN);
    CHECK(!core::parseDateTime("9999-12-31", "12:00").valid());

    t::suite("timeutil / RFC 3339 with an explicit offset");
    {
        char iso[32];
        core::formatRfc3339(core::parseDateTime("2026-08-24", "13:00"), 120, iso);
        CHECK_STR(iso, "2026-08-24T13:00:00+02:00");
        core::formatRfc3339(core::parseDateTime("2026-01-05", "07:05"), 60, iso);
        CHECK_STR(iso, "2026-01-05T07:05:00+01:00");
        core::formatRfc3339(core::parseDateTime("2026-12-31", "23:59"), -330, iso);
        CHECK_STR(iso, "2026-12-31T23:59:00-05:30");
        core::formatRfc3339(core::parseDateTime("2026-03-01", "00:00"), 0, iso);
        CHECK_STR(iso, "2026-03-01T00:00:00+00:00");
        // An hour past midnight rolls the date, the same as everywhere else.
        core::formatRfc3339(core::parseDateTime("2026-08-24", "25:10"), 120, iso);
        CHECK_STR(iso, "2026-08-25T01:10:00+02:00");
        core::formatRfc3339(core::invalidTs(), 120, iso);
        CHECK_STR(iso, "");
    }

    t::suite("timeutil / ISO 8601 timestamps as HAFAS sends them");
    CHECK_EQ(core::parseIso8601Local("2026-08-22T18:46:45.000+02:00").min,
             core::parseDateTime("2026-08-22", "18:46").min);
    CHECK_EQ(core::parseIso8601Local("2026-08-22T18:46:45Z").min,
             core::parseDateTime("2026-08-22", "18:46").min);
    CHECK(!core::parseIso8601Local("2026-08-22 broken").valid());
    CHECK(!core::parseIso8601Local(nullptr).valid());
}
