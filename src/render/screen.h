// screen.h - the 240x240 commute layout.
//
// Everything the screen shows is in ScreenModel. The renderer does no fetching,
// no time arithmetic and no decisions, so it can be exercised on the host with
// made-up data -- which is how the layout gets reviewed before any hardware
// exists.
#pragma once
#include <cstdint>
#include "canvas.h"

namespace render {

constexpr int16_t kScreenW = 240;
constexpr int16_t kScreenH = 240;

// The footer band, exposed so the attribution test can assert against the real
// geometry rather than against numbers copied out of the .cpp and left to rot.
constexpr int16_t kFooterTop = 218;
constexpr int16_t kFooterBottom = kScreenH;

// The AVV usage agreement, clause 3.b, requires the AVV mark to be shown in
// legible size wherever query results are displayed. renderCommute always draws
// it at the right-hand end of the footer; renderIdle does not, because the idle
// screen shows no query results.
extern const char* const kAttributionText;

struct ScreenModel {
    // --- countdown ---------------------------------------------------------
    int32_t minutesToLeave = 0;     // negative once leave time has passed
    char nowText[8] = "--:--";

    // --- appointment -------------------------------------------------------
    char destination[48] = "";
    // What the appointment is CALLED. The destination says where; this says
    // what, which is the half you actually recognise at a glance.
    char eventTitle[40] = "";
    char eventTime[8] = "--:--";

    // --- transit -----------------------------------------------------------
    bool hasTransit = false;
    char line[12] = "";
    char boardStop[48] = "";
    char depTime[8] = "--:--";
    char arrTime[8] = "--:--";
    int32_t delayMin = 0;
    int32_t transfers = 0;        // changes on the chosen journey; 0 is direct
    bool cancelled = false;
    bool willBeLate = false;
    int32_t lateByMin = 0;
    char noTransitReason[40] = "";   // shown when hasTransit is false

    // --- cycling -----------------------------------------------------------
    bool hasCycle = false;
    int32_t cycleMin = 0;
    int32_t ascentM = 0;
    int32_t comfortScore = 0;
    const char* comfortLabel = "";
    const char* comfortReason = "";

    // --- footer ------------------------------------------------------------
    bool stale = false;
    char statusText[32] = "";
};

// The alert screen: what the SmallTV shows near a departure.
void renderCommute(Canvas& c, const ScreenModel& m);

// Shown when there is nothing to act on but we still hold the screen briefly
// (e.g. immediately after an alert clears).
void renderIdle(Canvas& c, const char* nowText, const char* note);

} // namespace render
