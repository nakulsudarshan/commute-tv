// font.h - 1bpp baked bitmap fonts.
#pragma once
#include <cstdint>

// On the ESP32 these arrays should live in flash. Left empty on the host so
// the same headers compile for tests and the preview tool.
#ifndef PROGMEM_ATTR
#define PROGMEM_ATTR
#endif

namespace render {

struct Glyph {
    uint16_t offset;   // byte offset into the font's bitmap blob
    uint8_t  width;
    uint8_t  height;
    int8_t   xOffset;  // bearing from the pen position
    int8_t   yOffset;  // relative to the baseline, negative is above
    uint8_t  advance;  // pen movement after drawing
};

struct Font {
    const uint8_t* bits;
    const Glyph*   glyphs;
    uint8_t first;
    uint8_t last;
    uint8_t lineHeight;
    uint8_t ascent;

    const Glyph* glyph(char c) const {
        const uint8_t u = static_cast<uint8_t>(c);
        if (u < first || u > last) return nullptr;
        return &glyphs[u - first];
    }
};

} // namespace render
