#include "test.h"
#include "../src/core/ratelimit.h"

using namespace core;

namespace {
RateLimitConfig testConfig() {
    RateLimitConfig c;
    c.minIntervalSec = 20;
    c.maxPerHour = 10;
    c.maxPerDay = 40;
    c.backoffBaseSec = 30;
    c.backoffMaxSec = 480;
    return c;
}
} // namespace

void test_ratelimit() {
    t::suite("ratelimit / minimum interval");
    {
        RequestBudget b(testConfig());
        CHECK(b.take(1000));                       // first request always allowed
        CHECK(!b.take(1001));                      // a tight loop hits this
        CHECK(!b.take(1019));
        CHECK_EQ(b.secondsUntilAllowed(1001), 19);
        CHECK(b.take(1020));                       // exactly at the boundary
        CHECK_EQ(b.taken(), 2);
        CHECK_EQ(b.refused(), 2);
        // check() must not consume budget -- asking is free, taking is not.
        const uint32_t before = b.taken();
        for (int i = 0; i < 5; ++i)
            CHECK_EQ(static_cast<int>(b.check(2000)), static_cast<int>(RateDecision::Allow));
        CHECK_EQ(b.taken(), before);
    }

    t::suite("ratelimit / rolling hour");
    {
        RequestBudget b(testConfig());
        // Ten requests, one every 30 s, fills the hourly budget.
        for (int i = 0; i < 10; ++i) CHECK(b.take(static_cast<uint32_t>(10000 + i * 30)));
        CHECK_EQ(b.usedLastHour(10300), 10);
        CHECK_EQ(static_cast<int>(b.check(10300)), static_cast<int>(RateDecision::HourlyCap));

        // The window rolls: room appears an hour after the FIRST one, not an
        // hour after the last. A fixed-window limiter would get this wrong.
        CHECK_EQ(b.secondsUntilAllowed(10300), 10000u + 3600u - 10300u);
        CHECK_EQ(static_cast<int>(b.check(13599)), static_cast<int>(RateDecision::HourlyCap));
        CHECK_EQ(static_cast<int>(b.check(13601)), static_cast<int>(RateDecision::Allow));
        CHECK(b.take(13601));
        CHECK_EQ(b.usedLastHour(13601), 10);       // the first has aged out, this one is new
    }

    t::suite("ratelimit / rolling day");
    {
        RateLimitConfig c = testConfig();
        c.minIntervalSec = 1;
        c.maxPerHour = 5;
        c.maxPerDay = 12;
        RequestBudget b(c);

        // Spread across hours so the hourly cap never fires first.
        uint32_t t0 = 0;
        int sent = 0;
        for (int h = 0; h < 4 && sent < 12; ++h)
            for (int i = 0; i < 3 && sent < 12; ++i) {
                t0 = static_cast<uint32_t>(h * 3600 + i * 60);
                CHECK(b.take(t0));
                ++sent;
            }
        CHECK_EQ(sent, 12);
        CHECK_EQ(b.usedLastDay(t0), 12);
        CHECK_EQ(static_cast<int>(b.check(t0 + 600)), static_cast<int>(RateDecision::DailyCap));

        // Room returns 24 hours after the oldest hour bucket.
        CHECK_EQ(static_cast<int>(b.check(24u * 3600u - 1u)), static_cast<int>(RateDecision::DailyCap));
        CHECK_EQ(static_cast<int>(b.check(24u * 3600u + 10u)), static_cast<int>(RateDecision::Allow));
    }

    t::suite("ratelimit / backoff after failure");
    {
        RequestBudget b(testConfig());
        CHECK(b.take(1000));
        b.recordFailure(1000);
        CHECK_EQ(static_cast<int>(b.check(1029)), static_cast<int>(RateDecision::BackingOff));
        CHECK_EQ(b.secondsUntilAllowed(1000), 30);
        CHECK_EQ(static_cast<int>(b.check(1030)), static_cast<int>(RateDecision::Allow));

        // Consecutive failures double the wait, up to the cap.
        RequestBudget c2(testConfig());
        c2.recordFailure(0);                       CHECK_EQ(c2.secondsUntilAllowed(0), 30);
        c2.recordFailure(0);                       CHECK_EQ(c2.secondsUntilAllowed(0), 60);
        c2.recordFailure(0);                       CHECK_EQ(c2.secondsUntilAllowed(0), 120);
        c2.recordFailure(0);                       CHECK_EQ(c2.secondsUntilAllowed(0), 240);
        c2.recordFailure(0);                       CHECK_EQ(c2.secondsUntilAllowed(0), 480);
        c2.recordFailure(0);                       CHECK_EQ(c2.secondsUntilAllowed(0), 480);  // capped
        for (int i = 0; i < 40; ++i) c2.recordFailure(0);
        CHECK_EQ(c2.secondsUntilAllowed(0), 480);  // and stays capped, no overflow
        CHECK(c2.consecutiveFailures() > 0);

        // One success clears it.
        c2.recordSuccess(0);
        CHECK_EQ(c2.consecutiveFailures(), 0);
        CHECK_EQ(static_cast<int>(c2.check(0)), static_cast<int>(RateDecision::Allow));
    }
    {
        // Clause 6.b lets AVV restrict access. When a server says how long to
        // wait, that wins -- but only ever to wait longer, never shorter.
        RequestBudget b(testConfig());
        b.recordFailure(1000, 600);
        CHECK_EQ(b.secondsUntilAllowed(1000), 600);
        RequestBudget b2(testConfig());
        b2.recordFailure(1000, 5);                 // shorter than our own backoff
        CHECK_EQ(b2.secondsUntilAllowed(1000), 30);
    }

    t::suite("ratelimit / the runaway loop it exists to stop");
    {
        // The failure this is really for: something calls in a tight loop for
        // an hour. Simulate one call per second for two hours and check the
        // budget holds.
        RateLimitConfig c = testConfig();
        c.maxPerHour = 60;
        c.maxPerDay = 500;
        RequestBudget b(c);
        int allowed = 0;
        for (uint32_t s = 0; s < 7200; ++s)
            if (b.take(s)) ++allowed;

        // minIntervalSec = 20 alone would permit 360 in two hours; the hourly
        // cap is what actually binds.
        CHECK(allowed <= 121);
        CHECK(allowed >= 100);
        CHECK(b.refused() > 7000);
    }
    {
        // And the same loop against a server that is down: backoff must stop it
        // from becoming a retry storm.
        RequestBudget b(testConfig());
        int attempts = 0;
        for (uint32_t s = 0; s < 3600; ++s) {
            if (b.take(s)) { ++attempts; b.recordFailure(s); }
        }
        CHECK(attempts <= 12);                     // 30,60,120,240,480,480,... in an hour
    }

    t::suite("ratelimit / a hostile or broken Retry-After");
    {
        // Servers do send an absolute epoch in Retry-After by mistake. Left
        // unclamped it parks the device for decades.
        RequestBudget b(testConfig());
        b.recordFailure(1000, 1756051200u);
        CHECK(b.secondsUntilAllowed(1000) <= core::kMaxBackoffSec);
        CHECK_EQ(b.secondsUntilAllowed(1000), core::kMaxBackoffSec);
    }
    {
        // And the far worse one: a value large enough that nowSec + wait wraps
        // past 2^32 used to store a backoff in the PAST, switching the backoff
        // off entirely at the exact moment it was needed.
        RequestBudget b(testConfig());
        b.recordFailure(1000, 4294967295u);
        CHECK_EQ(static_cast<int>(b.check(1000)), static_cast<int>(RateDecision::BackingOff));
        CHECK(b.secondsUntilAllowed(1000) > 0);
        CHECK(b.secondsUntilAllowed(1000) <= core::kMaxBackoffSec);
    }
    {
        // Near the end of a long uptime the addition itself can wrap.
        RequestBudget b(testConfig());
        const uint32_t late = 4294967000u;
        b.recordFailure(late, 3600);
        CHECK_EQ(static_cast<int>(b.check(late)), static_cast<int>(RateDecision::BackingOff));
    }

    t::suite("ratelimit / a timestamp of zero is a real timestamp");
    {
        // esp_timer_get_time()/1000000 is 0 for the whole first second after
        // boot. A ring that used 0 as its empty-slot marker made that request
        // invisible, so the first hour of every boot allowed one request too
        // many and the health panel under-reported.
        RequestBudget b(testConfig());
        CHECK(b.take(0));
        CHECK_EQ(b.usedLastHour(0), 1);
        CHECK_EQ(b.usedLastHour(10), 1);

        RateLimitConfig c = testConfig();
        c.minIntervalSec = 1;
        c.maxPerHour = 10;
        RequestBudget fromZero(c);
        int allowed = 0;
        for (uint32_t t0 = 0; t0 < 3600; ++t0) if (fromZero.take(t0)) ++allowed;
        CHECK_EQ(allowed, 10);

        RequestBudget fromLater(c);
        allowed = 0;
        for (uint32_t t0 = 100000; t0 < 103600; ++t0) if (fromLater.take(t0)) ++allowed;
        CHECK_EQ(allowed, 10);
    }

    t::suite("ratelimit / clock jumps");
    {
        // A monotonic counter that goes backwards has wrapped or been replaced.
        // Trusting the old history could block for an hour or let a burst
        // through; both are wrong, so the history is dropped.
        RequestBudget b(testConfig());
        for (int i = 0; i < 10; ++i) CHECK(b.take(static_cast<uint32_t>(100000 + i * 30)));
        CHECK_EQ(static_cast<int>(b.check(100300)), static_cast<int>(RateDecision::HourlyCap));
        CHECK(b.take(50));                         // clock went backwards
        CHECK_EQ(b.usedLastHour(50), 1);
        // Lifetime counters survive a reset -- they are diagnostics, not budget.
        CHECK(b.taken() >= 11);
    }
    {
        // check() and secondsUntilAllowed() are const and cannot reset the
        // history, but they must still survive a counter that restarted: a
        // backoff further out than any we could have set belongs to a previous
        // timeline and must not strand the device.
        RequestBudget b(testConfig());
        b.recordFailure(4000000000u);
        CHECK_EQ(static_cast<int>(b.check(5)), static_cast<int>(RateDecision::Allow));
        CHECK_EQ(b.secondsUntilAllowed(5), 0);
    }

    t::suite("ratelimit / configuration is clamped, not trusted");
    {
        RateLimitConfig c;
        c.maxPerHour = 5000;                       // larger than the ring can track exactly
        c.maxPerDay = 1;                           // smaller than the hourly cap
        c.backoffBaseSec = 100;
        c.backoffMaxSec = 10;                      // inverted
        RequestBudget b(c);
        CHECK(b.config().maxPerHour <= 63);
        CHECK(b.config().maxPerDay >= b.config().maxPerHour);
        CHECK(b.config().backoffMaxSec >= b.config().backoffBaseSec);

        RateLimitConfig z;
        z.maxPerHour = 0;
        z.maxPerDay = 0;
        RequestBudget b2(z);
        CHECK(b2.config().maxPerHour >= 1);        // zero would deadlock the device
        CHECK(b2.take(0));
    }

    t::suite("ratelimit / decision names");
    CHECK_STR(rateDecisionName(RateDecision::Allow), "allow");
    CHECK_STR(rateDecisionName(RateDecision::TooSoon), "too soon");
    CHECK_STR(rateDecisionName(RateDecision::HourlyCap), "hourly cap");
    CHECK_STR(rateDecisionName(RateDecision::DailyCap), "daily cap");
    CHECK_STR(rateDecisionName(RateDecision::BackingOff), "backing off");
}
