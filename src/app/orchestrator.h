// orchestrator.h - the loop that ties everything together.
//
// Everything under src/ up to now answers one question well: what does AVV say,
// what is on the calendar, how hard is that hill, what does the panel look like.
// This is the piece that decides WHEN to ask and WHAT TO DO with the answers,
// and it is the only place in the firmware that knows the order of operations.
//
// Three ideas run through it.
//
// 1. IT TALKS TO `Sources`, NOT TO THE NETWORK. The orchestrator's real job is
//    sequencing under failure -- AVV refuses, the calendar empties mid-alert,
//    the television is unplugged while we are holding its screen. Those are the
//    cases worth testing, and provoking them through hand-written JSON would
//    make the tests about JSON. The clients already have their own tests for
//    that. So the seam is a five-method interface, `LiveSources` wires the real
//    clients behind it, and the tests drive the same code with a fake.
//
// 2. EVERY OUTBOUND REQUEST GOES THROUGH A BUDGET. Clause 6.a of the AVV
//    agreement requires technical measures against mass retrieval, and a
//    firmware bug that polls in a tight loop is exactly what that clause is
//    about. Each upstream gets its own core::RequestBudget because they have
//    genuinely different limits -- AVV's is contractual, openrouteservice's is
//    2,000 a day, Open-Meteo's is generous, Google's is effectively unlimited.
//    A budget refusal is not an error and is never shown as one.
//
// 3. THE SCREEN IS GIVEN BACK ON EVERY PATH. The SmallTV has a ~1 MB image
//    partition and stock firmware that keeps whatever you upload for ever. If
//    this code takes the screen and then loses the event, the clock, or its
//    nerve, it must still hand the screen back and delete the file. Every exit
//    from Alerting goes through standDown(), and a failed release is retried
//    rather than forgotten -- see `imageLeftBehind`, which the health panel
//    ranks above every other television status for that reason.
//
// It does NOT render anything. It fills a render::ScreenModel and an
// oled::HealthModel and hands them over; how a 240x240 frame becomes bands of
// JPEG on a device with 110 KB of contiguous heap is somebody else's problem,
// and one that is already solved.
#pragma once
#include <cstdint>

#include "../core/alert.h"
#include "../core/comfort.h"
#include "../core/places.h"
#include "../core/ratelimit.h"
#include "../core/types.h"
#include "../google/calendar.h"
#include "../oled/health.h"
#include "../render/screen.h"

namespace app {

// Three clocks, because three things need different ones and pretending
// otherwise is how timezone bugs start.
//
//   local  -- minutes, local time. Everything the user sees, and all the
//             journey arithmetic. See core/timeutil.h for why local and not UTC.
//   sec    -- epoch seconds, for the request budgets. RequestBudget copes with
//             this jumping when NTP first lands.
//   utcSec -- real UTC epoch seconds, needed ONLY for the Google JWT's iat/exp,
//             because Google checks them against Google's clock.
struct Clock {
    core::Timestamp local;
    uint32_t sec = 0;
    int64_t  utcSec = 0;
    // Minutes east of UTC right now, from tz_offset.h. Google's Calendar API
    // wants it in timeMin/timeMax and nothing else in the firmware does, so it
    // travels with the clock rather than being recomputed where it is needed.
    int16_t  utcOffsetMin = 0;
    bool     synced = false;   // false until NTP has answered even once
};

enum class Phase : uint8_t {
    NoClock,     // before NTP. Do nothing: every decision here needs the time
    Idle,        // nothing actionable on the calendar
    Watching,    // an event is known, the alert is not due yet
    Alerting,    // holding the SmallTV's screen
    Standdown,   // trying to give it back, and not succeeding yet
};
const char* phaseName(Phase p);

// What went wrong, most recently and still relevant. One at a time on purpose:
// the health panel has one error line, and a queue of stale complaints is worse
// than the newest true one.
enum class Fault : uint8_t {
    None, Clock, Calendar, Place, Transit, Cycle, Weather, Screen,
};
const char* faultName(Fault f);

// What a source call did. `refused` separates "the upstream said no" -- a key,
// a quota, a denial -- from "we could not reach it", because the first deserves
// a long backoff and the second deserves a retry.
struct Outcome {
    bool ok = false;
    bool refused = false;
    // The far end answered, and the answer was "there is nothing there". That
    // is a FACT, not a fault: retrying the same question gets the same nothing.
    // What it justifies is asking a DIFFERENT question -- which is what
    // ensurePlace() does with a Google Maps business name it cannot resolve.
    // Kept apart from `refused`, which means the far end said no to US.
    bool notFound = false;
    char note[24] = {0};

    static Outcome good();
    static Outcome fail(const char* note, bool refused = false);
    // ok = false, notFound = true. Still a failure to the caller that does not
    // care why; distinguishable to the one that does.
    static Outcome missing(const char* note);
};

struct TripRequest {
    double fromLat = 0.0, fromLon = 0.0;
    double toLat = 0.0, toLon = 0.0;
    core::Timestamp arriveBy;
};

// What the television is doing, gathered by a ping rather than guessed at.
struct ScreenState {
    bool reachable = false;
    bool freeKnown = false;
    uint32_t freeKb = 0;
    bool imageLeftBehind = false;   // our file is still there and should not be
};

// The seam. Implemented for real by LiveSources (below), and by a fake in the
// tests. Every method may fail; none of them may block for long.
struct Sources {
    virtual ~Sources() = default;

