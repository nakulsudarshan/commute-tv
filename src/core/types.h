// types.h - plain data shared by the pure core.
// No Arduino, no JSON library, no dynamic allocation.
#pragma once
#include <cstdint>
#include "timeutil.h"

namespace core {

constexpr int kMaxLegs = 6;       // AVV itineraries in practice never exceed this
constexpr int kMaxCandidates = 6; // HAFAS numF caps at 6
constexpr int kNameLen = 40;

enum class LegMode : uint8_t { Walk, Bus, Tram, Train, Other };

struct Leg {
    LegMode mode = LegMode::Other;
    char line[16] = {0};        // "3B", "RE1"
    char fromName[kNameLen] = {0};
    char toName[kNameLen] = {0};
    char direction[kNameLen] = {0};

    Timestamp plannedDep, plannedArr;
    Timestamp rtDep, rtArr;     // invalid when no realtime data exists

    bool cancelled = false;

    // Effective times fall back to planned when realtime is absent. Every
    // consumer must use these rather than picking a field themselves --
    // that inconsistency is exactly how "we showed the scheduled time during
    // a delay" bugs happen.
    Timestamp depTime() const { return rtDep.valid() ? rtDep : plannedDep; }
    Timestamp arrTime() const { return rtArr.valid() ? rtArr : plannedArr; }

    // Positive = late, negative = early, 0 = on time or unknown.
    int32_t depDelayMin() const {
        return (rtDep.valid() && plannedDep.valid()) ? diffMinutes(rtDep, plannedDep) : 0;
    }
};

struct Itinerary {
    Leg legs[kMaxLegs];
    uint8_t legCount = 0;

    bool cancelled = false;
    // HAFAS `reachable` is false when current delays break an interchange.
    // Defaults to true: absence of the field means "no reason to doubt it".
    bool reachable = true;

    int32_t transfers = 0;

    Timestamp departure() const { return legCount ? legs[0].depTime() : invalidTs(); }
    Timestamp arrival()   const { return legCount ? legs[legCount - 1].arrTime() : invalidTs(); }

    // Delay on the leg you actually have to catch.
    int32_t depDelayMin() const { return legCount ? legs[0].depDelayMin() : 0; }

    // First non-walking leg: what the display should name.
    const Leg* firstTransitLeg() const {
        for (uint8_t i = 0; i < legCount; ++i)
            if (legs[i].mode != LegMode::Walk) return &legs[i];
        return nullptr;
    }

    bool usable() const { return legCount > 0 && !cancelled && reachable
                                 && departure().valid() && arrival().valid(); }
};

struct Weather {
    bool valid = false;
    float tempC = 0.0f;
    float precipProbPct = 0.0f;
    float windKph = 0.0f;
};

struct CycleRoute {
    bool valid = false;
    int32_t durationMin = 0;
    float distanceKm = 0.0f;
    float ascentM = 0.0f;
    float descentM = 0.0f;
};

struct CalendarEvent {
    char title[kNameLen] = {0};
    char location[80] = {0};   // empty => the event is ignored entirely
    Timestamp start;
    bool allDay = false;

    bool actionable() const { return !allDay && location[0] != '\0' && start.valid(); }
};

} // namespace core
