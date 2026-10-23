#include "screen.h"
#include "font_data.h"
#include <cstdio>
#include <cstring>

namespace render {

// Vertical bands. Chosen so the countdown dominates: at 240x240 seen across a
// room, one number has to be readable and everything else is secondary.
static constexpr int16_t kPad = 8;
static constexpr int16_t kInner = 8;                 // padding inside a panel
static constexpr int16_t kContentW = kScreenW - 2 * kPad;

// ⚠️ kDestY IS 76 AND MUST STAY THERE. Moving it to 68 to make room for the
// event title clipped "min ago" off the bottom of the countdown -- the single
// most important text on the screen -- because the 44 px FontLarge baseline
// sits at 30 + ascent and needs every pixel above 76. Seen on the real panel,
// not in the preview, because the preview scenes all had a positive countdown
// and "min" is shorter than "min ago".
//
// The two extra lines the destination panel needed came out of the transit and
// bike panels instead, which had slack. The static_assert keeps it honest.
static constexpr int16_t kDestY = 76;
static constexpr int16_t kDestH = 46;
static constexpr int16_t kTransitY = kDestY + kDestH + 6;
static constexpr int16_t kTransitH = 56;
static constexpr int16_t kBikeY = kTransitY + kTransitH + 6;
static constexpr int16_t kBikeH = 24;
static constexpr int16_t kFooterY = kBikeY + kBikeH + 4;
static_assert(kFooterY == kFooterTop, "screen.h advertises the wrong footer top");

// Draw with `top` as the top edge rather than the baseline: easier to reason
// about when laying out bands.
static int16_t textTop(Canvas& c, int16_t x, int16_t top, const char* s,
                       const Font& f, Color col) {
    return c.text(x, static_cast<int16_t>(top + f.ascent), s, f, col);
}

static void panel(Canvas& c, int16_t y, int16_t h, Color fill) {
    c.roundRect(kPad, y, kContentW, h, 5, fill);
}

// Draws `left` and `right` on one line and guarantees they cannot collide:
// the right string is measured first and the left is clipped to whatever
// remains. Long Aachen stop names are the norm, not an edge case, so this is
// enforced structurally rather than by choosing careful wording.
static void rowLR(Canvas& c, int16_t x, int16_t top, int16_t width,
                  const char* left, Color lc,
                  const char* right, Color rc, const Font& f) {
    const int16_t baseline = static_cast<int16_t>(top + f.ascent);
    int16_t rw = 0;
    if (right && right[0]) {
        rw = c.textWidth(right, f);
        c.text(static_cast<int16_t>(x + width - rw), baseline, right, f, rc);
    }
    const int16_t avail = static_cast<int16_t>(width - rw - (rw ? 8 : 0));
    if (left && left[0] && avail > 12)
        c.textClipped(x, baseline, avail, left, f, lc);
}

static Color delayColor(int32_t delayMin, bool cancelled) {
    if (cancelled) return col::bad;
    if (delayMin >= 5) return col::bad;
    if (delayMin >= 1) return col::warn;
    return col::accent;
}

static Color comfortColor(int32_t score) {
    if (score >= 75) return col::accent;
    if (score >= 45) return col::warn;
    return col::bad;
}

static void drawHeader(Canvas& c, const ScreenModel& m) {
    const bool due = m.minutesToLeave <= 0;      // at zero it is already time to go
    const bool past = m.minutesToLeave < 0;
    const bool urgent = m.minutesToLeave <= 5;
    const Color bar = due ? col::bad : (urgent ? col::warn : col::accent);

    // Accent stripe: readable as a status from across the room, before you
    // have resolved any of the text.
    c.rect(0, 0, kScreenW, 5, bar);

    rowLR(c, kPad, 12, kContentW,
          due ? "LEAVE NOW" : "LEAVE IN", col::dim,
          m.nowText, col::dim, FontSmall);

    char big[12];
    const int32_t mins = m.minutesToLeave < 0 ? -m.minutesToLeave : m.minutesToLeave;
    std::snprintf(big, sizeof big, "%ld", static_cast<long>(mins));

    const int16_t baseline = 30 + FontLarge.ascent;
    const int16_t numW = c.text(kPad, baseline, big, FontLarge, bar);
    c.text(static_cast<int16_t>(kPad + numW + 6),
           static_cast<int16_t>(baseline), past ? "min ago" : "min",
           FontMedium, col::dim);
}

static void drawDestination(Canvas& c, const ScreenModel& m) {
    panel(c, kDestY, kDestH, col::panel);
    const int16_t x = kPad + kInner;
    const int16_t w = kContentW - 2 * kInner;

    char at[16];
    std::snprintf(at, sizeof at, "at %s", m.eventTime);
    rowLR(c, x, static_cast<int16_t>(kDestY + 5), w,
          m.destination, col::text, at, col::dim, FontMedium);

    // The title, dim and small, underneath. Skipped entirely when empty rather
    // than leaving a gap that looks like something failed to load.
    if (m.eventTitle[0]) {
        c.textClipped(x, static_cast<int16_t>(kDestY + 26 + FontSmall.ascent), w,
                      m.eventTitle, FontSmall, col::dim);
    }
}

// "direct" is worth saying out loud, not just implying by the absence of a
// number: it is the thing that makes a journey feel easy.
static void changesText(int32_t transfers, char* out, size_t cap) {
    // Clamped, because these strings are drawn and a truncated "2 chang" would
    // be worse than a wrong number. kMaxLegs caps a real itinerary at five
    // changes anyway; anything above that is a parser accident, not a journey.
    if (transfers > 9) transfers = 9;
    if (transfers <= 0) std::snprintf(out, cap, "direct");
    else if (transfers == 1) std::snprintf(out, cap, "1 change");
    else std::snprintf(out, cap, "%ld changes", static_cast<long>(transfers));
}

static void drawTransit(Canvas& c, const ScreenModel& m) {
    panel(c, kTransitY, kTransitH, col::panel);
    const int16_t x = kPad + kInner;
    const int16_t w = kContentW - 2 * kInner;

    if (!m.hasTransit) {
        textTop(c, x, static_cast<int16_t>(kTransitY + 8), "No connection",
                FontMedium, col::bad);
        c.textClipped(x, static_cast<int16_t>(kTransitY + 34 + FontSmall.ascent),
                      w, m.noTransitReason, FontSmall, col::dim);
        return;
    }

    const Color lc = delayColor(m.delayMin, m.cancelled);

    // Line badge, coloured by severity. First question is "which bus", second
    // is "is it fine" -- the badge answers both at once.
    const int16_t badgeW = static_cast<int16_t>(c.textWidth(m.line, FontMedium) + 16);
    const int16_t badgeH = 24;
    const int16_t badgeY = kTransitY + 7;
    c.roundRect(x, badgeY, badgeW, badgeH, 4, lc);
    c.textAligned(x, static_cast<int16_t>(badgeY + (badgeH - FontMedium.lineHeight) / 2
                                          + FontMedium.ascent),
                  badgeW, m.line, FontMedium, col::bg, Align::Center);

    c.textClipped(static_cast<int16_t>(x + badgeW + 8),
                  static_cast<int16_t>(badgeY + 3 + FontMedium.ascent),
                  static_cast<int16_t>(w - badgeW - 8),
                  m.boardStop, FontMedium, col::text);

    // Second row states the consequence, not just the numbers. When the trip
    // arrives late that replaces the arrival time rather than being squeezed
    // in beside it -- the earlier version overlapped and was unreadable.
    char row[56];
    // Compact on purpose. The first version read "14:50  on time   arr 15:12",
    // which did not leave room for the changes count and clipped to
    // "arr 15..." -- an arrival time with the minutes missing, which is worse
    // than no arrival time at all.
    //
    // "on time" is now said by the COLOUR and by the absence of a +N, which is
    // what delayColor() was already for.
    if (m.cancelled) {
        std::snprintf(row, sizeof row, "%s  CANCELLED", m.depTime);
    } else if (m.delayMin > 0) {
        std::snprintf(row, sizeof row, "%s +%ld - %s", m.depTime,
                      static_cast<long>(m.delayMin), m.arrTime);
    } else {
        std::snprintf(row, sizeof row, "%s - %s", m.depTime, m.arrTime);
    }
    // The right-hand slot says the single most useful thing about this journey.
    // Normally that is how many times you have to change; when you are going to
    // arrive late, it is emphatically that.
    char aside[24];
    if (m.willBeLate) {
        long late = static_cast<long>(m.lateByMin);
        if (late > 999) late = 999;          // a four-digit delay is a bad clock
        if (late < 0) late = 0;
        std::snprintf(aside, sizeof aside, "%ld min late", late);
    } else {
        changesText(m.transfers, aside, sizeof aside);
    }
    rowLR(c, x, static_cast<int16_t>(kTransitY + 34), w, row,
          m.willBeLate ? col::bad : lc, aside,
          m.willBeLate ? col::bad : (m.transfers == 0 ? col::accent : col::dim),
          FontSmall);
}

static void drawBike(Canvas& c, const ScreenModel& m) {
    if (!m.hasCycle) return;
    panel(c, kBikeY, kBikeH, col::panel);

    // The score always carries its reason. A number nobody can explain is a
    // number they stop trusting -- so the reason gets the space it needs and
    // the left-hand detail yields.
    char right[40];
    if (m.comfortReason && m.comfortReason[0])
        std::snprintf(right, sizeof right, "%s %ld %s",
                      m.comfortLabel, static_cast<long>(m.comfortScore), m.comfortReason);
    else
        std::snprintf(right, sizeof right, "%s %ld",
                      m.comfortLabel, static_cast<long>(m.comfortScore));

    const int16_t x = kPad + kInner;
    const int16_t w = kContentW - 2 * kInner;
    const int16_t rw = c.textWidth(right, FontSmall);
    const int16_t avail = static_cast<int16_t>(w - rw - 8);

    // Drop the ascent rather than clipping it: "+8..." is worse than no
    // number at all, because a half-shown figure still reads as a figure.
    char full[40], shortForm[24];
    std::snprintf(full, sizeof full, "BIKE %ldmin  +%ldm",
                  static_cast<long>(m.cycleMin), static_cast<long>(m.ascentM));
    std::snprintf(shortForm, sizeof shortForm, "BIKE %ldmin",
                  static_cast<long>(m.cycleMin));
    const char* left = (c.textWidth(full, FontSmall) <= avail) ? full : shortForm;

    const int16_t baseline = static_cast<int16_t>(kBikeY + 6 + FontSmall.ascent);
    c.text(static_cast<int16_t>(x + w - rw), baseline, right, FontSmall,
           comfortColor(m.comfortScore));
    if (avail > 12) c.textClipped(x, baseline, avail, left, FontSmall, col::bike);
}

// The attribution required by the AVV usage agreement, clause 3.b: the AVV mark
// must be placed "in lesbarer Groesse" -- in legible size -- wherever query
// results are displayed. It is drawn at the same 13 px as the status line, on
// every result screen, and it is not conditional on anything: a screen showing
// AVV data without it would be a breach, and the cheapest way to never get that
// wrong is to never make it optional.
const char* const kAttributionText = "AVV";

static void drawFooter(Canvas& c, const ScreenModel& m) {
    // rowLR measures the right string first and clips the left to what remains,
    // so a long status can never push the attribution off the screen -- which
    // is the failure mode that matters here.
    //
    // Stale data must also announce itself. A plausible-looking screen showing
    // old information is the worst failure this system can have.
    rowLR(c, kPad, kFooterY, kContentW,
          m.statusText, m.stale ? col::warn : col::dim,
          kAttributionText, col::dim, FontSmall);
}

void renderCommute(Canvas& c, const ScreenModel& m) {
    c.fill(col::bg);
    drawHeader(c, m);
    drawDestination(c, m);
    drawTransit(c, m);
    drawBike(c, m);
    drawFooter(c, m);
}

void renderIdle(Canvas& c, const char* nowText, const char* note) {
    c.fill(col::bg);
    c.rect(0, 0, kScreenW, 5, col::dim);
    c.textAligned(0, static_cast<int16_t>(90 + FontLarge.ascent), kScreenW,
                  nowText ? nowText : "--:--", FontLarge, col::text, Align::Center);
    if (note)
        c.textAligned(0, static_cast<int16_t>(160 + FontSmall.ascent), kScreenW,
                      note, FontSmall, col::dim, Align::Center);
}

} // namespace render
