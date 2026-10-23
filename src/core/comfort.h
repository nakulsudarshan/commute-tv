// comfort.h - cycling comfort score.
//
//   score = 100 * (0.5*T + 0.5*E) * W
//
// T temperature band, E climb effort, W weather penalty.
// Every threshold lives in ComfortConfig so it can be tuned from the web UI
// without reflashing the device.
#pragma once
#include <cstdint>
#include "types.h"

namespace core {

enum class ComfortReason : uint8_t {
    None, Cold, Heat, Climb, Rain, Wind
};

struct ComfortConfig {
    // --- Temperature (degrees C) -------------------------------------------
    // Bands as specified: <10 extreme, 10-15 moderate, 15-28 ideal, >28 extreme.
    // Ramps are placed OUTSIDE each band so the stated boundaries keep their
    // exact specified value: at 10.0 the score is 0.5, at 15.0 it is 1.0,
    // at 28.0 it is still 1.0. Only beyond a boundary does it slide.
    float tColdFloor   = 9.0f;    // at or below -> 0.0
    float tModerateLo  = 10.0f;   // 0.5 from here
    float tIdealRampLo = 14.0f;   // starts climbing toward 1.0
    float tIdealLo     = 15.0f;   // 1.0 from here
    float tIdealHi     = 28.0f;   // 1.0 up to and including here
    float tHeatCeil    = 29.0f;   // at or above -> 0.0
    float vModerate    = 0.5f;

    // --- Climb effort (metres of ascent per km) ----------------------------
    float cFlat = 5.0f,  eFlat = 1.0f;
    float cGentle = 15.0f, eGentle = 0.6f;
    float cNoticeable = 30.0f, eNoticeable = 0.3f;
    float cHard = 45.0f, eHard = 0.1f;

    // --- Weather penalty (multiplicative) ----------------------------------
    float rainHiPct = 70.0f, wRainHi = 0.35f;
    float rainMidPct = 40.0f, wRainMid = 0.65f;
    float windHiKph = 30.0f, wWindHi = 0.75f;
    float windMidKph = 20.0f, wWindMid = 0.9f;

    // --- Presentation -------------------------------------------------------
    int scoreIdeal = 75;   // >= -> "Ideal"
    int scoreOk = 45;      // >= -> "OK", below -> "Rough"
};

struct ComfortResult {
    int score = 0;            // 0-100
    float t = 0.0f, e = 0.0f, w = 1.0f;
    ComfortReason reason = ComfortReason::None;
    const char* label() const;      // "Ideal" / "OK" / "Rough"
    const char* reasonText() const; // "rain", "climb", ... or ""
};

float temperatureScore(float tempC, const ComfortConfig& c);
float effortScore(float ascentM, float distanceKm, const ComfortConfig& c);
float weatherPenalty(float precipProbPct, float windKph, const ComfortConfig& c);

ComfortResult computeComfort(const Weather& w, const CycleRoute& r,
                             const ComfortConfig& c = ComfortConfig{});

} // namespace core
