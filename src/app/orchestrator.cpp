#include "orchestrator.h"

#include <cstdio>
#include <cstring>

#include "../core/address.h"
#include "../core/textutil.h"
#include "../core/trip_select.h"

namespace app {
namespace {

// Retry pacing for a cycle route we have not managed to get. The route itself
// never changes, so this is purely about not hammering after a failure.
constexpr uint32_t kCycleRetrySec = 300;

void setStr(char* dst, size_t cap, const char* src) {
    core::copyStr(dst, cap, src ? src : "", src ? std::strlen(src) : 0);
}

void foldStr(char* dst, size_t cap, const char* src) {
    core::foldInto(dst, cap, src ? src : "", src ? std::strlen(src) : 0);
}

} // namespace

const char* phaseName(Phase p) {
    switch (p) {
        case Phase::NoClock:   return "no clock";
        case Phase::Idle:      return "idle";
        case Phase::Watching:  return "watching";
        case Phase::Alerting:  return "alerting";
        case Phase::Standdown: return "standdown";
    }
    return "?";
}

const char* faultName(Fault f) {
    switch (f) {
        case Fault::None:     return "";
        case Fault::Clock:    return "clock";
        case Fault::Calendar: return "calendar";
        case Fault::Place:    return "place";
        case Fault::Transit:  return "transit";
        case Fault::Cycle:    return "cycle";
        case Fault::Weather:  return "weather";
        case Fault::Screen:   return "screen";
    }
    return "?";
}

Outcome Outcome::good() {
    Outcome o;
    o.ok = true;
    return o;
}

Outcome Outcome::fail(const char* note, bool refused) {
    Outcome o;
    o.ok = false;
    o.refused = refused;
    setStr(o.note, sizeof o.note, note);
    return o;
}

Outcome Outcome::missing(const char* note) {
    Outcome o = Outcome::fail(note, false);
    o.notFound = true;
    return o;
}

Orchestrator::Orchestrator(const Config& cfg, Sources& src, core::PlaceBook& places)
    : cfg_(cfg), src_(&src), places_(&places),
      calendarBudget_(cfg.calendar), transitBudget_(cfg.transit),
      cycleBudget_(cfg.cycling), weatherBudget_(cfg.forecast) {}

void Orchestrator::setFault(Fault f, const char* text) {
    fault_ = f;
    setStr(faultText_, sizeof faultText_, (text && text[0]) ? text : faultName(f));
}

void Orchestrator::clearFault(Fault f) {
    if (fault_ == f) {
        fault_ = Fault::None;
        faultText_[0] = '\0';
    }
}

bool Orchestrator::due(uint32_t nowSec, uint32_t lastSec, uint32_t everySec,
                       bool ever) const {
    if (!ever) return true;
    // A backward jump -- which is exactly what happens the moment NTP lands --
    // must not park us until the clock catches up again.
    if (nowSec < lastSec) return true;
    return nowSec - lastSec >= everySec;
}

bool Orchestrator::stale() const {
    if (!transitAt_.valid() || !lastLocal_.valid()) return false;
    return core::diffMinutes(lastLocal_, transitAt_) > cfg_.staleAfterMin;
}

const core::Itinerary* Orchestrator::chosen() const {
    if (chosenIdx_ < 0 || chosenIdx_ >= candCount_) return nullptr;
    return &candidates_[chosenIdx_];
}

// ---------------------------------------------------------------------------

void Orchestrator::tick(const Clock& now) {
    lastLocal_ = now.local;
    lastSec_ = now.sec;
    clockSynced_ = now.synced;

    if (!now.synced || !now.local.valid()) {
        // Everything below this line needs to know what time it is. A countdown
        // computed from a 1970 clock is not a countdown, it is a lie with a
        // number in it -- so hand the screen back and say so.
        standDown(now);
        phase_ = mayHoldScreen_ ? Phase::Standdown : Phase::NoClock;
        setFault(Fault::Clock, "no clock");
        note_ = "waiting for the clock";
        return;
    }
    clearFault(Fault::Clock);

    refreshScreenState(now);
    refreshCalendar(now);

    if (!haveEvent_) {
        standDown(now);
        phase_ = mayHoldScreen_ ? Phase::Standdown : Phase::Idle;
        alertStatus_ = core::AlertStatus{};
        note_ = "nothing actionable on the calendar";
        return;
    }

    if (!ensurePlace(now)) {
        // We know there is an appointment and cannot work out where it is. That
        // is worth saying on the panel, and worth NOT taking the screen for.
        standDown(now);
        phase_ = mayHoldScreen_ ? Phase::Standdown : Phase::Watching;
        alertStatus_ = core::AlertStatus{};
        note_ = "cannot work out where that is";
        return;
    }

    refreshTrip(now);

    // The alert hangs off the DEPARTURE, not the appointment. That is the whole
    // point of asking AVV at all: leaving thirty minutes before a meeting is
    // useless information if the tram takes forty.
    const core::Itinerary* it = chosen();
    const core::Timestamp dep = it ? it->departure() : core::invalidTs();
    alertStatus_ = core::evaluate(now.local, dep, cfg_.alert);

    if (alertStatus_.state == core::AlertState::Passed) {
        standDown(now);
        handledStart_ = event_.start;
        setStr(handledLocation_, sizeof handledLocation_, event_.location);
        forgetEvent();
        everCalendar_ = false;          // go looking for the next one immediately
        phase_ = mayHoldScreen_ ? Phase::Standdown : Phase::Idle;
        note_ = "that departure is behind us";
        return;
    }

    refreshCycle(now);
    refreshWeather(now);
    driveScreen(now);
}

void Orchestrator::shutdown() {
    if (mayHoldScreen_) {
        const Outcome r = src_->releaseScreen();
        if (r.ok) {
            mayHoldScreen_ = false;
            screenConfirmed_ = false;
            ++screenReleases_;
        } else {
            setFault(Fault::Screen, r.note);
        }
    }
    forgetEvent();
    phase_ = mayHoldScreen_ ? Phase::Standdown : Phase::Idle;
}

// ---------------------------------------------------------------------------

// The appointment we have already seen through to the end, if any.
//
// No expiry, deliberately. handledStart_ is an ABSOLUTE timestamp, so the only
// thing that can ever match it is the same appointment -- a weekly recurrence
// lands on a different minute-since-epoch and is a different event. An expiry
// window here would be a branch that cannot change any outcome, and dead code
// carrying a plausible-sounding reason is worse than none.
bool Orchestrator::alreadyHandled(const core::CalendarEvent& e) const {
    if (!handledStart_.valid()) return false;
    return e.start == handledStart_ &&
           std::strcmp(e.location, handledLocation_) == 0;
}

void Orchestrator::adopt(const core::CalendarEvent& e) {
    event_ = e;
    haveEvent_ = true;
    note_ = "new appointment adopted";

    // A different appointment invalidates everything downstream of it. The
    // cycle route deliberately survives -- it is keyed on the DESTINATION, and
    // the road to the Hauptgebaeude is the same road it was this morning.
    destResolved_ = false;
    placeSpelling_ = 0;
    destKey_[0] = '\0';
    destLabel_[0] = '\0';
    candCount_ = 0;
    chosenIdx_ = -1;
    anyCandidates_ = false;
    willBeLate_ = false;
    lateByMin_ = 0;
    transfers_ = 0;
    noTransitReason_[0] = '\0';
    transitAt_ = core::invalidTs();
    everTrip_ = false;
    everWeather_ = false;
    alertStatus_ = core::AlertStatus{};
}

void Orchestrator::forgetEvent() {
    haveEvent_ = false;
    event_ = core::CalendarEvent{};
    destResolved_ = false;
    placeSpelling_ = 0;
    destKey_[0] = '\0';
    destLabel_[0] = '\0';
    candCount_ = 0;
    chosenIdx_ = -1;
    anyCandidates_ = false;
    willBeLate_ = false;
    lateByMin_ = 0;
    transfers_ = 0;
    noTransitReason_[0] = '\0';
    transitAt_ = core::invalidTs();
    everTrip_ = false;
    alertStatus_ = core::AlertStatus{};
}

void Orchestrator::refreshCalendar(const Clock& now) {
    if (!due(now.sec, lastCalendarSec_, cfg_.calendarPollSec, everCalendar_)) return;
    if (calendarBudget_.check(now.sec) != core::RateDecision::Allow) {
        ++budgetRefusals_;
        return;
    }
    calendarBudget_.take(now.sec);
    lastCalendarSec_ = now.sec;
    everCalendar_ = true;
    ++calendarPolls_;

    google::EventList list;
    const Outcome r = src_->events(now, cfg_.lookaheadMin, list);
    if (!r.ok) {
        calendarBudget_.recordFailure(now.sec);
        setFault(Fault::Calendar, r.note);
        // Keep whatever appointment we already had. An outage at Google is not
        // a cancellation, and dropping the event would silently disarm an alert
        // that is about to matter.
        return;
    }
    calendarBudget_.recordSuccess(now.sec);
    clearFault(Fault::Calendar);

    const core::CalendarEvent* pick = nullptr;
    for (uint8_t i = 0; i < list.count; ++i) {
        const core::CalendarEvent& e = list.events[i];
        if (!e.actionable()) continue;         // no location => not ours to act on
        if (e.start <= now.local) continue;    // already under way
        if (alreadyHandled(e)) continue;
        pick = &e;
        break;
    }

    if (!pick) {
        forgetEvent();
        note_ = list.count ? "events, but none we can act on yet"
                           : "the calendar is empty for now";
        return;
    }
    if (!haveEvent_ || pick->start != event_.start ||
        std::strcmp(pick->location, event_.location) != 0) {
        adopt(*pick);
    } else {
        event_ = *pick;                        // same appointment, maybe retitled
    }
}

bool Orchestrator::ensurePlace(const Clock& now) {
    if (destResolved_ && std::strcmp(destKey_, event_.location) == 0) return true;

    // The PlaceBook first, always. An exact coordinate the user gave us beats a
    // search that might come back with a different Ponttor.
    if (const core::Place* p = places_->find(event_.location)) {
        destLat_ = p->lat;
        destLon_ = p->lon;
        setStr(destKey_, sizeof destKey_, event_.location);
        foldStr(destLabel_, sizeof destLabel_, p->label);
        destResolved_ = true;
        clearFault(Fault::Place);
        return true;
    }

    // Otherwise ask AVV -- but not with the raw string. Google Calendar fills
    // `location` from Google Maps, so what arrives is usually a formatted postal
    // address: "Mathieusstrasse 30, 52072 Aachen, Germany". HAFAS wants
    // "street, city"; the country is noise and the postcode is worse.
    //
    // ⚠️ AND WHEN THE ENTRY WAS PICKED FROM THE MAP rather than typed, Google
    // puts a BUSINESS NAME in front of the address -- "Netto-Marken Discount,
    // Marienbongard 28, 52062 Aachen". AVV has never heard of the shop, so the
    // street line is asked for first and the whole string only if that finds
    // nothing. The two spellings alternate across ticks rather than both going
    // out at once, because they are two AVV requests and clause 6.a's minimum
    // interval applies between them exactly as it does everywhere else. See
    // core/address.h.
    const size_t locLen = std::strlen(event_.location);
    char query[sizeof event_.location];
    const size_t queryLen =
        placeSpelling_ == 0
            ? core::transitQuery(event_.location, locLen, query, sizeof query)
            : core::simplifyAddress(event_.location, locLen, query, sizeof query);
    if (queryLen == 0) {
        setFault(Fault::Place, "no place");
        return false;
    }

    // This is an AVV request like any other and comes out of the same budget --
    // clause 6.a does not carve out an exception for geocoding.
    if (transitBudget_.check(now.sec) != core::RateDecision::Allow) {
        ++budgetRefusals_;
        return false;
    }
    transitBudget_.take(now.sec);

    double lat = 0.0, lon = 0.0;
    const Outcome r = src_->resolve(query, lat, lon);
    if (!r.ok) {
        transitBudget_.recordFailure(now.sec);
        // "There is nothing there" is an answer, and it is the one that says to
        // ask differently next time. A transport failure or a refusal means the
        // question never really got asked, so the spelling stays put.
        if (r.notFound) placeSpelling_ = static_cast<uint8_t>(placeSpelling_ ^ 1u);
        setFault(Fault::Place, r.note[0] ? r.note : "place?");
        return false;
    }
    transitBudget_.recordSuccess(now.sec);

    destLat_ = lat;
    destLon_ = lon;
    setStr(destKey_, sizeof destKey_, event_.location);
    // The first meaningful part, not the whole address: there is room on the
    // 240x240 screen for a line, not for ", 52072 Aachen, Germany".
    core::addressLabel(event_.location, std::strlen(event_.location), destLabel_,
                       sizeof destLabel_);
    destResolved_ = true;
    clearFault(Fault::Place);
    return true;
}

void Orchestrator::refreshTrip(const Clock& now) {
    // Once a minute while the alert is up, once every fifteen while merely
    // watching. A delay only changes what you do once you are about to leave.
    const bool close = alertStatus_.state == core::AlertState::Armed || mayHoldScreen_;
    const uint32_t every = close ? cfg_.tripAlertRefreshSec : cfg_.tripIdleRefreshSec;
    if (!due(now.sec, lastTripSec_, every, everTrip_)) return;
    if (transitBudget_.check(now.sec) != core::RateDecision::Allow) {
        ++budgetRefusals_;
        return;
    }
    transitBudget_.take(now.sec);
    lastTripSec_ = now.sec;
    everTrip_ = true;
    ++tripPolls_;

    TripRequest q;
    q.fromLat = cfg_.homeLat;
    q.fromLon = cfg_.homeLon;
    q.toLat = destLat_;
    q.toLon = destLon_;
    q.arriveBy = event_.start;

    int n = 0;
    const Outcome r = src_->trip(q, scratch_, core::kMaxCandidates, n);
    if (!r.ok) {
        transitBudget_.recordFailure(now.sec);
        setFault(Fault::Transit, r.note);
        // Keep the itineraries we already have. Realtime data that is clearly
        // marked stale beats none at all -- but it MUST be marked, which is
        // what transitAt_ and stale() are for.
        return;
    }
    transitBudget_.recordSuccess(now.sec);
    clearFault(Fault::Transit);

    if (n < 0) n = 0;
    if (n > core::kMaxCandidates) n = core::kMaxCandidates;
    for (int i = 0; i < n; ++i) candidates_[i] = scratch_[i];
    candCount_ = n;
    transitAt_ = now.local;

    const core::TripChoice c = core::selectTrip(candidates_, candCount_, event_.start);
    anyCandidates_ = c.anyCandidates;
    willBeLate_ = c.willBeLate;
    lateByMin_ = c.lateByMin;
    transfers_ = c.transfers;
    chosenIdx_ = c.trip ? static_cast<int>(c.trip - candidates_) : -1;

    if (!c.trip) {
        setStr(noTransitReason_, sizeof noTransitReason_,
               c.anyCandidates ? "no usable connection" : "no connection found");
        note_ = c.anyCandidates ? "AVV has nothing usable" : "AVV found no connection";
    } else {
        noTransitReason_[0] = '\0';
        note_ = "journey planned";
    }
}

void Orchestrator::refreshCycle(const Clock& now) {
    if (!cfg_.cycleEnabled) return;
    // Keyed on the destination, and cached even when the answer was "there is
    // no way to cycle there" -- that is an answer, not a failure to get one.
    if (std::strcmp(cycleKey_, event_.location) == 0) return;
    if (!due(now.sec, lastCycleSec_, kCycleRetrySec, everCycle_)) return;
    if (cycleBudget_.check(now.sec) != core::RateDecision::Allow) {
        ++budgetRefusals_;
        return;
    }
    cycleBudget_.take(now.sec);
    lastCycleSec_ = now.sec;
    everCycle_ = true;

    TripRequest q;
    q.fromLat = cfg_.homeLat;
    q.fromLon = cfg_.homeLon;
    q.toLat = destLat_;
    q.toLon = destLon_;

    core::CycleRoute route;
    const Outcome r = src_->cycle(q, route);
    if (!r.ok) {
        cycleBudget_.recordFailure(now.sec);
        setFault(Fault::Cycle, r.note);
        return;
    }
    cycleBudget_.recordSuccess(now.sec);
    clearFault(Fault::Cycle);
    cycle_ = route;
    setStr(cycleKey_, sizeof cycleKey_, event_.location);
    comfort_ = core::computeComfort(weather_, cycle_, cfg_.comfort);
}

void Orchestrator::refreshWeather(const Clock& now) {
    // Weather is only ever used to score the ride. With no route to score,
    // fetching it would be a request nobody reads.
    if (!cfg_.cycleEnabled || !cycle_.valid) return;
    if (!due(now.sec, lastWeatherSec_, cfg_.weatherRefreshSec, everWeather_)) return;
    if (weatherBudget_.check(now.sec) != core::RateDecision::Allow) {
        ++budgetRefusals_;
        return;
    }
    weatherBudget_.take(now.sec);
    lastWeatherSec_ = now.sec;
    everWeather_ = true;

    // The hour you would actually be riding, not the hour it is now.
    const core::Timestamp at =
        alertStatus_.window.valid ? alertStatus_.window.leave : event_.start;

    core::Weather w;
    const Outcome r = src_->weather(destLat_, destLon_, at, w);
    if (!r.ok) {
        weatherBudget_.recordFailure(now.sec);
        setFault(Fault::Weather, r.note);
        return;
    }
    weatherBudget_.recordSuccess(now.sec);
    clearFault(Fault::Weather);
    weather_ = w;
    comfort_ = core::computeComfort(weather_, cycle_, cfg_.comfort);
}

void Orchestrator::refreshScreenState(const Clock& now) {
    const uint32_t every = screen_.reachable ? cfg_.pingSec : cfg_.pingWhenDownSec;
    if (!due(now.sec, lastPingSec_, every, everPing_)) return;
    lastPingSec_ = now.sec;
    everPing_ = true;

    ScreenState st;
    const Outcome r = src_->pingScreen(st);
    // A television that does not answer is unreachable, and saying so is the
    // point of the row. Do not keep the last good answer around looking fresh.
    screen_ = r.ok ? st : ScreenState{};
}

void Orchestrator::driveScreen(const Clock& now) {
    if (alertStatus_.state != core::AlertState::Armed) {
        standDown(now);
        phase_ = mayHoldScreen_ ? Phase::Standdown : Phase::Watching;
        if (note_ == nullptr || note_[0] == '\0') note_ = "waiting for the window";
        return;
    }

    phase_ = Phase::Alerting;

    // Once a minute is enough. The countdown is in minutes, and the frame gate
    // downstream drops anything that would have been byte-identical anyway --
    // but not sending it at all is cheaper than hashing it to find that out.
    if (screenConfirmed_ && now.local.min == lastShownMinute_) return;

    render::ScreenModel m;
    fillScreen(m);

    // Set the flag BEFORE the upload, not after. If showScreen() half-succeeds
    // -- the file lands and the theme switch does not, or the reply is lost --
    // then something of ours is on a 1 MB partition and we have to be the ones
    // who remember. releaseScreen() is idempotent, so calling it for an upload
    // that never happened costs one request; NOT calling it for one that did
    // costs a file for ever.
    mayHoldScreen_ = true;

    const Outcome r = src_->showScreen(m);
    if (!r.ok) {
        setFault(Fault::Screen, r.note[0] ? r.note : "tv failed");
        screenConfirmed_ = false;
        return;
    }
    clearFault(Fault::Screen);
    screenConfirmed_ = true;
    lastShownMinute_ = now.local.min;
    ++screenPushes_;
}

void Orchestrator::standDown(const Clock&) {
    if (!mayHoldScreen_) return;

    const Outcome r = src_->releaseScreen();
    if (!r.ok) {
        // Do NOT clear the flag. A television that will not give the screen
        // back is still holding our file, and retrying on the next tick is the
        // entire reason this is a flag and not a local variable.
        setFault(Fault::Screen, r.note[0] ? r.note : "tv held");
        return;
    }
    mayHoldScreen_ = false;
    screenConfirmed_ = false;
    lastShownMinute_ = INT32_MIN;
    ++screenReleases_;
    clearFault(Fault::Screen);
}

// ---------------------------------------------------------------------------

void Orchestrator::fillScreen(render::ScreenModel& m) const {
    m = render::ScreenModel{};

    m.minutesToLeave = alertStatus_.minutesToLeave;
    core::formatHHMM(lastLocal_, m.nowText);
    foldStr(m.destination, sizeof m.destination,
            destLabel_[0] ? destLabel_ : event_.location);
    core::formatHHMM(event_.start, m.eventTime);
    // What it is CALLED, under where it is. Already ASCII-folded on the way in
    // from Google, which is what the 240x240 fonts can draw.
    foldStr(m.eventTitle, sizeof m.eventTitle, event_.title);

    if (const core::Itinerary* it = chosen()) {
        m.hasTransit = true;
        if (const core::Leg* l = it->firstTransitLeg()) {
            setStr(m.line, sizeof m.line, l->line);
            foldStr(m.boardStop, sizeof m.boardStop, l->fromName);
            core::formatHHMM(l->depTime(), m.depTime);
        } else {
            // An all-walking itinerary is a real answer: it is close enough to
            // walk. Say the departure without inventing a line number for it.
            core::formatHHMM(it->departure(), m.depTime);
        }
        core::formatHHMM(it->arrival(), m.arrTime);
        m.delayMin = it->depDelayMin();
        m.cancelled = it->cancelled;
    } else {
        m.hasTransit = false;
        setStr(m.noTransitReason, sizeof m.noTransitReason,
               noTransitReason_[0] ? noTransitReason_ : "no connection");
    }

    m.willBeLate = willBeLate_;
    m.lateByMin = lateByMin_;
    m.transfers = transfers_;

    if (cycle_.valid) {
        m.hasCycle = true;
        m.cycleMin = cycle_.durationMin;
        m.ascentM = static_cast<int32_t>(cycle_.ascentM);
        m.comfortScore = comfort_.score;
        m.comfortLabel = comfort_.label();
        m.comfortReason = comfort_.reasonText();
    }

    m.stale = stale();
    if (fault_ != Fault::None) setStr(m.statusText, sizeof m.statusText, faultText_);
}

void Orchestrator::fillHealth(oled::HealthModel& m) const {
    core::formatHHMM(lastLocal_, m.clock);
    m.clockSynced = clockSynced_;

    m.haveEvent = haveEvent_;
    if (haveEvent_) {
        core::formatHHMM(event_.start, m.eventTime);
        foldStr(m.eventPlace, sizeof m.eventPlace,
                destLabel_[0] ? destLabel_ : event_.location);
    }

    m.stateText = phaseName(phase_);
    m.minutesToLeave = alertStatus_.minutesToLeave;
    m.alerting = screenConfirmed_;

    m.tvReachable = screen_.reachable;
    // Our file is on the device and should not be -- either the ping saw it, or
    // we know we failed to give the screen back. This outranks every other
    // television status because on a ~1 MB partition it is the one that
    // eventually stops the whole system working.
    m.tvImageLeftBehind = screen_.imageLeftBehind ||
                          (mayHoldScreen_ && phase_ == Phase::Standdown);
    m.tvFreeKnown = screen_.freeKnown;
    m.tvFreeKb = screen_.freeKb;

    if (!transitAt_.valid() || !lastLocal_.valid()) {
        // NEVER fetched is not the same as fresh and must not read the same.
        m.transitAgeMin = -1;
    } else {
        int32_t age = core::diffMinutes(lastLocal_, transitAt_);
        if (age < 0) age = 0;             // a clock jump is not a negative age
        if (age > 999) age = 999;
        m.transitAgeMin = age;
    }
    m.transitStale = stale();

    m.requestsThisHour = transitBudget_.usedLastHour(lastSec_);
    m.requestsPerHourCap = transitBudget_.config().maxPerHour;
    // Only a real backoff, never the routine minimum interval: a panel that
    // says "wait 14s" every fourteen seconds is a panel nobody reads.
    m.backoffSec = transitBudget_.consecutiveFailures() > 0
                       ? transitBudget_.secondsUntilAllowed(lastSec_)
                       : 0;

    if (fault_ != Fault::None) setStr(m.errorText, sizeof m.errorText, faultText_);
}

} // namespace app
