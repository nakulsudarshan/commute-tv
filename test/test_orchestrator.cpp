#include "test.h"
#include "../src/app/orchestrator.h"

#include <cstring>
#include <string>
#include <vector>

using namespace app;

namespace {

// A scripted Sources. Every method records that it was called and hands back
// whatever the test told it to -- which is the point: the orchestrator's job is
// sequencing under failure, and "openrouteservice returned 429 while we were
// holding the screen" is a two-line setup here and an afternoon of contrived
// JSON otherwise.
struct FakeSources : Sources {
    // --- what to hand back --------------------------------------------------
    Outcome eventsOut = Outcome::good();
    Outcome resolveOut = Outcome::good();
    Outcome tripOut = Outcome::good();
    Outcome cycleOut = Outcome::good();
    Outcome weatherOut = Outcome::good();
    Outcome pingOut = Outcome::good();
    Outcome showOut = Outcome::good();
    Outcome releaseOut = Outcome::good();

    google::EventList events_;
    std::vector<core::Itinerary> trips;
    double resolveLat = 50.7, resolveLon = 6.1;
    core::CycleRoute cycleRoute;
    core::Weather weatherData;
    ScreenState screenState;

    // --- what happened ------------------------------------------------------
    int eventCalls = 0, resolveCalls = 0, tripCalls = 0, cycleCalls = 0;
    int weatherCalls = 0, pingCalls = 0, showCalls = 0, releaseCalls = 0;
    render::ScreenModel lastShown;
    // The exact order of screen operations, because "was the screen given back"
    // is a question about sequence, not about counts.
    std::vector<char> screenLog;    // 'S' = show, 'R' = release

    Outcome events(const Clock&, int32_t, google::EventList& out) override {
        ++eventCalls;
        if (eventsOut.ok) out = events_;
        return eventsOut;
    }
    char lastResolveQuery[96] = {0};
    // Every question asked, in order. `resolve` can be asked twice for one
    // location -- the street line first, the whole Google string second -- and a
    // test that only sees the last one cannot tell which was which.
    std::vector<std::string> resolveQueries;
    // Answers in order; the last one is repeated once the script runs out.
    // Empty means "always resolveOut".
    std::vector<Outcome> resolveScript;
    // Never out of range. A test that asks about a question which was never
    // asked should FAIL, legibly -- not index past the end and take the whole
    // run down with it, which is what the first draft of these tests did.
    const char* query(size_t i) const {
        return i < resolveQueries.size() ? resolveQueries[i].c_str() : "(not asked)";
    }
    Outcome resolve(const char* text, double& lat, double& lon) override {
        ++resolveCalls;
        core::copyStr(lastResolveQuery, sizeof lastResolveQuery, text ? text : "",
                      text ? std::strlen(text) : 0);
        resolveQueries.push_back(text ? text : "");
        Outcome out = resolveOut;
        if (!resolveScript.empty()) {
            const size_t i = static_cast<size_t>(resolveCalls) - 1;
            out = resolveScript[i < resolveScript.size() ? i
                                                         : resolveScript.size() - 1];
        }
        if (out.ok) { lat = resolveLat; lon = resolveLon; }
        return out;
    }
    Outcome trip(const TripRequest&, core::Itinerary* out, int cap, int& count) override {
        ++tripCalls;
        count = 0;
        if (!tripOut.ok) return tripOut;
        for (size_t i = 0; i < trips.size() && count < cap; ++i) out[count++] = trips[i];
        return tripOut;
    }
    Outcome cycle(const TripRequest&, core::CycleRoute& out) override {
        ++cycleCalls;
        if (cycleOut.ok) out = cycleRoute;
        return cycleOut;
    }
    Outcome weather(double, double, core::Timestamp, core::Weather& out) override {
        ++weatherCalls;
        if (weatherOut.ok) out = weatherData;
        return weatherOut;
    }
    Outcome pingScreen(ScreenState& out) override {
        ++pingCalls;
        if (pingOut.ok) out = screenState;
        return pingOut;
    }
    Outcome showScreen(const render::ScreenModel& m) override {
        ++showCalls;
        if (showOut.ok) { lastShown = m; screenLog.push_back('S'); }
        return showOut;
    }
    Outcome releaseScreen() override {
        ++releaseCalls;
        if (releaseOut.ok) screenLog.push_back('R');
        return releaseOut;
    }

