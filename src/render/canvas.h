// canvas.h - RGB565 framebuffer and drawing primitives.
//
// No Arduino, no display driver. On the ESP32 the same buffer is JPEG-encoded
// and POSTed to the SmallTV; on the host it is written as a BMP so the layout
// can be reviewed without any hardware. Identical pixels either way, which is
// the entire point.
#pragma once
#include <cstdint>
#include <cstddef>
#include "font.h"

namespace render {

using Color = uint16_t;   // RGB565

constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<Color>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Palette. Kept here rather than scattered through the layout so the whole
// scheme can be adjusted in one place.
namespace col {
constexpr Color bg       = rgb(10, 12, 18);
constexpr Color panel    = rgb(22, 26, 36);
constexpr Color text     = rgb(236, 238, 245);
constexpr Color dim      = rgb(130, 138, 155);
constexpr Color accent   = rgb(0, 209, 126);   // on time / good
constexpr Color warn     = rgb(255, 176, 32);  // minor delay / moderate
constexpr Color bad      = rgb(255, 82, 82);   // late / cancelled / rough
constexpr Color bus      = rgb(90, 170, 255);
constexpr Color bike     = rgb(190, 140, 255);
} // namespace col

enum class Align : uint8_t { Left, Center, Right };

// A Canvas is normally a whole frame, but it can instead be a horizontal BAND:
// a buffer holding only rows [bandY, bandY + bandRows) of a taller logical
// screen.
//
// This exists because the ESP32 cannot allocate a full 240x240 RGB565 frame.
// Measured on the real board: 115,200 bytes needed, 110,580 available as the
// largest contiguous block, and malloc(115200) fails. Rendering in strips of
// 16 rows needs 7,680 bytes instead.
//
// The important property is that width() and height() keep reporting the FULL
// screen. Layout code positions things against the real screen size and is
// completely unaware of banding; pixels that fall outside the current band are
// simply discarded. Drawing the same scene once per band therefore produces
// exactly the same image as drawing it once into a full frame -- which the
// tests assert byte for byte.
class Canvas {
public:
    // Whole frame: the buffer holds all `h` rows.
    Canvas(Color* pixels, int16_t w, int16_t h)
        : px_(pixels), w_(w), h_(h), bandY_(0), bandH_(h) {}

    // One band: the buffer holds `bandRows` rows, starting at logical row
    // `bandY` of a screen that is `fullH` rows tall.
    Canvas(Color* pixels, int16_t w, int16_t fullH, int16_t bandY, int16_t bandRows)
        : px_(pixels), w_(w), h_(fullH), bandY_(bandY), bandH_(bandRows) {}

    int16_t width()  const { return w_; }
    int16_t height() const { return h_; }      // the SCREEN, not the buffer
    Color*  data()   const { return px_; }

    // Band geometry. bandRows() is the buffer's real height.
    int16_t bandY()    const { return bandY_; }
    int16_t bandRows() const { return bandH_; }
    bool    isFullFrame() const { return bandY_ == 0 && bandH_ == h_; }

    // Bytes of buffer this canvas actually needs -- the band, not the screen.
    size_t  sizeBytes() const {
        return static_cast<size_t>(w_) * static_cast<size_t>(bandH_) * sizeof(Color);
    }

    void fill(Color c);
    void pixel(int16_t x, int16_t y, Color c);
    void hLine(int16_t x, int16_t y, int16_t len, Color c);
    void vLine(int16_t x, int16_t y, int16_t len, Color c);
    void rect(int16_t x, int16_t y, int16_t w, int16_t h, Color c);
    void frame(int16_t x, int16_t y, int16_t w, int16_t h, Color c);
    void roundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, Color c);

    // Text. Returns the advance width in pixels. `y` is the BASELINE, which
    // makes mixed-size rows line up correctly.
    int16_t text(int16_t x, int16_t y, const char* s, const Font& f, Color c);
    int16_t textWidth(const char* s, const Font& f) const;
    int16_t textAligned(int16_t x, int16_t y, int16_t boxW, const char* s,
                        const Font& f, Color c, Align a);

    // Draws as much of `s` as fits in maxW, appending an ellipsis if truncated.
    // Long stop names are the norm in Aachen, so this is the default path, not
    // an edge case.
    int16_t textClipped(int16_t x, int16_t y, int16_t maxW, const char* s,
                        const Font& f, Color c);

private:
    void glyph(int16_t x, int16_t baseline, const Glyph& g, const Font& f, Color c);
    Color* px_;
    int16_t w_, h_;        // h_ is the logical screen height
    int16_t bandY_;        // logical row this buffer starts at
    int16_t bandH_;        // rows actually in the buffer
};

// Writes a 24-bit BMP. Host-only helper for previewing the layout; returns
// false on any I/O failure, or if `c` is a band rather than a whole frame.
bool writeBmp(const Canvas& c, const char* path);

} // namespace render
