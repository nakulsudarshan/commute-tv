#include "health.h"

#include <cstdio>
#include <cstring>

#include "font_pixel.h"

namespace oled {
namespace {

// FontPixel, not FontTiny. The 9 px TrueType face was legible but mushy on the
// real glass -- at 1 bpp a rasterised outline puts stems between pixels, so
// some letters came out spidery and others blobby. A font drawn ON the grid has
// nothing to round. FontTiny is left in the tree as the fallback if this ever
// looks worse on hardware than it does in the preview.
using render::FontPixel;
const render::Font& kFont = FontPixel;

// Six rows of text under a header. Capitals are 7 px and descenders drop 1 px,
// so a 9 px pitch starting at baseline 17 puts the last baseline on 62 and the
// deepest descender on 62 as well -- inside a 64 px panel, with nothing
// clipped. (FontTiny's descenders reached 3 px below the baseline and the
// bottom row lost its tail; that trade is no longer necessary.)
//
// The header is plain text over a rule rather than white-on-black. An inverted
// bar looks smart in a mockup and is genuinely hard to read at this size on a
// real OLED: knocking a one-pixel stroke out of a filled block leaves very
// little letter behind.
constexpr int16_t kBarH = kHeaderH;
constexpr int16_t kPad = kPadX;

// Label on the left, value starting immediately after the label column.
//
// This used to right-align the value against the panel edge, the idea being a
// single column the eye could run down. On the real screen it just left a hole:
// with values like "no data" and "nothing", two thirds of every row was blank.
// Left-aligned, the rows read as label-then-answer, and a long value gets the
// full 90 px before it has to clip.
void row(MonoCanvas& c, int i, const char* label, const char* value) {
    const int16_t y = rowY(i);
    c.text(kPad, y, label, kFont);
    // textClipped draws in full when it fits and marks with ".." when it does
    // not, so there is no separate case to get wrong.
    c.textClipped(valueX(), y, valueW(), value, kFont);
}

} // namespace

void renderBoot(MonoCanvas& c, const char* line) {
    c.clear();
    const char* title = "commute-tv";
    const int16_t tw = c.textWidth(title, kFont);
    c.text(static_cast<int16_t>((kWidth - tw) / 2), 8, title, kFont);
    c.hline(0, kBarH, kWidth);
    if (line && line[0]) {
        const int16_t w = c.textWidth(line, kFont);
        c.textClipped(static_cast<int16_t>((kWidth - w) / 2 > 0 ? (kWidth - w) / 2 : kPad),
                      36, static_cast<int16_t>(kWidth - 2 * kPad), line, kFont);
    }
}

void renderHealth(MonoCanvas& c, const HealthModel& m) {
    c.clear();

    // --- header: the clock, and whether to believe it ----------------------
    // An unsynced clock is drawn as "--:--" by whoever fills the model, but say
    // it in words too: a wrong time here means every countdown is wrong, and
    // that is not something to leave to a reader noticing the dashes.
    c.text(kPad, 8, m.clockSynced ? m.clock : "no clock", kFont);

    // The alert state is the other thing worth seeing at a glance. When we are
    // actually holding the SmallTV's screen it gets the inverted badge -- one
    // short word, where the loss of stroke is affordable and the contrast earns
    // its place.
    const char* badge = m.alerting ? "ON AIR" : m.stateText;
    if (badge && badge[0]) {
        const int16_t bw = c.textWidth(badge, kFont);
        const int16_t bx = static_cast<int16_t>(kWidth - kPad - bw);
        if (m.alerting) {
            c.fillRect(static_cast<int16_t>(bx - 2), 0,
                       static_cast<int16_t>(bw + 4), kBarH, Ink::On);
            c.text(bx, 8, badge, kFont, Ink::Off);
        } else {
            c.text(bx, 8, badge, kFont);
        }
    }
    c.hline(0, kBarH, kWidth);

    char buf[40];
    int line = 0;

    // --- wifi ---------------------------------------------------------------
    if (!m.wifiUp) {
        row(c, line++, "wifi", "down");
    } else if (m.rssiDbm == 0) {
        // Zero is not a signal reading, it is the absence of one.
        row(c, line++, "wifi", m.wifiName[0] ? m.wifiName : "up");
    } else {
        std::snprintf(buf, sizeof buf, "%s %d", m.wifiName[0] ? m.wifiName : "up",
                      static_cast<int>(m.rssiDbm));
        row(c, line++, "wifi", buf);
    }

    // --- the appointment ----------------------------------------------------
    if (m.haveEvent) {
        std::snprintf(buf, sizeof buf, "%s %s", m.eventTime, m.eventPlace);
        row(c, line++, "next", buf);
    } else {
        row(c, line++, "next", "nothing");
    }

    // --- leave time ---------------------------------------------------------
    if (m.haveEvent) {
        if (m.minutesToLeave > 0) std::snprintf(buf, sizeof buf, "in %ld min",
                                                static_cast<long>(m.minutesToLeave));
        else if (m.minutesToLeave == 0) std::snprintf(buf, sizeof buf, "now");
        else std::snprintf(buf, sizeof buf, "%ld min ago",
                           static_cast<long>(-m.minutesToLeave));
        row(c, line++, "leave", buf);
    }

    // --- how fresh the transit data is --------------------------------------
    if (m.transitAgeMin < 0) {
        // Never fetched is NOT the same as fresh, and must not read the same.
        row(c, line++, "avv", "no data");
    } else {
        std::snprintf(buf, sizeof buf, "%ldm%s", static_cast<long>(m.transitAgeMin),
                      m.transitStale ? " STALE" : "");
        row(c, line++, "avv", buf);
    }

    // --- the television -----------------------------------------------------
    if (!m.tvReachable) {
        row(c, line++, "tv", "no reply");
    } else if (m.tvImageLeftBehind) {
        // The housekeeping failed. On a ~1 MB image partition this is the thing
        // that eventually stops the whole system working, so it outranks the
        // free-space figure.
        row(c, line++, "tv", "file left!");
    } else if (m.tvFreeKnown) {
        std::snprintf(buf, sizeof buf, "ok %luk", static_cast<unsigned long>(m.tvFreeKb));
        row(c, line++, "tv", buf);
    } else {
        row(c, line++, "tv", "ok");
    }

    // --- budget, or the error, if there is one ------------------------------
    if (m.errorText[0] && line < kMaxRows) {
        row(c, line++, "err", m.errorText);
    } else if (line < kMaxRows) {
        if (m.backoffSec > 0) {
            std::snprintf(buf, sizeof buf, "wait %lus",
                          static_cast<unsigned long>(m.backoffSec));
        } else if (m.requestsPerHourCap > 0) {
            std::snprintf(buf, sizeof buf, "%u/%u hr",
                          static_cast<unsigned>(m.requestsThisHour),
                          static_cast<unsigned>(m.requestsPerHourCap));
        } else {
            std::snprintf(buf, sizeof buf, "%u/hr",
                          static_cast<unsigned>(m.requestsThisHour));
        }
        row(c, line++, "req", buf);
    }
}

} // namespace oled
