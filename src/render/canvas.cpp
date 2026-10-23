#include "canvas.h"
#include <cstdio>
#include <cstring>

namespace render {

void Canvas::fill(Color c) {
    // The buffer, which for a band is only bandH_ rows.
    const int32_t n = static_cast<int32_t>(w_) * bandH_;
    for (int32_t i = 0; i < n; ++i) px_[i] = c;
}

void Canvas::pixel(int16_t x, int16_t y, Color c) {
    if (x < 0 || x >= w_) return;
    // Every drawing primitive in this class funnels through here, so banding
    // needs to be handled in exactly one place: translate into the buffer and
    // drop anything outside it. For a whole-frame canvas bandY_ is 0 and
    // bandH_ is h_, making this identical to a plain bounds check.
    const int16_t by = static_cast<int16_t>(y - bandY_);
    if (by < 0 || by >= bandH_) return;
    px_[static_cast<int32_t>(by) * w_ + x] = c;
}

void Canvas::hLine(int16_t x, int16_t y, int16_t len, Color c) {
    for (int16_t i = 0; i < len; ++i) pixel(static_cast<int16_t>(x + i), y, c);
}

void Canvas::vLine(int16_t x, int16_t y, int16_t len, Color c) {
    for (int16_t i = 0; i < len; ++i) pixel(x, static_cast<int16_t>(y + i), c);
}

void Canvas::rect(int16_t x, int16_t y, int16_t w, int16_t h, Color c) {
    for (int16_t j = 0; j < h; ++j) hLine(x, static_cast<int16_t>(y + j), w, c);
}

void Canvas::frame(int16_t x, int16_t y, int16_t w, int16_t h, Color c) {
    hLine(x, y, w, c);
    hLine(x, static_cast<int16_t>(y + h - 1), w, c);
    vLine(x, y, h, c);
    vLine(static_cast<int16_t>(x + w - 1), y, h, c);
}

void Canvas::roundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, Color c) {
    if (r * 2 > w) r = static_cast<int16_t>(w / 2);
    if (r * 2 > h) r = static_cast<int16_t>(h / 2);
    rect(static_cast<int16_t>(x + r), y, static_cast<int16_t>(w - 2 * r), h, c);
    rect(x, static_cast<int16_t>(y + r), r, static_cast<int16_t>(h - 2 * r), c);
    rect(static_cast<int16_t>(x + w - r), static_cast<int16_t>(y + r), r,
         static_cast<int16_t>(h - 2 * r), c);
    // Filled quarter circles at each corner.
    for (int16_t dy = 0; dy < r; ++dy) {
        for (int16_t dx = 0; dx < r; ++dx) {
            const int32_t a = r - dx, b = r - dy;
            if (a * a + b * b <= static_cast<int32_t>(r) * r) {
                pixel(static_cast<int16_t>(x + dx), static_cast<int16_t>(y + dy), c);
                pixel(static_cast<int16_t>(x + w - 1 - dx), static_cast<int16_t>(y + dy), c);
                pixel(static_cast<int16_t>(x + dx), static_cast<int16_t>(y + h - 1 - dy), c);
                pixel(static_cast<int16_t>(x + w - 1 - dx),
                      static_cast<int16_t>(y + h - 1 - dy), c);
            }
        }
    }
}

void Canvas::glyph(int16_t x, int16_t baseline, const Glyph& g, const Font& f, Color c) {
    if (g.width == 0 || g.height == 0) return;
    const uint8_t stride = static_cast<uint8_t>((g.width + 7) / 8);
    const uint8_t* src = f.bits + g.offset;
    for (uint8_t row = 0; row < g.height; ++row) {
        for (uint8_t colIdx = 0; colIdx < g.width; ++colIdx) {
            const uint8_t byte = src[row * stride + (colIdx >> 3)];
            if (byte & (0x80 >> (colIdx & 7))) {
                pixel(static_cast<int16_t>(x + g.xOffset + colIdx),
                      static_cast<int16_t>(baseline + g.yOffset + row), c);
            }
        }
    }
}

