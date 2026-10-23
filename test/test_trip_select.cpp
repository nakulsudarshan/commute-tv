#include "test.h"
#include "../src/core/trip_select.h"
using namespace core;

static Itinerary mk(const char* dep, const char* arr, const char* rtDep = nullptr,
                    bool cancelled = false, bool reachable = true,
                    int32_t transfers = 0) {
    Itinerary it;
    it.transfers = transfers;
    it.legCount = 1;
    it.legs[0].mode = LegMode::Bus;
    it.legs[0].plannedDep = parseDateTime("2026-08-13", dep);
    it.legs[0].plannedArr = parseDateTime("2026-08-13", arr);
    if (rtDep) it.legs[0].rtDep = parseDateTime("2026-08-13", rtDep);
    it.cancelled = cancelled;
    it.reachable = reachable;
    return it;
}

void test_trip_select() {
    t::suite("trip selection");
    const Timestamp event = parseDateTime("2026-08-13", "15:30");

    // Among trips that arrive in time, the latest departure wins -- there is no
    // virtue in leaving early.
    {
        Itinerary c[3] = { mk("13:50", "14:20"), mk("14:50", "15:20"), mk("14:20", "14:50") };
        TripChoice r = selectTrip(c, 3, event);
        CHECK(r.trip != nullptr);
        CHECK(!r.willBeLate);
        CHECK_EQ(r.trip->departure().min, parseDateTime("2026-08-13", "14:50").min);
    }

    // Cancelled and unreachable candidates are skipped even when otherwise ideal.
    {
        Itinerary c[3] = {
            mk("14:50", "15:20", nullptr, true,  true),   // cancelled
            mk("14:45", "15:15", nullptr, false, false),  // interchange broken
            mk("14:20", "14:55"),
        };
        TripChoice r = selectTrip(c, 3, event);
        CHECK(r.trip != nullptr);
        CHECK_EQ(r.trip->departure().min, parseDateTime("2026-08-13", "14:20").min);
    }

    // Realtime delay is what counts: a trip scheduled to arrive in time but
    // running late must not be preferred over one that actually makes it.
    {
        Itinerary late = mk("14:50", "15:20", "15:10");   // 20 min late
        late.legs[0].rtArr = parseDateTime("2026-08-13", "15:40");
        Itinerary fine = mk("14:20", "14:55");
        Itinerary c[2] = { late, fine };
        TripChoice r = selectTrip(c, 2, event);
        CHECK(!r.willBeLate);
        CHECK_EQ(r.trip->departure().min, parseDateTime("2026-08-13", "14:20").min);
    }

    // When nothing arrives in time we still return the least-bad option and
    // say how late it is, rather than showing nothing at all.
    {
        Itinerary c[2] = { mk("15:20", "15:50"), mk("15:40", "16:10") };
        TripChoice r = selectTrip(c, 2, event);
        CHECK(r.trip != nullptr);
        CHECK(r.willBeLate);
        CHECK_EQ(r.lateByMin, 20);
        CHECK(r.anyCandidates);
    }

    // Everything cancelled: no trip, and anyCandidates stays false so the
    // caller can distinguish "all bad" from "none returned".
    {
        Itinerary c[2] = { mk("14:50", "15:20", nullptr, true), mk("14:20", "14:55", nullptr, true) };
        TripChoice r = selectTrip(c, 2, event);
        CHECK(r.trip == nullptr);
        CHECK(!r.anyCandidates);
    }

    // Degenerate inputs.
    {
        CHECK(selectTrip(nullptr, 0, event).trip == nullptr);
        Itinerary c[1] = { mk("14:20", "14:55") };
        CHECK(selectTrip(c, 1, invalidTs()).trip == nullptr);
    }

    t::suite("itinerary helpers");
    {
        Itinerary it;
        it.legCount = 3;
        it.legs[0].mode = LegMode::Walk;
        it.legs[1].mode = LegMode::Bus;
        std::strcpy(it.legs[1].line, "3B");
        it.legs[2].mode = LegMode::Walk;
        CHECK(it.firstTransitLeg() != nullptr);
        CHECK_STR(it.firstTransitLeg()->line, "3B");

        // Delay reporting falls back to 0 when no realtime data exists, rather
        // than inventing a negative delay from an invalid timestamp.
        Itinerary noRt = mk("14:20", "14:55");
        CHECK_EQ(noRt.depDelayMin(), 0);
        Itinerary rt = mk("14:20", "14:55", "14:26");
        CHECK_EQ(rt.depDelayMin(), 6);
    }

    t::suite("trip selection / changes cost something");
    {
        // ⚠️ THE ONE FROM REAL LIFE, 25 Aug 2026. A 13:30 appointment at
        // Mariabrunnstrasse; AVV offered six itineraries. The old rule -- latest
        // departure that still arrives in time -- chose a TWO-CHANGE journey,
        // while a direct 3B arriving 13:27 was sitting in the same list. Nakul
        // spotted it in the AVV app; the firmware would have sent him the long
        // way round.
        const Timestamp meeting = parseDateTime("2026-08-13", "13:30");
        Itinerary c[2] = {
            mk("12:50", "13:27", nullptr, false, true, 0),   // direct 3B
            mk("13:05", "13:28", nullptr, false, true, 2),   // two changes, later
        };
        TripChoice r = selectTrip(c, 2, meeting);
        CHECK(r.trip != nullptr);
        CHECK_EQ(r.transfers, 0);
        CHECK_EQ(r.trip->departure().min, parseDateTime("2026-08-13", "12:50").min);
    }
    {
        // But "fewest changes, always" would be just as wrong. One change is
        // worth 15 minutes of lie-in, so a single-change journey leaving 40
        // minutes later than the direct one SHOULD win.
        const Timestamp meeting = parseDateTime("2026-08-13", "13:30");
        Itinerary c[2] = {
            mk("12:20", "13:00", nullptr, false, true, 0),
            mk("13:00", "13:25", nullptr, false, true, 1),
        };
        TripChoice r = selectTrip(c, 2, meeting);
        CHECK_EQ(r.transfers, 1);
        CHECK_EQ(r.trip->departure().min, parseDateTime("2026-08-13", "13:00").min);
    }
    {
        // Exactly at the tipping point the extra change does NOT pay for itself:
        // 15 minutes later for one more change is a wash, and a wash should not
        // buy you an interchange to stand around in.
        const Timestamp meeting = parseDateTime("2026-08-13", "13:30");
        Itinerary c[2] = {
            mk("12:45", "13:20", nullptr, false, true, 0),
            mk("13:00", "13:25", nullptr, false, true, 1),
        };
        TripChoice r = selectTrip(c, 2, meeting);
        CHECK_EQ(r.transfers, 0);
    }
    {
        // The penalty is configurable, and setting it to zero restores exactly
        // the old behaviour -- which is the honest way to describe a change of
        // policy rather than pretending the old one never existed.
        const Timestamp meeting = parseDateTime("2026-08-13", "13:30");
        Itinerary c[2] = {
            mk("12:50", "13:27", nullptr, false, true, 0),
            mk("13:05", "13:28", nullptr, false, true, 2),
        };
        TripPrefs none;
        none.transferPenaltyMin = 0;
        TripChoice r = selectTrip(c, 2, meeting, none);
        CHECK_EQ(r.transfers, 2);
        CHECK_EQ(r.trip->departure().min, parseDateTime("2026-08-13", "13:05").min);
    }
    {
        // Already going to be late: soonest arrival wins, and among equally late
        // options the one with fewer things to go wrong.
        const Timestamp meeting = parseDateTime("2026-08-13", "13:00");
        Itinerary c[2] = {
            mk("13:00", "13:40", nullptr, false, true, 3),
            mk("13:02", "13:40", nullptr, false, true, 0),
        };
        TripChoice r = selectTrip(c, 2, meeting);
        CHECK(r.willBeLate);
        CHECK_EQ(r.transfers, 0);
        CHECK_EQ(r.lateByMin, 40);
    }
}
