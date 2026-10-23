// health.h - what the little screen says when the big one is dark.
//
// The SmallTV shows a commute for about thirty-five minutes a day. The other
// twenty-three hours it shows a clock, and the honest question is "is this
// thing working, or has it quietly stopped?" This panel answers that, and it is
// the only place the firmware ever admits to a problem.
//
// So the rule for every line here: **say what is true, including when what is
// true is that we do not know.** A health panel that shows a plausible-looking
// number it made up is worse than a blank one -- it is the failure mode that
// makes you stop checking.
#pragma once
#include <cstdint>

#include "mono_canvas.h"

namespace oled {

// Every string here is a FIXED-SIZE field, and whatever fills it must copy
// with a bound -- core::copyStr or snprintf, never strcpy. A stop name or an
// error text that does not fit is truncated on purpose; the panel clips it
// again at draw time and marks it with "..", so a too-long value degrades to
// something honest rather than corrupting the struct behind it.
struct HealthModel {
    // --- clock -------------------------------------------------------------
    char clock[8] = "--:--";
    bool clockSynced = false;      // false until NTP has answered even once

    // --- network -----------------------------------------------------------
    bool wifiUp = false;
    char wifiName[20] = "";
    int16_t rssiDbm = 0;           // 0 means unknown, not "perfect signal"

    // --- the appointment we are acting on ----------------------------------
    bool haveEvent = false;
    char eventTime[8] = "--:--";
    char eventPlace[24] = "";

    // --- state -------------------------------------------------------------
    const char* stateText = "idle";
    int32_t minutesToLeave = 0;
    bool alerting = false;         // are we holding the SmallTV's screen?

    // --- upstreams ---------------------------------------------------------
    bool tvReachable = false;
    bool tvImageLeftBehind = false;
    bool tvFreeKnown = false;
    uint32_t tvFreeKb = 0;

    // Age of the newest realtime data AVV gave us, in minutes. Negative means
    // we have never had any -- which is a different thing from "it is fresh",
    // and the panel must not let them look alike.
    int32_t transitAgeMin = -1;
    bool transitStale = false;

    // --- the request budget ------------------------------------------------
    uint16_t requestsThisHour = 0;
    uint16_t requestsPerHourCap = 0;
    uint32_t backoffSec = 0;

    // --- the one thing that went wrong -------------------------------------
    char errorText[24] = "";
};

// Layout, exposed so the tests can assert against the real geometry rather
// than numbers copied out of the .cpp and left to rot.
constexpr int16_t kHeaderH = 9;
constexpr int16_t kRowPitch = 9;
constexpr int16_t kFirstBaseline = 17;
constexpr int16_t kPadX = 2;
constexpr int16_t kLabelW = 34;
constexpr int kMaxRows = 6;

// Baseline of row `i`, and the x/width of its right-aligned value column.
inline int16_t rowY(int i) { return static_cast<int16_t>(kFirstBaseline + i * kRowPitch); }
inline int16_t valueX() { return static_cast<int16_t>(kPadX + kLabelW); }
inline int16_t valueW() { return static_cast<int16_t>(kWidth - valueX() - kPadX); }

// Draws the whole panel. Clears first: this is the only thing on the display.
void renderHealth(MonoCanvas& c, const HealthModel& m);

// A one-line boot/splash screen, for the seconds before there is anything to
// report.
void renderBoot(MonoCanvas& c, const char* line);

} // namespace oled
