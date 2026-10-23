#include "ratelimit.h"

#include <cstdint>

namespace core {

const char* rateDecisionName(RateDecision d) {
    switch (d) {
        case RateDecision::Allow:      return "allow";
        case RateDecision::TooSoon:    return "too soon";
        case RateDecision::HourlyCap:  return "hourly cap";
        case RateDecision::DailyCap:   return "daily cap";
        case RateDecision::BackingOff: return "backing off";
    }
    return "?";
}

RateLimitConfig RequestBudget::clamped(const RateLimitConfig& in) {
    RateLimitConfig c = in;
    // The hourly ring is exact only while the limit fits in it, so the limit is
    // clamped to the storage rather than the storage silently under-counting.
    if (c.maxPerHour > kRing - 1) c.maxPerHour = kRing - 1;
    if (c.maxPerHour == 0) c.maxPerHour = 1;
    if (c.maxPerDay == 0) c.maxPerDay = 1;
    if (c.maxPerDay < c.maxPerHour) c.maxPerDay = c.maxPerHour;
    if (c.backoffBaseSec > kMaxBackoffSec) c.backoffBaseSec = kMaxBackoffSec;
    if (c.backoffMaxSec > kMaxBackoffSec) c.backoffMaxSec = kMaxBackoffSec;
    if (c.backoffMaxSec < c.backoffBaseSec) c.backoffMaxSec = c.backoffBaseSec;
    return c;
}

void RequestBudget::reset() {
    for (int i = 0; i < kRing; ++i) { ring_[i] = 0; ringUsed_[i] = false; }
    ringNext_ = 0;
    for (int i = 0; i < kDayBuckets; ++i) {
        bucketHour_[i] = 0;
        bucketCount_[i] = 0;
        bucketUsed_[i] = false;
    }
    lastRequestSec_ = 0;
    everRequested_ = false;
    backoffUntilSec_ = 0;
    failures_ = 0;
    // taken_/refused_ are lifetime counters for the health panel and survive.
}

// A monotonic clock that goes backwards has either wrapped or been replaced.
// Either way the recorded history describes a different timeline, so it is
// discarded: counting stale entries against a fresh clock could either block
// requests for an hour or let a burst through, and neither is acceptable.
void RequestBudget::handleClockJump(uint32_t nowSec) {
    if (everRequested_ && nowSec < lastSeenSec_) reset();
    lastSeenSec_ = nowSec;
}

uint16_t RequestBudget::usedLastHour(uint32_t nowSec) const {
    uint16_t n = 0;
    for (int i = 0; i < kRing; ++i)
        if (ringUsed_[i] && nowSec >= ring_[i] && nowSec - ring_[i] < 3600u) ++n;
    return n;
}

uint16_t RequestBudget::usedLastDay(uint32_t nowSec) const {
    const uint32_t thisHour = nowSec / 3600u;
    const uint32_t oldest = (thisHour >= 23u) ? (thisHour - 23u) : 0u;
    uint16_t n = 0;
    for (int i = 0; i < kDayBuckets; ++i)
        if (bucketUsed_[i] && bucketHour_[i] >= oldest && bucketHour_[i] <= thisHour)
            n = static_cast<uint16_t>(n + bucketCount_[i]);
    return n;
}

RateDecision RequestBudget::check(uint32_t nowSec) const {
    // check() is const and so cannot reset a stale history, but it must still
    // survive a clock that went backwards: a backoff further out than any
    // backoff we could legitimately have set belongs to a previous timeline.
    if (nowSec < backoffUntilSec_ && backoffUntilSec_ - nowSec <= kMaxBackoffSec)
        return RateDecision::BackingOff;

    if (everRequested_) {
        // Unsigned arithmetic: a backward jump is caught in take()/note(), and
        // until then a request that looks like it came from the future is
        // treated as too soon, which is the safe direction.
        if (nowSec < lastRequestSec_) return RateDecision::TooSoon;
        if (nowSec - lastRequestSec_ < cfg_.minIntervalSec) return RateDecision::TooSoon;
    }

    if (usedLastHour(nowSec) >= cfg_.maxPerHour) return RateDecision::HourlyCap;
    if (usedLastDay(nowSec) >= cfg_.maxPerDay) return RateDecision::DailyCap;
    return RateDecision::Allow;
}

void RequestBudget::note(uint32_t nowSec) {
    ring_[ringNext_] = nowSec;
    ringUsed_[ringNext_] = true;
    ringNext_ = (ringNext_ + 1) % kRing;

    const uint32_t hour = nowSec / 3600u;
    const int slot = static_cast<int>(hour % kDayBuckets);
    if (!bucketUsed_[slot] || bucketHour_[slot] != hour) {
        bucketHour_[slot] = hour;
        bucketCount_[slot] = 0;
        bucketUsed_[slot] = true;
    }
    if (bucketCount_[slot] < 0xFFFFu) ++bucketCount_[slot];

    lastRequestSec_ = nowSec;
    everRequested_ = true;
    ++taken_;
}

bool RequestBudget::take(uint32_t nowSec) {
    handleClockJump(nowSec);
    if (check(nowSec) != RateDecision::Allow) {
        ++refused_;
        return false;
    }
    note(nowSec);
    return true;
}

void RequestBudget::recordFailure(uint32_t nowSec, uint32_t retryAfterSec) {
    handleClockJump(nowSec);
    if (failures_ < 32) ++failures_;

    // base * 2^(failures-1), capped, computed without overflowing on a long
    // outage.
    uint32_t wait = cfg_.backoffBaseSec;
    for (uint16_t i = 1; i < failures_ && wait < cfg_.backoffMaxSec; ++i) wait *= 2;
    if (wait > cfg_.backoffMaxSec) wait = cfg_.backoffMaxSec;

    // A server that told us how long to wait is more authoritative than our
    // guess -- but only ever to lengthen the wait, never to shorten it, and
    // never past the hard ceiling. An unclamped Retry-After is how this ends up
    // either waiting for decades or, once the sum wraps, not waiting at all.
    if (retryAfterSec > kMaxBackoffSec) retryAfterSec = kMaxBackoffSec;
    if (retryAfterSec > wait) wait = retryAfterSec;
    if (wait > kMaxBackoffSec) wait = kMaxBackoffSec;

    // Saturating add: nowSec is near 2^32 late in a long uptime.
    const uint32_t until = (nowSec > UINT32_MAX - wait) ? UINT32_MAX : (nowSec + wait);
    if (until > backoffUntilSec_) backoffUntilSec_ = until;
}

void RequestBudget::recordSuccess(uint32_t nowSec) {
    handleClockJump(nowSec);
    failures_ = 0;
    backoffUntilSec_ = 0;
}

uint32_t RequestBudget::secondsUntilAllowed(uint32_t nowSec) const {
    switch (check(nowSec)) {
        case RateDecision::Allow:
            return 0;

        case RateDecision::BackingOff: {
            const uint32_t d = backoffUntilSec_ - nowSec;
            return d > kMaxBackoffSec ? kMaxBackoffSec : d;
        }

        case RateDecision::TooSoon:
            if (nowSec < lastRequestSec_) return cfg_.minIntervalSec;
            return cfg_.minIntervalSec - (nowSec - lastRequestSec_);

        case RateDecision::HourlyCap: {
            // Room appears when the oldest request inside the window ages out.
            uint32_t oldest = 0;
            bool found = false;
            for (int i = 0; i < kRing; ++i)
                if (ringUsed_[i] && nowSec >= ring_[i] && nowSec - ring_[i] < 3600u)
                    if (!found || ring_[i] < oldest) { oldest = ring_[i]; found = true; }
            if (!found) return 0;
            const uint32_t freeAt = oldest + 3600u;
            return (freeAt > nowSec) ? (freeAt - nowSec) : 0u;
        }

        case RateDecision::DailyCap: {
            // Likewise, when the oldest hour bucket falls out of the window.
            const uint32_t thisHour = nowSec / 3600u;
            const uint32_t oldestAllowed = (thisHour >= 23u) ? (thisHour - 23u) : 0u;
            uint32_t oldest = 0;
            bool found = false;
            for (int i = 0; i < kDayBuckets; ++i)
                if (bucketUsed_[i] && bucketCount_[i] > 0 &&
                    bucketHour_[i] >= oldestAllowed && bucketHour_[i] <= thisHour)
                    if (!found || bucketHour_[i] < oldest) { oldest = bucketHour_[i]; found = true; }
            if (!found) return 0;
            const uint32_t freeAt = (oldest + 24u) * 3600u;
            return (freeAt > nowSec) ? (freeAt - nowSec) : 0u;
        }
    }
    return 0;
}

} // namespace core