int16_t Canvas::text(int16_t x, int16_t y, const char* s, const Font& f, Color c) {
    if (!s) return 0;
    int16_t pen = x;
    for (const char* p = s; *p; ++p) {
        const Glyph* g = f.glyph(*p);
        if (!g) continue;
        glyph(pen, y, *g, f, c);
        pen = static_cast<int16_t>(pen + g->advance);
    }
    return static_cast<int16_t>(pen - x);
}

int16_t Canvas::textWidth(const char* s, const Font& f) const {
    if (!s) return 0;
    int16_t w = 0;
    for (const char* p = s; *p; ++p) {
        const Glyph* g = f.glyph(*p);
        if (g) w = static_cast<int16_t>(w + g->advance);
    }
    return w;
}

int16_t Canvas::textAligned(int16_t x, int16_t y, int16_t boxW, const char* s,
                            const Font& f, Color c, Align a) {
    const int16_t w = textWidth(s, f);
    int16_t sx = x;
    if (a == Align::Center) sx = static_cast<int16_t>(x + (boxW - w) / 2);
    else if (a == Align::Right) sx = static_cast<int16_t>(x + boxW - w);
    return text(sx, y, s, f, c);
}

int16_t Canvas::textClipped(int16_t x, int16_t y, int16_t maxW, const char* s,
                            const Font& f, Color c) {
    if (!s) return 0;
    if (textWidth(s, f) <= maxW) return text(x, y, s, f, c);

    const int16_t ellipsisW = textWidth("...", f);
    int16_t pen = x;
    const int16_t limit = static_cast<int16_t>(x + maxW - ellipsisW);
    for (const char* p = s; *p; ++p) {
        const Glyph* g = f.glyph(*p);
        if (!g) continue;
        if (pen + g->advance > limit) break;
        glyph(pen, y, *g, f, c);
        pen = static_cast<int16_t>(pen + g->advance);
    }
    pen = static_cast<int16_t>(pen + text(pen, y, "...", f, c));
    return static_cast<int16_t>(pen - x);
}

bool writeBmp(const Canvas& c, const char* path) {
    // A band holds only part of the image, so there is nothing sensible to
    // write. Refuse rather than emit a file that silently contains a strip.
    if (!c.isFullFrame()) return false;

    std::FILE* fp = std::fopen(path, "wb");
    if (!fp) return false;

    const int32_t w = c.width(), h = c.height();
    const int32_t rowBytes = (w * 3 + 3) & ~3;          // rows padded to 4 bytes
    const int32_t dataSize = rowBytes * h;
    const int32_t fileSize = 54 + dataSize;

    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    auto put32 = [&](int i, int32_t v) {
        hdr[i] = static_cast<uint8_t>(v); hdr[i+1] = static_cast<uint8_t>(v >> 8);
        hdr[i+2] = static_cast<uint8_t>(v >> 16); hdr[i+3] = static_cast<uint8_t>(v >> 24);
    };
    put32(2, fileSize); put32(10, 54); put32(14, 40);
    put32(18, w); put32(22, -h);                        // negative = top-down
    hdr[26] = 1; hdr[28] = 24;
    put32(34, dataSize);
    if (std::fwrite(hdr, 1, 54, fp) != 54) { std::fclose(fp); return false; }

    uint8_t row[3 * 512 + 4];
    for (int32_t y = 0; y < h; ++y) {
        std::memset(row, 0, static_cast<size_t>(rowBytes));
        for (int32_t x = 0; x < w; ++x) {
            const Color p = c.data()[y * w + x];
            // Expand RGB565 back to 888, replicating the high bits so that
            // white stays white rather than drifting to 248,252,248.
            const uint8_t r5 = static_cast<uint8_t>((p >> 11) & 0x1F);
            const uint8_t g6 = static_cast<uint8_t>((p >> 5) & 0x3F);
            const uint8_t b5 = static_cast<uint8_t>(p & 0x1F);
            row[x * 3 + 2] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
            row[x * 3 + 1] = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
            row[x * 3 + 0] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
        }
        if (std::fwrite(row, 1, static_cast<size_t>(rowBytes), fp)
            != static_cast<size_t>(rowBytes)) { std::fclose(fp); return false; }
    }
    std::fclose(fp);
    return true;
}

} // namespace render