    std::string log() const { return std::string(screenLog.begin(), screenLog.end()); }
};

// 09:00 on an arbitrary day, in minutes-since-epoch as core::Timestamp counts.
constexpr int32_t kDay = 20000 * 1440;
core::Timestamp at(int h, int m) { return core::makeTs(kDay + h * 60 + m); }

Clock clockAt(int h, int m, uint32_t sec) {
    Clock c;
    c.local = at(h, m);
    c.sec = sec;
    c.utcSec = static_cast<int64_t>(sec);
    c.synced = true;
    return c;
}

core::CalendarEvent meeting(const char* where, int h, int m) {
    core::CalendarEvent e;
    core::copyStr(e.title, sizeof e.title, "Lecture", 7);
    core::copyStr(e.location, sizeof e.location, where, std::strlen(where));
    e.start = at(h, m);
    return e;
}

// One bus leg: board at depH:depM, arrive arrH:arrM.
core::Itinerary bus(int depH, int depM, int arrH, int arrM, int delayMin = 0,
                    int32_t transfers = 0) {
    core::Itinerary it;
    it.transfers = transfers;
    core::Leg& l = it.legs[0];
    l.mode = core::LegMode::Bus;
    core::copyStr(l.line, sizeof l.line, "3B", 2);
    core::copyStr(l.fromName, sizeof l.fromName, "Ponttor", 7);
    core::copyStr(l.toName, sizeof l.toName, "Bushof", 6);
    l.plannedDep = at(depH, depM);
    l.plannedArr = at(arrH, arrM);
    if (delayMin) {
        l.rtDep = core::addMinutes(l.plannedDep, delayMin);
        l.rtArr = core::addMinutes(l.plannedArr, delayMin);
    }
    it.legCount = 1;
    return it;
}

Config testConfig() {
    Config c;
    c.homeLat = 50.77;
    c.homeLon = 6.08;
    c.alert.leadMin = 30;
    c.alert.graceMin = 5;
    c.alert.safetyBufferMin = 5;
    // Budgets wide open unless a test narrows them: this suite is about
    // sequencing, and a surprise rate limit would make every result ambiguous.
    c.calendar.minIntervalSec = 0;
    c.transit.minIntervalSec = 0;
    c.cycling.minIntervalSec = 0;
    c.forecast.minIntervalSec = 0;
    c.calendar.maxPerHour = c.transit.maxPerHour = 600;
    c.cycling.maxPerHour = c.forecast.maxPerHour = 600;
    c.calendar.maxPerDay = c.transit.maxPerDay = 5000;
    c.cycling.maxPerDay = c.forecast.maxPerDay = 5000;
    return c;
}

// The common arrangement: one appointment at 10:00 at a known place, one bus
// leaving at 09:20. leave = 09:20 - 5 safety = 09:15; the alert opens 30 min
// before that, at 08:45, and closes 5 min after, at 09:20.
struct Rig {
    FakeSources src;
    core::PlaceBook places;
    Config cfg = testConfig();

    Rig() {
        places.add("SuperC", 50.7788, 6.0780);
        src.events_.events[0] = meeting("SuperC", 10, 0);
        src.events_.count = 1;
        src.trips.push_back(bus(9, 20, 9, 50));
        src.screenState.reachable = true;
        src.screenState.freeKnown = true;
        src.screenState.freeKb = 943;
    }
};

} // namespace

