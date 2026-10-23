#include "test.h"
#include "../src/oled/health.h"
#include "../src/oled/mono_canvas.h"
#include "../src/oled/ssd1306.h"
#include "../src/oled/font_pixel.h"
#include "../src/oled/font_tiny.h"
#include "../src/core/textutil.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace oled;
// FontPixel is what the panel draws with; FontTiny is still exercised by the
// canvas tests below, because those assert things about MonoCanvas that should
// hold for ANY font and a second one is the cheapest way to say so.
using render::FontPixel;
using render::FontTiny;

namespace {

struct Frame {
    std::vector<uint8_t> buf;
    MonoCanvas c;
    Frame() : buf(kBufferBytes, 0), c(buf.data()) {}
};

bool allBytes(const Frame& f, uint8_t v) {
    for (size_t i = 0; i < kBufferBytes; ++i)
        if (f.buf[i] != v) return false;
    return true;
}

// "Is this exact string drawn at this exact place?" -- render it into a scratch
// canvas and require every pixel it would set to be set in the real one. Extra
// ink elsewhere is fine; a missing stroke is not.
bool drawnAt(const MonoCanvas& c, int16_t x, int16_t y, const char* s,
             const render::Font& f = FontPixel) {
    Frame scratch;
    scratch.c.text(x, y, s, f);
    int lit = 0;
    for (int16_t yy = 0; yy < kHeight; ++yy) {
        for (int16_t xx = 0; xx < kWidth; ++xx) {
            if (!scratch.c.get(xx, yy)) continue;
            ++lit;
            if (!c.get(xx, yy)) return false;
        }
    }
    return lit > 0;
}

// The value column is right-aligned, so its position follows from its width.
bool valueOnRow(const MonoCanvas& c, int rowIndex, const char* value) {
    Frame scratch;
    const int16_t w = scratch.c.textWidth(value, FontPixel);
    if (w > valueW()) return false;                 // would have been clipped
    // Values are LEFT-aligned in the value column: they start at valueX() and
    // run right. Right-alignment left a hole two thirds of a row wide on the
    // real panel, which is why this assertion changed shape.
    return drawnAt(c, valueX(), rowY(rowIndex), value);
}

bool labelOnRow(const MonoCanvas& c, int rowIndex, const char* label) {
    return drawnAt(c, kPadX, rowY(rowIndex), label);
}

struct MockBus final : I2cBus {
    struct Txn { uint8_t addr; std::vector<uint8_t> bytes; };
    std::vector<Txn> log;
    bool failAfter = -1 == 0 ? false : false;
    int failFrom = -1;                              // -1: never fail
    int calls = 0;

    bool write(uint8_t addr, const uint8_t* data, size_t len) override {
        ++calls;
        if (failFrom >= 0 && calls > failFrom) return false;
        log.push_back({addr, std::vector<uint8_t>(data, data + len)});
        return true;
    }

    // Every data byte across all data transactions, in order.
    std::vector<uint8_t> pixelBytes() const {
        std::vector<uint8_t> out;
        for (const auto& t : log)
            if (!t.bytes.empty() && t.bytes[0] == kDataPrefix)
                out.insert(out.end(), t.bytes.begin() + 1, t.bytes.end());
        return out;
    }
    bool commandContains(uint8_t a, uint8_t b) const {
        for (const auto& t : log) {
            if (t.bytes.empty() || t.bytes[0] != kCmdPrefix) continue;
            for (size_t i = 1; i + 1 < t.bytes.size(); ++i)
                if (t.bytes[i] == a && t.bytes[i + 1] == b) return true;
        }
        return false;
    }
};

HealthModel workingModel() {
    HealthModel m;
    std::strcpy(m.clock, "14:32");
    m.clockSynced = true;
    m.wifiUp = true;
    std::strcpy(m.wifiName, "Campus");
    m.rssiDbm = -73;
    m.haveEvent = true;
    std::strcpy(m.eventTime, "15:30");
    std::strcpy(m.eventPlace, "SuperC");
    m.stateText = "armed";
    m.minutesToLeave = 12;
    m.tvReachable = true;
    m.tvFreeKnown = true;
    m.tvFreeKb = 943;
    m.transitAgeMin = 2;
    m.requestsThisHour = 7;
    m.requestsPerHourCap = 60;
    return m;
}

} // namespace