    virtual Outcome events(const Clock& now, int32_t lookaheadMin,
                           google::EventList& out) = 0;

    // Free text from a calendar entry -> a coordinate. Only called when the
    // PlaceBook does not already know the place.
    virtual Outcome resolve(const char* text, double& lat, double& lon) = 0;

    virtual Outcome trip(const TripRequest& q, core::Itinerary* out, int cap,
                         int& count) = 0;

    virtual Outcome cycle(const TripRequest& q, core::CycleRoute& out) = 0;

    virtual Outcome weather(double lat, double lon, core::Timestamp at,
                            core::Weather& out) = 0;

    // The television. showScreen is "put this on it"; releaseScreen is "give it
    // back and delete the file". Both are expected to be idempotent.
    virtual Outcome pingScreen(ScreenState& out) = 0;
    virtual Outcome showScreen(const render::ScreenModel& m) = 0;
    virtual Outcome releaseScreen() = 0;
};

struct Config {
    core::AlertConfig alert;
    core::ComfortConfig comfort;

    double homeLat = 0.0, homeLon = 0.0;

    // How far ahead to ask the calendar. Twelve hours means a morning poll
    // already knows about an evening appointment.
    int32_t lookaheadMin = 720;

    // Cadences, in seconds. The transit ones are the ones that matter: AVV is
    // asked once every fifteen minutes while merely watching, and once a minute
    // once the alert is up and a delay would actually change what you do.
    uint32_t calendarPollSec = 300;
    uint32_t tripIdleRefreshSec = 900;
    uint32_t tripAlertRefreshSec = 60;
    uint32_t weatherRefreshSec = 3600;
    uint32_t pingSec = 300;
    // ⚠️ AND MUCH FASTER WHILE IT IS NOT ANSWERING. The television joins the
    // access point some seconds after we start it, and the first ping always
    // lands before that. At a flat 300 s cadence the panel then said "no reply"
    // for five minutes after the device was demonstrably there and associated.
    // Poll faster when something is wrong: that is when the answer changes.
    uint32_t pingWhenDownSec = 20;

    // Realtime data older than this is marked STALE rather than shown as if it
    // were current. Never fetched is a different thing again, and reads
    // differently -- see fillHealth().
    int32_t staleAfterMin = 10;

    bool cycleEnabled = true;
    // Quiet hours are NOT handled here: core::evaluate() already returns Idle
    // during them, while still computing the countdown, so the television stays
    // dark and the OLED keeps telling the truth. Set alert.quietStartMin /
    // quietEndMin and it happens. Duplicating that check in this file would
    // give it two places to disagree with itself.

    core::RateLimitConfig transit;    // AVV -- contractual, see clause 6.a
    core::RateLimitConfig calendar;
    core::RateLimitConfig cycling;    // openrouteservice free tier: 2,000/day
    core::RateLimitConfig forecast;
};

class Orchestrator {
public:
    Orchestrator(const Config& cfg, Sources& src, core::PlaceBook& places);

    // Call often; it decides for itself what is due. Cheap when nothing is.
    void tick(const Clock& now);

    // Give the screen back and forget the event. For a deliberate shutdown --
    // a reboot, a config change, an OTA -- where leaving a file on a 1 MB
    // partition would outlive the reason for it.
    void shutdown();

    Phase phase() const { return phase_; }

    // The last thing that decided the phase, in words. `fault()` says what went
    // WRONG; this says what HAPPENED, which is a different question and the one
    // that was missing when the board went from `watching` to `idle` with no
    // explanation anyone could read. Always a string literal, never formatted.
    const char* note() const { return note_; }
    Fault fault() const { return fault_; }
    const char* faultText() const { return faultText_; }
    // Two different questions, deliberately. mayHoldScreen() is "might our
    // file be on the device" and drives the release path; screenConfirmed() is
    // "is our frame actually up" and drives the ON AIR badge. Conflating them
    // means either a badge that lies or a file left behind, and only one of
    // those is recoverable.
    bool mayHoldScreen() const { return mayHoldScreen_; }
    bool screenConfirmed() const { return screenConfirmed_; }
    bool haveEvent() const { return haveEvent_; }
    const core::CalendarEvent& event() const { return event_; }
    const core::AlertStatus& alert() const { return alertStatus_; }

    // The two models. fillScreen is only meaningful while Alerting; fillHealth
    // is always meaningful, which is the entire point of it.
    void fillScreen(render::ScreenModel& m) const;

    // Fills everything the orchestrator knows. It does NOT touch the wifi rows
    // -- the radio is the firmware's business, not this class's -- so the
    // caller fills wifiUp/wifiName/rssiDbm afterwards.
    void fillHealth(oled::HealthModel& m) const;

