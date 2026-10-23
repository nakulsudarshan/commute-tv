#include "trip_select.h"

namespace core {

TripChoice selectTrip(const Itinerary* candidates, int count, Timestamp eventStart,
                      const TripPrefs& prefs) {
    TripChoice out;
    if (!candidates || count <= 0 || !eventStart.valid()) return out;

    const Itinerary* bestOnTime = nullptr;
    int32_t bestScore = 0;                   // higher is better; see below
    const Itinerary* bestLate = nullptr;

    for (int i = 0; i < count; ++i) {
        const Itinerary& c = candidates[i];
        if (!c.usable()) continue;           // cancelled / unreachable / malformed
        out.anyCandidates = true;

        if (c.arrival() <= eventStart) {
            // Leave as late as you can -- but count each change as
            // transferPenaltyMin of departure time you would have given up to
            // avoid it. A direct bus at 12:50 scores 12:50; a two-change at
            // 13:05 scores 12:35, and loses.
            const int32_t score =
                c.departure().min - prefs.transferPenaltyMin * c.transfers;
            if (!bestOnTime || score > bestScore) {
                bestOnTime = &c;
                bestScore = score;
            }
        } else if (!bestLate || c.arrival() < bestLate->arrival() ||
                   (c.arrival() == bestLate->arrival() &&
                    c.transfers < bestLate->transfers)) {
            // Already going to be late: get there soonest, and among equally
            // late options take the one with fewer things to go wrong.
            bestLate = &c;
        }
    }

    if (bestOnTime) {
        out.trip = bestOnTime;
        out.transfers = bestOnTime->transfers;
        return out;
    }

    if (bestLate) {
        out.trip = bestLate;
        out.transfers = bestLate->transfers;
        out.willBeLate = true;
        out.lateByMin = diffMinutes(bestLate->arrival(), eventStart);
    }
    return out;
}

} // namespace core