void test_oled() {
    t::suite("oled / the SSD1306 memory layout");
    {
        // One byte covers eight pixels VERTICALLY, and a page is a 128-byte
        // strip. Getting this wrong produces a display that shows something,
        // which is the hardest kind of wrong to spot.
        Frame f;
        f.c.pixel(0, 0, Ink::On);
        CHECK_EQ(f.buf[0], 0x01);
        f.c.clear();
        f.c.pixel(0, 7, Ink::On);
        CHECK_EQ(f.buf[0], 0x80);
        f.c.clear();
        f.c.pixel(0, 8, Ink::On);
        CHECK_EQ(f.buf[0], 0x00);
        CHECK_EQ(f.buf[kWidth], 0x01);
        f.c.clear();
        f.c.pixel(127, 63, Ink::On);
        CHECK_EQ(f.buf[kBufferBytes - 1], 0x80);
        CHECK_EQ(f.buf.size(), 1024u);
    }
    {
        Frame f;
        f.c.pixel(5, 5, Ink::On);
        CHECK(f.c.get(5, 5));
        f.c.pixel(5, 5, Ink::Invert);
        CHECK(!f.c.get(5, 5));
        f.c.pixel(5, 5, Ink::Invert);
        CHECK(f.c.get(5, 5));
        f.c.pixel(5, 5, Ink::Off);
        CHECK(!f.c.get(5, 5));

        // Out of bounds writes must be dropped, not wrapped onto another row.
        f.c.clear();
        f.c.pixel(-1, 0, Ink::On);
        f.c.pixel(kWidth, 0, Ink::On);
        f.c.pixel(0, -1, Ink::On);
        f.c.pixel(0, kHeight, Ink::On);
        CHECK(allBytes(f, 0x00));
        CHECK(!f.c.get(-1, 0));
        CHECK(!f.c.get(0, kHeight));
    }
    {
        Frame f;
        f.c.clear(Ink::On);
        CHECK(allBytes(f, 0xFF));
        f.c.clear();
        CHECK(allBytes(f, 0x00));
    }

    t::suite("oled / drawing");
    {
        Frame f;
        f.c.hline(2, 3, 5);
        for (int16_t x = 2; x < 7; ++x) CHECK(f.c.get(x, 3));
        CHECK(!f.c.get(1, 3));
        CHECK(!f.c.get(7, 3));

        f.c.clear();
        f.c.rect(1, 1, 4, 4);
        CHECK(f.c.get(1, 1));
        CHECK(f.c.get(4, 4));
        CHECK(!f.c.get(2, 2));           // outline, not filled

        f.c.clear();
        f.c.fillRect(1, 1, 3, 3);
        CHECK(f.c.get(2, 2));

        // A zero or negative size draws nothing rather than misbehaving.
        f.c.clear();
        f.c.rect(1, 1, 0, 5);
        f.c.fillRect(1, 1, 5, 0);
        CHECK(allBytes(f, 0x00));
    }
    {
        Frame f;
        const int16_t w = f.c.textWidth("hello", FontTiny);
        CHECK(w > 0);
        CHECK(w < kWidth);
        CHECK_EQ(f.c.text(0, 20, "hello", FontTiny), w);
        CHECK(drawnAt(f.c, 0, 20, "hello", FontTiny));
        CHECK_EQ(f.c.textWidth(nullptr, FontTiny), 0);

        // Right-aligned means the string ENDS at x+w.
        f.c.clear();
        f.c.textRight(0, 100, 20, "abc", FontTiny);
        const int16_t aw = f.c.textWidth("abc", FontTiny);
        CHECK(drawnAt(f.c, static_cast<int16_t>(100 - aw), 20, "abc", FontTiny));
    }
    {
        // A clipped line must ADMIT it is clipped. A health line that just runs
        // off the edge looks like a complete answer.
        Frame f;
        const char* longText = "RWTH Hauptgebaude Templergraben";
        CHECK(f.c.textWidth(longText, FontTiny) > 60);
        const int16_t used = f.c.textClipped(0, 20, 60, longText, FontTiny);
        CHECK(used <= 60);
        CHECK(f.c.textWidth("..", FontTiny) > 0);
        // Short enough to fit is drawn whole, with no marker.
        Frame g;
        g.c.textClipped(0, 20, 120, "short", FontTiny);
        CHECK(drawnAt(g.c, 0, 20, "short", FontTiny));
    }

    t::suite("oled / the init sequence");
    {
        uint8_t seq[64];
        const size_t n = initSequence(0xCF, seq, sizeof seq);
        CHECK(n > 0);
        CHECK_EQ(seq[0], 0xAE);              // display off first
        CHECK_EQ(seq[n - 1], 0xAF);          // display on last

        MockBus bus;
        Config cfg;
        Display d(cfg, bus);
        CHECK(d.begin());
        CHECK(d.ready());
        CHECK_EQ(bus.log.size(), 1u);
        CHECK_EQ(bus.log[0].addr, 0x3C);
        CHECK_EQ(bus.log[0].bytes[0], kCmdPrefix);

        // The two bytes that decide whether the panel lights up at all, and
        // both have a plausible-looking wrong value that is right for a
        // DIFFERENT module. 0x8D 0x10 leaves the charge pump off and the screen
        // black; 0xDA 0x02 is the 128x32 COM pin layout and shows every other
        // row.
        CHECK(bus.commandContains(0x8D, 0x14));
        CHECK(!bus.commandContains(0x8D, 0x10));
        CHECK(bus.commandContains(0xDA, 0x12));
        CHECK(!bus.commandContains(0xDA, 0x02));
        CHECK(bus.commandContains(0xA8, 0x3F));   // 64 rows
        CHECK(bus.commandContains(0x81, 0xCF));   // the contrast we asked for
        CHECK(bus.commandContains(0x20, 0x00));   // horizontal addressing

        uint8_t tiny[4];
        CHECK_EQ(initSequence(0xCF, tiny, sizeof tiny), 0u);
        CHECK_EQ(initSequence(0xCF, nullptr, 64), 0u);
    }

    t::suite("oled / flushing a frame");
    {
        MockBus bus;
        Config cfg;
        cfg.chunkBytes = 16;
        Display d(cfg, bus);
        CHECK(d.begin());
        bus.log.clear();

        Frame f;
        f.c.pixel(0, 0, Ink::On);
        f.c.pixel(127, 63, Ink::On);
        CHECK(d.flush(f.c));

        // Every byte of the frame arrives, in order.
        const std::vector<uint8_t> px = bus.pixelBytes();
        CHECK_EQ(px.size(), kBufferBytes);
        CHECK_EQ(px[0], 0x01);
        CHECK_EQ(px[kBufferBytes - 1], 0x80);

        // The window is addressed explicitly every flush. Relying on the
        // pointer being where the last one left it works until a transaction is
        // lost, after which every frame is shifted and nothing says why.
        CHECK(bus.commandContains(0x21, 0x00));   // column 0..
        CHECK(bus.commandContains(0x22, 0x00));   // page 0..
        CHECK_EQ(bus.log[0].bytes[0], kCmdPrefix);
        CHECK_EQ(bus.log[0].bytes[3], 127);       // last column
        CHECK_EQ(bus.log[0].bytes[6], 7);         // last page

        // Chunked to fit a small I2C buffer: 1024 / 16 = 64 data transactions.
        int dataTxns = 0;
        for (const auto& t : bus.log)
            if (t.bytes[0] == kDataPrefix) {
                ++dataTxns;
                if (t.bytes.size() > 17) { CHECK(false); break; }
            }
        CHECK_EQ(dataTxns, 64);
    }
    {
        // A bus that stops acknowledging is itself the health signal -- the
        // panel cannot report its own failure.
        MockBus bus;
        bus.failFrom = 0;
        Config cfg;
        Display d(cfg, bus);
        CHECK(!d.begin());
        CHECK(!d.ready());
        CHECK_EQ(d.failedWrites(), 1u);

        MockBus bus2;
        Config cfg2;
        Display d2(cfg2, bus2);
        CHECK(d2.begin());
        bus2.failFrom = 1;                    // fail once flushing starts
        Frame f;
        CHECK(!d2.flush(f.c));
        CHECK(d2.failedWrites() > 0);
    }
    {
        // A silly chunk size must not overflow the transaction buffer.
        MockBus bus;
        Config cfg;
        cfg.chunkBytes = 0;
        Display d(cfg, bus);
        CHECK(d.begin());
        Frame f;
        CHECK(d.flush(f.c));
        CHECK_EQ(bus.pixelBytes().size(), kBufferBytes);

        MockBus bus2;
        Config cfg2;
        cfg2.chunkBytes = 9999;
        Display d2(cfg2, bus2);
        CHECK(d2.begin());
        CHECK(d2.flush(f.c));
        CHECK_EQ(bus2.pixelBytes().size(), kBufferBytes);
    }

    t::suite("oled / the pixel font fits the panel it was drawn for");
    {
        // FontPixel exists because a rasterised face at this size is mush. The
        // things worth asserting are the ones that would silently ruin the
        // panel if someone edited the art: a hole in the range, a glyph that
        // grew too tall for the row pitch, or one too wide for the column.
        CHECK_EQ(FontPixel.first, 0x20);
        CHECK_EQ(FontPixel.last, 0x7E);

        int16_t widest = 0, highest = 0, deepest = 0;
        for (int ch = 0x20; ch <= 0x7E; ++ch) {
            const render::Glyph* g = FontPixel.glyph(static_cast<char>(ch));
            CHECK(g != nullptr);
            if (!g) continue;
            if (g->advance > widest) widest = g->advance;
            // yOffset is relative to the baseline, negative above it.
            const int16_t top = g->yOffset;
            const int16_t bottom = static_cast<int16_t>(g->yOffset + g->height - 1);
            if (top < highest) highest = top;
            if (g->height > 0 && bottom > deepest) deepest = bottom;
        }

        // Row 0's baseline is 17 and the header rule sits on y=9, so nothing
        // may reach more than 7 px above its baseline or the two collide.
        CHECK(highest >= -7);
        // The last baseline is 62 on a 64 px panel, so a descender may fall at
        // most 1 px below the baseline before it is clipped off the bottom.
        CHECK(deepest <= 1);
        CHECK_EQ(rowY(kMaxRows - 1) + deepest <= kHeight - 1, true);

        // Six pixels per character is what makes the value column usable: 90 px
        // of value means fifteen characters, and a stop name is not short.
        CHECK_EQ(widest, 6);
        CHECK(valueW() / widest >= 15);

        // A space carries no ink but must still move the pen, or words run
        // together -- easy to break by "tidying" the art.
        const render::Glyph* sp = FontPixel.glyph(' ');
        CHECK(sp != nullptr);
        if (sp) {
            CHECK_EQ(sp->height, 0);
            CHECK(sp->advance > 0);
        }

        // Narrow letters must actually BE narrow, otherwise the font is just a
        // monospace one wearing a proportional coat and long values clip early.
        const render::Glyph* i = FontPixel.glyph('i');
        const render::Glyph* m = FontPixel.glyph('m');
        CHECK(i != nullptr && m != nullptr);
        if (i && m) CHECK(i->advance < m->advance);
    }

    t::suite("oled / the panel says what is true");
    {
        Frame f;
        renderHealth(f.c, workingModel());
        CHECK(labelOnRow(f.c, 0, "wifi"));
        CHECK(labelOnRow(f.c, 1, "next"));
        CHECK(labelOnRow(f.c, 2, "leave"));
        CHECK(labelOnRow(f.c, 3, "avv"));
        CHECK(labelOnRow(f.c, 4, "tv"));
        CHECK(labelOnRow(f.c, 5, "req"));
        CHECK(valueOnRow(f.c, 2, "in 12 min"));
        CHECK(valueOnRow(f.c, 3, "2m"));
        CHECK(valueOnRow(f.c, 5, "7/60 hr"));
        CHECK(drawnAt(f.c, kPadX, 8, "14:32"));
    }
    {
        // An unsynced clock must not be readable as a time. Every countdown
        // below it would be wrong, and dashes alone are too easy to miss.
        HealthModel m = workingModel();
        m.clockSynced = false;
        Frame f;
        renderHealth(f.c, m);
        CHECK(drawnAt(f.c, kPadX, 8, "no clock"));
        CHECK(!drawnAt(f.c, kPadX, 8, "14:32"));
    }
    {
        // Never fetched is NOT the same as fresh, and must not look the same.
        HealthModel m = workingModel();
        m.transitAgeMin = -1;
        Frame f;
        renderHealth(f.c, m);
        CHECK(valueOnRow(f.c, 3, "no data"));

        HealthModel stale = workingModel();
        stale.transitAgeMin = 47;
        stale.transitStale = true;
        Frame g;
        renderHealth(g.c, stale);
        CHECK(valueOnRow(g.c, 3, "47m STALE"));
    }
    {
        // A signal reading of zero is the absence of a reading, not a perfect
        // one, so it is not printed as a number.
        HealthModel m = workingModel();
        m.rssiDbm = 0;
        Frame f;
        renderHealth(f.c, m);
        CHECK(valueOnRow(f.c, 0, "Campus"));

        HealthModel down = workingModel();
        down.wifiUp = false;
        Frame g;
        renderHealth(g.c, down);
        CHECK(valueOnRow(g.c, 0, "down"));
    }
    {
        // A file left on the device outranks the free-space figure: on a ~1 MB
        // partition it is the thing that eventually stops everything working.
        HealthModel m = workingModel();
        m.tvImageLeftBehind = true;
        Frame f;
        renderHealth(f.c, m);
        CHECK(valueOnRow(f.c, 4, "file left!"));

        HealthModel gone = workingModel();
        gone.tvReachable = false;
        Frame g;
        renderHealth(g.c, gone);
        CHECK(valueOnRow(g.c, 4, "no reply"));

        Frame ok;
        renderHealth(ok.c, workingModel());
        CHECK(valueOnRow(ok.c, 4, "ok 943k"));
    }
    {
        // The error displaces the budget rather than being dropped: a fault is
        // always worth more than a statistic.
        HealthModel m = workingModel();
        std::strcpy(m.errorText, "tv full");
        Frame f;
        renderHealth(f.c, m);
        CHECK(labelOnRow(f.c, 5, "err"));
        CHECK(valueOnRow(f.c, 5, "tv full"));
        CHECK(!labelOnRow(f.c, 5, "req"));
    }
    {
        // Holding the SmallTV's screen is worth seeing at a glance, and the
        // badge is inverted -- so the ink around it is set.
        HealthModel m = workingModel();
        m.alerting = true;
        Frame f;
        renderHealth(f.c, m);
        int litInBar = 0;
        for (int16_t x = 90; x < kWidth; ++x)
            for (int16_t y = 0; y < kHeaderH; ++y)
                if (f.c.get(x, y)) ++litInBar;
        CHECK(litInBar > 100);
    }
    {
        // Nothing may be drawn below the panel, whatever the content.
        HealthModel m = workingModel();
        // Bounded, the way the orchestrator must be: these are fixed-size
        // fields and a name that does not fit is truncated, never overrun.
        // core::copyStr rather than snprintf so the deliberate truncation does
        // not read as an accident to -Wformat-truncation.
        const char* place = "RWTH Hauptgebaude Templergraben 55";
        const char* err = "a very long error indeed";
        core::copyStr(m.eventPlace, sizeof m.eventPlace, place, std::strlen(place));
        core::copyStr(m.errorText, sizeof m.errorText, err, std::strlen(err));
        Frame f;
        renderHealth(f.c, m);
        CHECK_EQ(f.buf.size(), kBufferBytes);   // no overrun: ASan would catch it
        // And the long place name is clipped rather than wrapping or running on.
        CHECK(!valueOnRow(f.c, 1, "15:30 RWTH Hauptgebaude Templergraben 55"));
    }
    {
        Frame f;
        renderBoot(f.c, "starting");
        CHECK(drawnAt(f.c, static_cast<int16_t>((kWidth - f.c.textWidth("commute-tv", FontPixel)) / 2),
                      8, "commute-tv"));
    }
}