    // Counters, for the tests and for anyone wondering what it has been doing.
    uint32_t calendarPolls() const { return calendarPolls_; }
    uint32_t tripPolls() const { return tripPolls_; }
    uint32_t screenPushes() const { return screenPushes_; }
    uint32_t screenReleases() const { return screenReleases_; }
    uint32_t budgetRefusals() const { return budgetRefusals_; }
    const core::RequestBudget& transitBudget() const { return transitBudget_; }

private:
    void setFault(Fault f, const char* text);
    void clearFault(Fault f);

    bool due(uint32_t nowSec, uint32_t lastSec, uint32_t everySec, bool ever) const;

    void refreshCalendar(const Clock& now);
    bool ensurePlace(const Clock& now);
    void refreshTrip(const Clock& now);
    void refreshCycle(const Clock& now);
    void refreshWeather(const Clock& now);
    void refreshScreenState(const Clock& now);
    void driveScreen(const Clock& now);
    void standDown(const Clock& now);
    void forgetEvent();
    void adopt(const core::CalendarEvent& e);
    bool alreadyHandled(const core::CalendarEvent& e) const;
    bool stale() const;

    const core::Itinerary* chosen() const;

    Config cfg_;
    Sources* src_;
    core::PlaceBook* places_;

    Phase phase_ = Phase::NoClock;
    Fault fault_ = Fault::None;
    char faultText_[24] = {0};
    const char* note_ = "starting";

    // The last tick's clock, so fillScreen()/fillHealth() can be const and
    // still say what time it is.
    core::Timestamp lastLocal_;
    uint32_t lastSec_ = 0;
    bool clockSynced_ = false;

    // --- the appointment ----------------------------------------------------
    core::CalendarEvent event_;
    bool haveEvent_ = false;

    // The location text we resolved, so that a different event with a different
    // location re-resolves rather than inheriting the last one's coordinates.
    char destKey_[sizeof(core::CalendarEvent::location)] = {0};
    // What to CALL the place on screen. The PlaceBook's label is the user's own
    // word for it ("Hauptgebaeude"), which beats whatever Google Maps calls it.
    char destLabel_[core::kNameLen] = {0};
    // Which spelling of the location to ask AVV about next: 0 the street line,
    // 1 the whole simplified string. Flipped only when AVV answers "nothing
    // there", so a Google Maps business name gets a second, better question and
    // an unreachable AVV does not get two bad ones.
    uint8_t placeSpelling_ = 0;
    double destLat_ = 0.0, destLon_ = 0.0;
    bool destResolved_ = false;

    // The appointment we have already seen through to the end. Without this the
    // alert re-arms: Passed means the DEPARTURE is behind us, and the event
    // itself is still comfortably in the future, so the next calendar poll
    // hands back the same entry and the whole thing starts again.
    core::Timestamp handledStart_;
    char handledLocation_[sizeof(core::CalendarEvent::location)] = {0};

    // --- what we know about getting there -----------------------------------
    core::Itinerary candidates_[core::kMaxCandidates];
    // A separate landing area, so that a Sources implementation which scribbles
    // on `out` and THEN fails cannot destroy the itineraries we already have.
    // 5,808 bytes, and it lived on the stack until the board told us otherwise.
    core::Itinerary scratch_[core::kMaxCandidates];
    int candCount_ = 0;
    int chosenIdx_ = -1;             // an index, not a pointer: it outlives copies
    bool willBeLate_ = false;
    int32_t lateByMin_ = 0;
    int32_t transfers_ = 0;
    bool anyCandidates_ = false;
    char noTransitReason_[40] = {0};
    core::Timestamp transitAt_;      // when the realtime data we hold was fetched

    core::CycleRoute cycle_;
    char cycleKey_[sizeof(core::CalendarEvent::location)] = {0};
    core::Weather weather_;
    core::ComfortResult comfort_;

    core::AlertStatus alertStatus_;

    // --- the television -----------------------------------------------------
    ScreenState screen_;
    bool mayHoldScreen_ = false;      // conservative: set BEFORE the upload
    bool screenConfirmed_ = false;    // only after the device said yes
    int32_t lastShownMinute_ = INT32_MIN;

    // --- when we last asked each upstream anything --------------------------
    uint32_t lastCalendarSec_ = 0;  bool everCalendar_ = false;
    uint32_t lastTripSec_ = 0;      bool everTrip_ = false;
    uint32_t lastCycleSec_ = 0;     bool everCycle_ = false;
    uint32_t lastWeatherSec_ = 0;   bool everWeather_ = false;
    uint32_t lastPingSec_ = 0;      bool everPing_ = false;

    core::RequestBudget calendarBudget_;
    core::RequestBudget transitBudget_;
    core::RequestBudget cycleBudget_;
    core::RequestBudget weatherBudget_;

    uint32_t calendarPolls_ = 0;
    uint32_t tripPolls_ = 0;
    uint32_t screenPushes_ = 0;
    uint32_t screenReleases_ = 0;
    uint32_t budgetRefusals_ = 0;
};

} // namespace app