void test_orchestrator() {
    t::suite("orchestrator / it does not live on the stack");
    {
        // A REGRESSION GUARD, not a design statement. The first hardware run of
        // the full pipeline rebooted with `Double exception` on every tick,
        // because refreshTrip() held a six-itinerary buffer on the stack while
        // LiveSources::trip held another one -- 11,776 bytes against the
        // Arduino loop task's 8,192.
        //
        // Nothing in a host test can see a stack overflow on an ESP32. What it
        // CAN see is the buffers no longer being locals: if someone moves them
        // back, the object shrinks and this fails.
        constexpr size_t kOneSetOfTrips = sizeof(core::Itinerary) * core::kMaxCandidates;
        CHECK(sizeof(Orchestrator) >= 2 * kOneSetOfTrips);

        // And the reason it matters, stated in numbers so the next person does
        // not have to rediscover it from a register dump.
        CHECK(kOneSetOfTrips > 4096);
    }

    t::suite("orchestrator / before the clock is right");
    {
        Rig r;
        Orchestrator o(r.cfg, r.src, r.places);
        Clock c;
        c.synced = false;                 // NTP has not answered
        o.tick(c);
        CHECK(o.phase() == Phase::NoClock);
        CHECK(o.fault() == Fault::Clock);
        // Nothing may be asked of anything: every decision below needs the time.
        CHECK_EQ(r.src.eventCalls, 0);
        CHECK_EQ(r.src.tripCalls, 0);
        CHECK_EQ(r.src.showCalls, 0);
    }
    {
        // And if the clock goes away WHILE we are holding the screen, the screen
        // goes back. A frozen countdown is worse than no countdown.
        Rig r;
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 50, 1000));
        CHECK(o.screenConfirmed());
        Clock dead;
        dead.synced = false;
        o.tick(dead);
        CHECK(!o.mayHoldScreen());
        CHECK_STR(r.src.log().c_str(), "SR");
    }

    t::suite("orchestrator / an empty calendar costs nothing");
    {
        Rig r;
        r.src.events_.count = 0;
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(9, 0, 1000));
        CHECK(o.phase() == Phase::Idle);
        CHECK(!o.haveEvent());
        // No event means no journey to plan. Asking AVV anyway would be a
        // request against a contractual budget for an answer nobody reads.
        CHECK_EQ(r.src.tripCalls, 0);
        CHECK_EQ(r.src.resolveCalls, 0);
        CHECK_EQ(r.src.cycleCalls, 0);
        CHECK_EQ(r.src.weatherCalls, 0);
    }
    {
        // An appointment with no location is not ours to act on -- that is the
        // rule the whole project is built around.
        Rig r;
        r.src.events_.events[0] = meeting("", 10, 0);
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(9, 0, 1000));
        CHECK(!o.haveEvent());
        CHECK_EQ(r.src.tripCalls, 0);
    }

    t::suite("orchestrator / finding the place");
    {
        // A place the user named is an exact coordinate. Never ask AVV to guess
        // at a name we already know the answer to.
        Rig r;
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(9, 0, 1000));
        CHECK_EQ(r.src.resolveCalls, 0);
        CHECK_EQ(r.src.tripCalls, 1);
    }
    {
        Rig r;
        r.src.events_.events[0] = meeting("Somewhere Else", 10, 0);
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(9, 0, 1000));
        CHECK_EQ(r.src.resolveCalls, 1);
        CHECK_EQ(r.src.tripCalls, 1);
    }
    {
        // A place nobody can resolve is not a journey. Say so, plan nothing.
        Rig r;
        r.src.events_.events[0] = meeting("Atlantis", 10, 0);
        r.src.resolveOut = Outcome::fail("no such stop");
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(9, 0, 1000));
        CHECK(o.fault() == Fault::Place);
        CHECK_EQ(r.src.tripCalls, 0);
        CHECK_EQ(r.src.showCalls, 0);
    }

    t::suite("orchestrator / it prefers a journey with fewer changes");
    {
        // The real one: a direct bus and a two-change alternative that leaves
        // slightly later. The old rule took the later departure and would have
        // sent Nakul the long way round.
        Rig r;
        r.src.trips.clear();
        r.src.trips.push_back(bus(9, 20, 9, 50, 0, 0));    // direct
        r.src.trips.push_back(bus(9, 30, 9, 55, 0, 2));    // two changes, later
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 50, 1000));

        render::ScreenModel s;
        o.fillScreen(s);
        CHECK_EQ(s.transfers, 0);
        CHECK_STR(s.depTime, "09:20");
    }

    t::suite("orchestrator / a Google Maps address, which is what actually arrives");
    {
        // Nakul's calendar entries come from the Google Calendar app, so
        // `location` is normally a formatted postal address rather than a name
        // he chose. AVV must be asked for "street, city" -- the country is noise
        // and the postcode is worse.
        Rig r;
        r.src.events_.events[0] = meeting("Mathieusstrasse 30, 52072 Aachen, Germany", 10, 0);
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 50, 1000));

        CHECK_EQ(r.src.resolveCalls, 1);
        CHECK_STR(r.src.lastResolveQuery, "Mathieusstrasse 30, Aachen");

        // And the screen gets a line, not an address.
        render::ScreenModel s;
        o.fillScreen(s);
        CHECK_STR(s.destination, "Mathieusstrasse 30");
    }
    {
        // ⚠️ THE ONE THAT ACTUALLY BROKE. Picking a shop from Google Maps rather
        // than typing an address puts the BUSINESS NAME in front of it, and AVV
        // has never heard of the shop. Ask for the street line, not the shop.
        Rig r;
        r.src.events_.events[0] =
            meeting("Netto-Marken Discount, Marienbongard 28, 52062 Aachen", 10, 0);
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 50, 1000));

        CHECK_EQ(r.src.resolveCalls, 1);
        CHECK_STR(r.src.lastResolveQuery, "Marienbongard 28, Aachen");

        // But the shop is still what he is going to, so it is what he is shown.
        render::ScreenModel s;
        o.fillScreen(s);
        CHECK_STR(s.destination, "Netto-Marken Discount");
    }
    {
        // And when the street line finds nothing, the whole string is tried on
        // the NEXT tick -- some places really are known to AVV by name and not
        // by address. Two ticks and not one, because two AVV requests in the
        // same second is exactly what clause 6.a's minimum interval forbids.
        Rig r;
        r.src.events_.events[0] =
            meeting("Elisenbrunnen Cafe, Friedrich-Wilhelm-Platz 1, 52062 Aachen", 10, 0);
        r.src.resolveScript = { Outcome::missing("place?"), Outcome::good() };
        Orchestrator o(r.cfg, r.src, r.places);

        o.tick(clockAt(8, 50, 1000));
        CHECK_EQ(r.src.resolveCalls, 1);
        CHECK_EQ(o.fault(), Fault::Place);

        o.tick(clockAt(8, 51, 1060));
        CHECK_EQ(r.src.resolveCalls, 2);
        CHECK_STR(r.src.query(0), "Friedrich-Wilhelm-Platz 1, Aachen");
        CHECK_STR(r.src.query(1),
                  "Elisenbrunnen Cafe, Friedrich-Wilhelm-Platz 1, Aachen");
        CHECK_EQ(r.src.tripCalls, 1);           // it got there in the end
        CHECK(o.fault() != Fault::Place);
    }
    {
        // A failure to REACH AVV is not a reason to ask it something else. The
        // question never got asked, so the same question is the right one to
        // ask again -- two different bad requests is the wrong answer to a flat
        // network, and it spends the budget clause 6.a exists to protect.
        Rig r;
        r.src.events_.events[0] =
            meeting("Netto-Marken Discount, Marienbongard 28, 52062 Aachen", 10, 0);
        r.src.resolveOut = Outcome::fail("avv no reply");
        Orchestrator o(r.cfg, r.src, r.places);

        o.tick(clockAt(8, 50, 1000));
        o.tick(clockAt(8, 51, 1060));
        CHECK_EQ(r.src.resolveCalls, 2);
        CHECK_STR(r.src.query(0), "Marienbongard 28, Aachen");
        CHECK_STR(r.src.query(1), "Marienbongard 28, Aachen");
        CHECK_EQ(o.fault(), Fault::Place);
    }
    {
        // Nothing found either way is a fault, not a loop: it keeps alternating
        // between the two spellings rather than settling on the worse one.
        Rig r;
        r.src.events_.events[0] =
            meeting("Nowhere At All, Imaginary Str. 9, 52062 Aachen", 10, 0);
        r.src.resolveOut = Outcome::missing("place?");
        Orchestrator o(r.cfg, r.src, r.places);

        o.tick(clockAt(8, 50, 1000));
        o.tick(clockAt(8, 51, 1060));
        o.tick(clockAt(8, 52, 1120));
        CHECK_EQ(r.src.resolveCalls, 3);
        CHECK_STR(r.src.query(0), "Imaginary Str. 9, Aachen");
        CHECK_STR(r.src.query(1),
                  "Nowhere At All, Imaginary Str. 9, Aachen");
        CHECK_STR(r.src.query(2), "Imaginary Str. 9, Aachen");
        CHECK_EQ(o.fault(), Fault::Place);
        CHECK_EQ(r.src.tripCalls, 0);
    }
    {
        // No street to find: the two spellings are the same string, so the
        // alternation is invisible and no request is wasted saying it twice.
        Rig r;
        r.src.events_.events[0] = meeting("Aachen Bushof", 10, 0);
        r.src.resolveOut = Outcome::missing("place?");
        Orchestrator o(r.cfg, r.src, r.places);

        o.tick(clockAt(8, 50, 1000));
        o.tick(clockAt(8, 51, 1060));
        CHECK_EQ(r.src.resolveCalls, 2);
        CHECK_STR(r.src.query(0), "Aachen Bushof");
        CHECK_STR(r.src.query(1), "Aachen Bushof");
    }
    {
        // A labelled place still wins, even buried in a full Google address --
        // PlaceBook matches on whole-word containment, so the coordinate Nakul
        // gave us beats a search that might find a different Ponttor.
        Rig r;
        r.src.events_.events[0] =
            meeting("SuperC, Templergraben 57, 52062 Aachen, Germany", 10, 0);
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 50, 1000));
        CHECK_EQ(r.src.resolveCalls, 0);
        CHECK_EQ(r.src.tripCalls, 1);

        render::ScreenModel s;
        o.fillScreen(s);
        CHECK_STR(s.destination, "SuperC");     // his word for it, not the street
    }
    {
        // A location that is nothing but a country is not a destination, and
        // must not become an empty AVV query.
        Rig r;
        r.src.events_.events[0] = meeting("Germany", 10, 0);
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 50, 1000));
        CHECK_EQ(r.src.resolveCalls, 0);
        CHECK(o.fault() == Fault::Place);
        CHECK_EQ(r.src.tripCalls, 0);
    }

    t::suite("orchestrator / the alert hangs off the departure, not the meeting");
    {
        Rig r;
        Orchestrator o(r.cfg, r.src, r.places);
        // 08:30 is an hour and a half before the 10:00 meeting but still 15 min
        // before the alert opens, because the bus goes at 09:20.
        o.tick(clockAt(8, 30, 1000));
        CHECK(o.phase() == Phase::Watching);
        CHECK_EQ(r.src.showCalls, 0);

        o.tick(clockAt(8, 45, 2000));    // leave 09:15 minus 30 lead
        CHECK(o.phase() == Phase::Alerting);
        CHECK_EQ(r.src.showCalls, 1);
        CHECK_EQ(o.alert().minutesToLeave, 30);
    }
    {
        // Same meeting, a bus an hour earlier: the alert opens an hour earlier.
        Rig r;
        r.src.trips.clear();
        r.src.trips.push_back(bus(8, 20, 8, 50));
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(7, 45, 1000));
        CHECK(o.phase() == Phase::Alerting);
    }

    t::suite("orchestrator / the screen is always given back");
    {
        // The ordinary path: take it, hold it, hand it back once the moment is
        // gone.
        Rig r;
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 50, 1000));
        CHECK(o.screenConfirmed());
        o.tick(clockAt(9, 30, 5000));    // past leave + grace
        CHECK(!o.mayHoldScreen());
        CHECK_STR(r.src.log().c_str(), "SR");
        CHECK_EQ(o.screenReleases(), 1u);
    }
    {
        // The appointment vanishes from the calendar mid-alert -- cancelled, or
        // moved. The screen must not stay ours.
        Rig r;
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 50, 1000));
        CHECK(o.mayHoldScreen());
        r.src.events_.count = 0;
        o.tick(clockAt(8, 51, 1400));
        CHECK(!o.mayHoldScreen());
        CHECK(!o.haveEvent());
        CHECK_STR(r.src.log().c_str(), "SR");
    }
    {
        // A television that will not let go is retried, and the panel ranks it
        // above everything else, because on a ~1 MB partition that file is what
        // eventually stops the system working.
        Rig r;
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 50, 1000));
        r.src.releaseOut = Outcome::fail("tv held");
        r.src.events_.count = 0;
        o.tick(clockAt(8, 51, 1400));
        CHECK(o.mayHoldScreen());              // still ours as far as we know
        CHECK(o.phase() == Phase::Standdown);
        CHECK_EQ(r.src.releaseCalls, 1);

        oled::HealthModel h;
        o.fillHealth(h);
        CHECK(h.tvImageLeftBehind);

        r.src.releaseOut = Outcome::good();
        o.tick(clockAt(8, 52, 1800));
        CHECK(!o.mayHoldScreen());
        CHECK_EQ(r.src.releaseCalls, 2);
    }
    {
        // An upload that fails still counts as "might be on the device". This is
        // the conservative direction on purpose: one wasted release beats a file
        // that outlives the reason for it.
        Rig r;
        r.src.showOut = Outcome::fail("upload rejected");
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 50, 1000));
        CHECK(o.mayHoldScreen());
        CHECK(!o.screenConfirmed());           // and the badge does NOT lie
        CHECK(o.fault() == Fault::Screen);
        o.tick(clockAt(9, 30, 5000));
        CHECK_EQ(r.src.releaseCalls, 1);
    }
    {
        Rig r;
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 50, 1000));
        o.shutdown();
        CHECK(!o.mayHoldScreen());
        CHECK(!o.haveEvent());
        CHECK_STR(r.src.log().c_str(), "SR");
    }

    t::suite("orchestrator / an outage is not a cancellation");
    {
        // Google being unreachable must not silently disarm an alert that is
        // about to matter.
        Rig r;
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 50, 1000));
        CHECK(o.haveEvent());
        r.src.eventsOut = Outcome::fail("no reply");
        o.tick(clockAt(8, 51, 1400));
        CHECK(o.haveEvent());
        CHECK(o.mayHoldScreen());
        CHECK(o.fault() == Fault::Calendar);
    }
    {
        // AVV going quiet keeps the itineraries we have -- and marks them, which
        // is the whole difference between useful and dangerous.
        Rig r;
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 50, 1000));
        oled::HealthModel fresh;
        o.fillHealth(fresh);
        CHECK_EQ(fresh.transitAgeMin, 0);
        CHECK(!fresh.transitStale);

        r.src.tripOut = Outcome::fail("avv down");
        o.tick(clockAt(9, 5, 2000));           // 15 minutes later
        oled::HealthModel old;
        o.fillHealth(old);
        CHECK_EQ(old.transitAgeMin, 15);
        CHECK(old.transitStale);
        CHECK(o.fault() == Fault::Transit);

        render::ScreenModel s;
        o.fillScreen(s);
        CHECK(s.stale);
        CHECK(s.hasTransit);                   // still the best we know
    }
    {
        // Never fetched is NOT the same as fetched-just-now, and the two must
        // not read alike. This is the same rule the health panel already lives
        // by; the orchestrator is where the value comes from.
        Rig r;
        r.src.events_.count = 0;
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(9, 0, 1000));
        oled::HealthModel h;
        o.fillHealth(h);
        CHECK_EQ(h.transitAgeMin, -1);
    }

    t::suite("orchestrator / the alert does not re-arm");
    {
        // The subtle one. Passed means the DEPARTURE is behind us -- the 10:00
        // meeting is still an hour away, so the next calendar poll hands back
        // the very same entry. Without a memory of what it has already seen
        // through, the countdown starts over.
        Rig r;
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 50, 1000));
        CHECK(o.phase() == Phase::Alerting);
        o.tick(clockAt(9, 25, 4000));          // leave 09:15 + 5 grace, gone
        CHECK(!o.mayHoldScreen());
        CHECK(!o.haveEvent());

        // AVV now offers the NEXT bus, exactly as it would in life -- the 10:00
        // meeting has not happened yet, so there is still a way to get there.
        // That is what makes this trap real: without a memory of the
        // appointment it has already seen through, the orchestrator re-adopts
        // it, re-plans it, and starts a second countdown for a moment that has
        // already been and gone.
        r.src.trips.clear();
        r.src.trips.push_back(bus(9, 50, 10, 20));
        const int tripsBefore = r.src.tripCalls;
        for (int i = 0; i < 5; ++i) o.tick(clockAt(9, 26 + i, 4300u + 60u * i));

        CHECK(o.phase() == Phase::Idle);
        CHECK(!o.haveEvent());
        CHECK_EQ(r.src.showCalls, 1);          // never a second alert
        CHECK_STR(r.src.log().c_str(), "SR");
        // And it stops ASKING, too. Re-adopting would reset the trip cache and
        // spend an AVV request every calendar poll on an appointment that is
        // over -- which is precisely the mass-retrieval clause 6.a is about.
        CHECK_EQ(r.src.tripCalls, tripsBefore);
    }
    {
        // But a genuinely different appointment at the same place still arms.
        Rig r;
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 50, 1000));
        o.tick(clockAt(9, 25, 4000));
        CHECK(!o.haveEvent());

        r.src.events_.events[0] = meeting("SuperC", 12, 0);
        r.src.trips.clear();
        r.src.trips.push_back(bus(11, 20, 11, 50));
        o.tick(clockAt(10, 45, 8000));
        CHECK(o.haveEvent());
        CHECK(o.phase() == Phase::Alerting);
    }

    t::suite("orchestrator / asking no more often than it must");
    {
        // Watching: AVV once every fifteen minutes. Alerting: once a minute.
        // The difference is the whole of clause 6.a in one number.
        Rig r;
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 0, 1000));
        CHECK_EQ(r.src.tripCalls, 1);
        o.tick(clockAt(8, 5, 1300));
        CHECK_EQ(r.src.tripCalls, 1);          // five minutes: not yet
        o.tick(clockAt(8, 16, 1960));
        CHECK_EQ(r.src.tripCalls, 2);          // sixteen: due

        o.tick(clockAt(8, 50, 4000));          // alert opens
        const int during = r.src.tripCalls;
        o.tick(clockAt(8, 51, 4060));
        CHECK_EQ(r.src.tripCalls, during + 1); // once a minute now
    }
    {
        // One frame a minute, however often tick() is called.
        Rig r;
        Orchestrator o(r.cfg, r.src, r.places);
        for (int i = 0; i < 10; ++i) o.tick(clockAt(8, 50, 4000u + static_cast<uint32_t>(i)));
        CHECK_EQ(r.src.showCalls, 1);
        o.tick(clockAt(8, 51, 4060));
        CHECK_EQ(r.src.showCalls, 2);
    }
    {
        // A budget refusal is not a fault and must never show as one -- it is
        // the system working exactly as the agreement requires.
        Rig r;
        r.cfg.transit.minIntervalSec = 3600;
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 0, 1000));
        CHECK_EQ(r.src.tripCalls, 1);
        o.tick(clockAt(8, 30, 2800));
        CHECK_EQ(r.src.tripCalls, 1);
        CHECK(o.fault() == Fault::None);
        CHECK(o.budgetRefusals() > 0);
    }

    t::suite("orchestrator / the bicycle");
    {
        // The road does not move: one request per destination, cached after.
        Rig r;
        r.src.cycleRoute.valid = true;
        r.src.cycleRoute.durationMin = 18;
        r.src.cycleRoute.distanceKm = 4.2f;
        r.src.cycleRoute.ascentM = 40.0f;
        r.src.weatherData.valid = true;
        r.src.weatherData.tempC = 18.0f;
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 0, 1000));
        CHECK_EQ(r.src.cycleCalls, 1);
        CHECK_EQ(r.src.weatherCalls, 1);
        for (int i = 0; i < 5; ++i) o.tick(clockAt(8, 1 + i, 1100u + 60u * i));
        CHECK_EQ(r.src.cycleCalls, 1);
        CHECK_EQ(r.src.weatherCalls, 1);

        o.tick(clockAt(8, 50, 4000));
        render::ScreenModel s;
        o.fillScreen(s);
        CHECK(s.hasCycle);
        CHECK_EQ(s.cycleMin, 18);
        CHECK(s.comfortScore > 0);
    }
    {
        // "There is no way to cycle there" is an ANSWER. Caching it is the
        // difference between one request and one every five minutes for ever.
        Rig r;
        r.src.cycleRoute.valid = false;        // ok, but no route
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 0, 1000));
        CHECK_EQ(r.src.cycleCalls, 1);
        o.tick(clockAt(8, 30, 2800));
        CHECK_EQ(r.src.cycleCalls, 1);
        // And with nothing to score, the forecast is a request nobody reads.
        CHECK_EQ(r.src.weatherCalls, 0);
    }

    t::suite("orchestrator / what the screen says");
    {
        Rig r;
        r.src.trips.clear();
        r.src.trips.push_back(bus(9, 20, 9, 50, 4));   // four minutes late
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 50, 1000));

        render::ScreenModel s;
        o.fillScreen(s);
        CHECK_STR(s.destination, "SuperC");            // the user's own word
        CHECK_STR(s.eventTime, "10:00");
        CHECK_STR(s.nowText, "08:50");
        CHECK(s.hasTransit);
        CHECK_STR(s.line, "3B");
        CHECK_STR(s.boardStop, "Ponttor");
        CHECK_STR(s.depTime, "09:24");                 // realtime, not planned
        CHECK_EQ(s.delayMin, 4);
        CHECK_STR(s.eventTitle, "Lecture");             // what it is called
        CHECK_EQ(s.transfers, 0);
    }
    {
        // AVV answered and there is nothing there. That is a sentence, not a
        // blank screen.
        Rig r;
        r.src.trips.clear();
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 50, 1000));
        render::ScreenModel s;
        o.fillScreen(s);
        CHECK(!s.hasTransit);
        CHECK(s.noTransitReason[0] != '\0');
    }
    {
        // The health panel, end to end.
        Rig r;
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 50, 1000));
        oled::HealthModel h;
        o.fillHealth(h);
        CHECK_STR(h.clock, "08:50");
        CHECK(h.clockSynced);
        CHECK(h.haveEvent);
        CHECK_STR(h.eventTime, "10:00");
        CHECK_STR(h.eventPlace, "SuperC");
        CHECK_STR(h.stateText, "alerting");
        CHECK(h.alerting);
        CHECK(h.tvReachable);
        CHECK(h.tvFreeKnown);
        CHECK_EQ(h.tvFreeKb, 943u);
        CHECK(!h.tvImageLeftBehind);
        CHECK_EQ(h.minutesToLeave, 25);
        CHECK_STR(h.errorText, "");
    }
    {
        // A television that does not answer says so, and does not keep the last
        // good answer around looking current.
        Rig r;
        Orchestrator o(r.cfg, r.src, r.places);
        o.tick(clockAt(8, 50, 1000));
        r.src.pingOut = Outcome::fail("no reply");
        o.tick(clockAt(9, 0, 1000 + 400));
        oled::HealthModel h;
        o.fillHealth(h);
        CHECK(!h.tvReachable);
        CHECK(!h.tvFreeKnown);
    }
}
