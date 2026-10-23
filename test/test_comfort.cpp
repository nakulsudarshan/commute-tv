#include "test.h"
#include "../src/core/comfort.h"
using namespace core;

static CycleRoute flatRoute(float km = 5.0f, float ascent = 10.0f) {
    CycleRoute r; r.valid = true; r.distanceKm = km; r.ascentM = ascent; r.durationMin = 20;
    return r;
}
static Weather wx(float t, float rain = 0, float wind = 0) {
    Weather w; w.valid = true; w.tempC = t; w.precipProbPct = rain; w.windKph = wind;
    return w;
}

void test_comfort() {
    t::suite("comfort / temperature");
    ComfortConfig c;

    // The specified boundaries must hold EXACTLY. Ramps sit outside the bands
    // precisely so these values are not softened.
    CHECK_NEAR(temperatureScore(10.0f, c), 0.5f, 1e-4);   // start of moderate
    CHECK_NEAR(temperatureScore(15.0f, c), 1.0f, 1e-4);   // start of ideal
    CHECK_NEAR(temperatureScore(28.0f, c), 1.0f, 1e-4);   // still ideal
    CHECK_NEAR(temperatureScore(20.0f, c), 1.0f, 1e-4);
    CHECK_NEAR(temperatureScore(12.0f, c), 0.5f, 1e-4);

    // Extremes.
    CHECK_NEAR(temperatureScore(9.0f, c),  0.0f, 1e-4);
    CHECK_NEAR(temperatureScore(-5.0f, c), 0.0f, 1e-4);
    CHECK_NEAR(temperatureScore(29.0f, c), 0.0f, 1e-4);
    CHECK_NEAR(temperatureScore(35.0f, c), 0.0f, 1e-4);

    // Ramps are monotonic and land mid-way where expected.
    CHECK_NEAR(temperatureScore(9.5f, c),  0.25f, 1e-4);
    CHECK_NEAR(temperatureScore(28.5f, c), 0.5f,  1e-4);
    CHECK(temperatureScore(14.5f, c) > 0.5f);
    CHECK(temperatureScore(14.5f, c) < 1.0f);

    t::suite("comfort / effort");
    CHECK_NEAR(effortScore(10.0f, 10.0f, c), 1.0f, 1e-4);    // 1 m/km, flat
    CHECK_NEAR(effortScore(50.0f, 10.0f, c), 1.0f, 1e-4);    // exactly 5 m/km
    CHECK_NEAR(effortScore(150.0f, 10.0f, c), 0.6f, 1e-4);   // 15 m/km
    CHECK_NEAR(effortScore(300.0f, 10.0f, c), 0.3f, 1e-4);   // 30 m/km
    CHECK_NEAR(effortScore(450.0f, 10.0f, c), 0.1f, 1e-4);   // 45 m/km
    CHECK_NEAR(effortScore(900.0f, 10.0f, c), 0.1f, 1e-4);   // clamped
    // Zero distance must not produce NaN.
    CHECK_NEAR(effortScore(50.0f, 0.0f, c), 1.0f, 1e-4);
    CHECK_NEAR(effortScore(-5.0f, 10.0f, c), 1.0f, 1e-4);    // negative ascent clamped

    t::suite("comfort / weather penalty");
    CHECK_NEAR(weatherPenalty(0, 0, c), 1.0f, 1e-4);
    CHECK_NEAR(weatherPenalty(50, 0, c), 0.65f, 1e-4);
    CHECK_NEAR(weatherPenalty(80, 0, c), 0.35f, 1e-4);
    CHECK_NEAR(weatherPenalty(0, 25, c), 0.9f, 1e-4);
    CHECK_NEAR(weatherPenalty(0, 40, c), 0.75f, 1e-4);
    CHECK_NEAR(weatherPenalty(80, 40, c), 0.35f * 0.75f, 1e-4);  // compounds
    CHECK_NEAR(weatherPenalty(40, 20, c), 1.0f, 1e-4);           // boundaries exclusive

    t::suite("comfort / combined");
    // Perfect day, flat route.
    {
        ComfortResult r = computeComfort(wx(20), flatRoute(5, 10), c);
        CHECK_EQ(r.score, 100);
        CHECK_STR(r.label(), "Ideal");
    }
    // Heavy rain must cap at Rough even on an otherwise perfect day.
    {
        ComfortResult r = computeComfort(wx(20, 90), flatRoute(5, 10), c);
        CHECK_EQ(r.score, 35);
        CHECK_STR(r.label(), "Rough");
        CHECK_STR(r.reasonText(), "rain");
    }
    // A hard climb on a good day.
    {
        ComfortResult r = computeComfort(wx(20), flatRoute(4, 200), c);  // 50 m/km
        CHECK_NEAR(r.e, 0.1f, 1e-4);
        CHECK_EQ(r.score, 55);
        CHECK_STR(r.reasonText(), "climb");
    }
    // Cold, flat: temperature is the dominant complaint.
    {
        ComfortResult r = computeComfort(wx(2), flatRoute(5, 10), c);
        CHECK_EQ(r.score, 50);
        CHECK_STR(r.reasonText(), "cold");
    }
    // Hot day names heat, not cold.
    {
        ComfortResult r = computeComfort(wx(33), flatRoute(5, 10), c);
        CHECK_STR(r.reasonText(), "heat");
    }
    // No route => no score at all, rather than a misleading zero.
    {
        CycleRoute none;
        ComfortResult r = computeComfort(wx(20), none, c);
        CHECK_EQ(r.score, 0);
        CHECK(r.reason == ComfortReason::None);
    }
    // Missing weather must not be treated as perfect weather silently; it
    // assumes a neutral 20 C and still produces a usable score.
    {
        Weather unknown;
        ComfortResult r = computeComfort(unknown, flatRoute(5, 10), c);
        CHECK_EQ(r.score, 100);
    }
}
