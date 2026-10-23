// mono_canvas.h - a 1-bit framebuffer in the shape the SSD1306 wants.
//
// The OLED is the health panel: the thing you look at when the SmallTV is
// showing nothing and you want to know whether that is correct. It has to say
// what the firmware believes -- WiFi, clock, next appointment, last fetch,
// whether the TV answered -- which means small text and a lot of lines, not a
// headline.
//
// WHY THE ODD MEMORY LAYOUT
// -------------------------
// The SSD1306 addresses its RAM in PAGES: one byte covers eight pixels
// VERTICALLY, and a page is a 128-byte horizontal strip eight pixels tall.
//
//     buf[page * width + x]   bit (y & 7)    <- y increases downward within the byte
//
// Storing the framebuffer in that layout means flushing is a straight memcpy to
// the bus rather than a transpose of 1,024 bytes on a device that has better
// things to do. It costs nothing: every drawing primitive goes through pixel(),
// exactly as on the colour canvas.
//
// 128x64 is 1,024 bytes. That is small enough to hold whole, which is why none
// of the band-rendering machinery appears here.
#pragma once
#include <cstddef>
#include <cstdint>

#include "../render/font.h"

namespace oled {

using render::Font;
using render::Glyph;

constexpr int16_t kWidth = 128;
constexpr int16_t kHeight = 64;
constexpr size_t kBufferBytes = static_cast<size_t>(kWidth) * kHeight / 8;

enum class Ink : uint8_t { Off = 0, On = 1, Invert = 2 };

class MonoCanvas {
public:
    // `buf` must be kBufferBytes long and outlive the canvas.
    explicit MonoCanvas(uint8_t* buf) : buf_(buf) {}

    int16_t width() const { return kWidth; }
    int16_t height() const { return kHeight; }
    const uint8_t* bits() const { return buf_; }
    uint8_t* bits() { return buf_; }

    void clear(Ink ink = Ink::Off);
    void pixel(int16_t x, int16_t y, Ink ink);
    bool get(int16_t x, int16_t y) const;

    void hline(int16_t x, int16_t y, int16_t w, Ink ink = Ink::On);
    void vline(int16_t x, int16_t y, int16_t h, Ink ink = Ink::On);
    void rect(int16_t x, int16_t y, int16_t w, int16_t h, Ink ink = Ink::On);
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, Ink ink = Ink::On);

    // `y` is the BASELINE, matching render::Canvas so the two behave the same.
    // Returns the advance so callers can chain.
    int16_t text(int16_t x, int16_t y, const char* s, const Font& f, Ink ink = Ink::On);
    int16_t textWidth(const char* s, const Font& f) const;

    // Draws as much as fits in `maxWidth` and appends ".." when it had to stop.
    // A health line that runs off the edge is worse than one that admits it.
    int16_t textClipped(int16_t x, int16_t y, int16_t maxWidth, const char* s,
                        const Font& f, Ink ink = Ink::On);

    // Right-aligned at `x + w`, for the value half of a label/value row.
    int16_t textRight(int16_t x, int16_t w, int16_t y, const char* s, const Font& f,
                      Ink ink = Ink::On);

private:
    void glyph(int16_t x, int16_t baseline, const Glyph& g, const Font& f, Ink ink);
    uint8_t* buf_;
};

} // namespace oled
