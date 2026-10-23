#include "test.h"
#include "../src/render/screen.h"
#include "../src/render/canvas.h"
#include "../src/render/font_data.h"

#include <cstring>
#include <vector>

using namespace render;

namespace {

// Renders into a real frame so the assertions are about pixels, not about
// whether a draw call was issued.
struct Frame {
    std::vector<Color> px;
    Canvas c;
    Frame() : px(static_cast<size_t>(kScreenW) * kScreenH, 0),
              c(px.data(), kScreenW, kScreenH) {}

    // How many pixels in a rectangle differ from the background. Text is thin,
    // so a count is a better signal than "any pixel set".
    int litPixels(int16_t x0, int16_t y0, int16_t x1, int16_t y1) const {
        int n = 0;
        for (int16_t y = y0; y < y1; ++y)
            for (int16_t x = x0; x < x1; ++x)
                if (px[static_cast<size_t>(y) * kScreenW + x] != col::bg) ++n;
        return n;
    }
};

// The band the attribution occupies: right-aligned inside the content margin.
struct Box { int16_t x0, y0, x1, y1; };

Box attributionBox() {
    Frame f;
    const int16_t w = f.c.textWidth(kAttributionText, FontSmall);
    const int16_t pad = 8;                       // kPad in screen.cpp
    const int16_t x1 = static_cast<int16_t>(kScreenW - pad);
    return Box{static_cast<int16_t>(x1 - w), kFooterTop, x1, kFooterBottom};
}

ScreenModel normalModel() {
    ScreenModel m;
    m.minutesToLeave = 12;
    std::strcpy(m.nowText, "14:32");
    std::strcpy(m.destination, "Uniklinik");
    std::strcpy(m.eventTime, "15:30");
    m.hasTransit = true;
    std::strcpy(m.line, "47");
    std::strcpy(m.boardStop, "Aachen, Laurensberg");
    std::strcpy(m.depTime, "14:44");
    std::strcpy(m.arrTime, "15:11");
    m.hasCycle = true;
    m.cycleMin = 18;
    m.ascentM = 42;
    m.comfortScore = 71;
    m.comfortLabel = "good";
    m.comfortReason = "16C, dry";
    std::strcpy(m.statusText, "updated 14:32");
    return m;
}

} // namespace

void test_screen() {
    const Box box = attributionBox();

    t::suite("screen / AVV attribution is always present");
    {
        // Clause 3.b of the usage agreement: the AVV mark must be shown in
        // legible size wherever query results are displayed. These assertions
        // exist so that a later layout change cannot quietly drop it.
        Frame f;
        renderCommute(f.c, normalModel());
        CHECK(f.litPixels(box.x0, box.y0, box.x1, box.y1) > 8);
    }
    {   // No status text at all -- the old footer returned early in this case,
        // which would have left AVV data on screen with no attribution.
        ScreenModel m = normalModel();
        m.statusText[0] = '\0';
        Frame f;
        renderCommute(f.c, m);
        CHECK(f.litPixels(box.x0, box.y0, box.x1, box.y1) > 8);
    }
    {   // A status line long enough to run the width of the screen must be
        // clipped rather than allowed to overrun the attribution.
        ScreenModel m = normalModel();
        std::strcpy(m.statusText, "STALE - last ok 13:52 retry");
        m.stale = true;
        Frame f;
        renderCommute(f.c, m);
        CHECK(f.litPixels(box.x0, box.y0, box.x1, box.y1) > 8);
        // And the gap immediately left of the attribution stays clear, so the
        // two strings cannot visually merge into one.
        CHECK_EQ(f.litPixels(static_cast<int16_t>(box.x0 - 4), box.y0, box.x0, box.y1), 0);
    }
    {   // Still shown when there is no connection to report: a failed lookup is
        // as much a query result as a successful one.
        ScreenModel m = normalModel();
        m.hasTransit = false;
        std::strcpy(m.noTransitReason, "no connection found");
        Frame f;
        renderCommute(f.c, m);
        CHECK(f.litPixels(box.x0, box.y0, box.x1, box.y1) > 8);
    }
    {   // Cancelled, delayed and will-be-late variants all still carry it.
        ScreenModel m = normalModel();
        m.cancelled = true;
        m.delayMin = 9;
        m.willBeLate = true;
        m.lateByMin = 6;
        Frame f;
        renderCommute(f.c, m);
        CHECK(f.litPixels(box.x0, box.y0, box.x1, box.y1) > 8);
    }

    t::suite("screen / the idle screen carries no attribution");
    {
        // Nothing on the idle screen came from AVV, so the mark would be a
        // claim about data that is not there.
        Frame f;
        renderIdle(f.c, "14:32", "nothing scheduled");
        CHECK_EQ(f.litPixels(box.x0, box.y0, box.x1, box.y1), 0);
    }

    t::suite("screen / the attribution is legible, not a watermark");
    {
        // "Legible size" is the agreement's wording. Assert it is drawn in the
        // same font as the status line beside it, at full opacity -- i.e. the
        // ink is the dim text colour and not something closer to background.
        Frame f;
        renderCommute(f.c, normalModel());
        bool sawDim = false;
        for (int16_t y = box.y0; y < box.y1; ++y)
            for (int16_t x = box.x0; x < box.x1; ++x)
                if (f.px[static_cast<size_t>(y) * kScreenW + x] == col::dim) sawDim = true;
        CHECK(sawDim);
        // Three characters at 13 px occupy a real amount of the footer.
        CHECK(box.x1 - box.x0 >= 18);
    }
}
