// trip_select.h - choose which HAFAS itinerary to act on.
//
// HAFAS is asked with searchForArrival=1, so it returns candidates that aim to
// arrive by the event start. We still have to choose, because:
//   - realtime delay may push a candidate past the deadline after the fact
//   - a candidate may be cancelled or have an interchange that no longer works
//   - among those that still work, the LATEST departure is the kind one
#pragma once
#include "types.h"

namespace core {

// How much earlier it is worth leaving to avoid one change.
//
// MEASURED AGAINST REALITY, 25 Aug 2026: for a 13:30 appointment AVV offered six
// itineraries and the old rule -- "the latest departure that still arrives in
// time" -- picked one with TWO changes, while a direct 3B was available. That is
// not a preference, it is the wrong answer: two changes is two chances to miss a
// connection, in the rain, with luggage.
//
// But "fewest changes, always" is equally wrong, because it would have you leave
// an hour early to avoid a single easy interchange. So each change is worth this
// many minutes of lie-in, and the arithmetic decides.
struct TripPrefs {
    int32_t transferPenaltyMin = 15;
};

struct TripChoice {
    const Itinerary* trip = nullptr;
    int32_t transfers = 0;        // changes on the chosen journey
    bool willBeLate = false;      // best available still arrives after the event
    int32_t lateByMin = 0;        // how late, when willBeLate
    bool anyCandidates = false;   // false => HAFAS returned nothing usable at all
};

// Picks the latest usable itinerary that still arrives by eventStart.
// If none does, returns the one arriving earliest and flags willBeLate, so the
// display can say "you will be ~7 min late" rather than showing nothing.
TripChoice selectTrip(const Itinerary* candidates, int count, Timestamp eventStart,
                      const TripPrefs& prefs = TripPrefs{});

} // namespace core
