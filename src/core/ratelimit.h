// ratelimit.h - the fuse on outbound API requests.
//
// WHY THIS EXISTS
// ---------------
// The AVV usage agreement is not a rate card -- it names no number -- but it
// does impose two obligations that have to live somewhere in the code:
//
//   3.a  "Um die Serverlast der AVV GmbH moeglichst gering zu halten, duerfen
//        dynamische Fahrplandaten ausschliesslich als sogenannte Live-Abfragen
//        abgerufen werden."
//        Dynamic timetable data may be fetched only as live queries -- a query
//        answering an actual need, not a background harvest.
//
//   6.a  "Lizenznehmer/Firma ist bemueht, durch technische Sicherungsmassnahmen
//        einen massenhaften Abruf von Daten zu verhindern."
//        Take technical measures against mass retrieval.
//
// 3.a is a design rule, enforced by the orchestrator: a trip query happens only
// inside an open alert window, which exists only because a calendar appointment
// exists. This file is 6.a -- the technical measure, and the backstop for when
// the design rule is defeated by a bug.
//
// The realistic load is small: an alert window is 35 minutes at one query a
// minute, so a few hundred requests a day at most. This class is therefore not
// there to shave requests off normal operation. It is there so that a spinning
// loop, a retry storm or a stuck alert window stops before AVV notices, and it
// is sized accordingly: generous in normal use, hard-stopping when something
// has plainly gone wrong.
//
// Clause 6.b also allows AVV to restrict access if they fear overload. The
// backoff below is what makes that survivable rather than a hot loop against a
// server that is refusing us.
//
// CLOCK
// -----
// Everything here takes a MONOTONIC second counter, not wall-clock time. A rate
// limit driven by the wall clock can be defeated by an NTP step, and NTP steps
// are routine on a device that boots without a battery-backed clock.
//
// On the ESP32 use esp_timer_get_time() / 1000000 -- a 64-bit microsecond
// counter. Do NOT feed it millis() / 1000: that wraps every 49.7 days, and a
// wrap would silently reset the day's budget. A backward jump is treated as a
// restart rather than trusted, so a wrap is survivable, but it is not something
// to design in on purpose.
#pragma once
#include <cstdint>

namespace core {

struct RateLimitConfig {
    // No two requests closer together than this. The single most useful limit:
    // it is what a runaway loop hits first.
    uint32_t minIntervalSec = 20;

    // Rolling windows, not calendar periods -- a rolling window needs no wall
    // clock and cannot be reset by waiting for midnight.
    uint16_t maxPerHour = 60;
    uint16_t maxPerDay  = 500;

    // Exponential backoff after a failed request: base, doubling per
    // consecutive failure, capped.
    uint32_t backoffBaseSec = 30;
    uint32_t backoffMaxSec  = 1800;
};

// No backoff may ever exceed this, from any source. A day is far longer than
// any real outage worth waiting through, and short enough that a device stuck
// behind a bad Retry-After recovers by itself.
constexpr uint32_t kMaxBackoffSec = 86400;

enum class RateDecision : uint8_t {
    Allow,
    TooSoon,        // inside minIntervalSec of the last request
    HourlyCap,      // rolling-hour budget spent
    DailyCap,       // rolling-day budget spent
    BackingOff,     // a recent failure, or the server told us to wait
};

const char* rateDecisionName(RateDecision d);

class RequestBudget {
public:
    RequestBudget() = default;
    explicit RequestBudget(const RateLimitConfig& cfg) : cfg_(clamped(cfg)) {}

    void configure(const RateLimitConfig& cfg) { cfg_ = clamped(cfg); }
    const RateLimitConfig& config() const { return cfg_; }

    // Would a request be permitted right now? No side effects, so the caller
    // can ask before doing the work of building one.
    RateDecision check(uint32_t nowSec) const;

    // Ask and, if permitted, record it. This is the call sites' entry point --
    // check() alone records nothing, so a caller that forgets take() would send
    // an unbudgeted request.
    bool take(uint32_t nowSec);

    // A request that failed for a reason the server owns: a 5xx, a 429, a
    // timeout. `retryAfterSec` carries an HTTP Retry-After when the server sent
    // one; it wins over our own backoff whenever it is longer, because a server
    // asking for room is more authoritative than our guess.
    //
    // It is clamped to kMaxBackoffSec first. Servers do send absolute epoch
    // seconds in that header by mistake, and an unclamped value would either
    // park the device for decades or -- once nowSec + wait wraps past 2^32 --
    // silently switch the backoff OFF, producing exactly the retry storm this
    // class exists to prevent.
    void recordFailure(uint32_t nowSec, uint32_t retryAfterSec = 0);

    // A request that came back. Clears the backoff.
    void recordSuccess(uint32_t nowSec);

    // How long until check() would return Allow. Zero when it already does.
    // The health panel shows this, and the orchestrator sleeps on it rather
    // than spinning.
    uint32_t secondsUntilAllowed(uint32_t nowSec) const;

    uint16_t usedLastHour(uint32_t nowSec) const;
    uint16_t usedLastDay(uint32_t nowSec) const;
    uint16_t consecutiveFailures() const { return failures_; }

    // Total requests taken since boot, and total refusals. Both are health-panel
    // material: a refusal count that climbs means something upstream is looping.
    uint32_t taken() const { return taken_; }
    uint32_t refused() const { return refused_; }

    void reset();

private:
    // Exact rolling hour: one timestamp per request. 64 slots covers the
    // clamped hourly maximum with room to spare, for 256 bytes.
    static constexpr int kRing = 64;
    // Rolling day by hour buckets -- 400 timestamps would be wasteful for a
    // limit that only has to be right to the hour.
    static constexpr int kDayBuckets = 24;

    static RateLimitConfig clamped(const RateLimitConfig& cfg);
    void note(uint32_t nowSec);
    void handleClockJump(uint32_t nowSec);

    RateLimitConfig cfg_{};

    // Slot occupancy is a separate flag rather than "timestamp != 0": zero is a
    // perfectly legal monotonic timestamp, and on the ESP32 it is the value for
    // the entire first second after boot. Using it as a sentinel made the first
    // request of every boot invisible to the hourly window.
    uint32_t ring_[kRing] = {};
    bool     ringUsed_[kRing] = {};
    int      ringNext_ = 0;

    uint32_t bucketHour_[kDayBuckets] = {};   // which hour each bucket holds
    uint16_t bucketCount_[kDayBuckets] = {};
    bool     bucketUsed_[kDayBuckets] = {};

    uint32_t lastSeenSec_ = 0;       // for detecting a backward clock jump
    uint32_t lastRequestSec_ = 0;
    bool     everRequested_ = false;

    uint32_t backoffUntilSec_ = 0;
    uint16_t failures_ = 0;

    uint32_t taken_ = 0;
    uint32_t refused_ = 0;
};

} // namespace core
