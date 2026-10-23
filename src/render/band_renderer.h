// band_renderer.h - draw the screen one horizontal strip at a time.
//
// WHY THIS EXISTS
// ---------------
// Measured on the actual ESP32 (tools/esp32-checkup, 22 Aug 2026):
//
//     malloc(115200) : FAILED     <- a 240x240 RGB565 frame
//     malloc(110000) : OK
//     largest contiguous free block: 110,580 bytes
//
// The board has 310 KB of free heap, but its DRAM is split into regions, so no
// single allocation can exceed ~110 KB. A full framebuffer misses by 4%.
//
// Rather than shrink the screen or reach for a static buffer sitting near the
// ESP32's ~160 KB static-DRAM ceiling, the frame is produced in strips. The
// scene is re-rendered into each strip and handed straight to the JPEG encoder,
// which walks MCU rows top to bottom and never looks back.
//
//     240 x 240 whole frame : 115,200 bytes   (impossible)
//     240 x  16 band        :   7,680 bytes   (comfortable)
//
// Re-drawing the scene per band costs about fifteen repeats of ~20 primitives.
// That is nothing next to the DCT, and it buys back 107 KB of heap for TLS.
//
// The reason this is safe: Canvas reports the FULL screen size regardless of
// banding, so layout code is untouched and unaware. The tests assert that the
// banded path produces JPEG bytes identical to the whole-frame path.
#pragma once
#include <cstddef>
#include <cstdint>

#include "../jpeg/jpeg_encoder.h"
#include "canvas.h"
#include "screen.h"

namespace render {

// Draws the whole scene into `c`. Called once per band with a Canvas whose
// origin is that band, so it must draw in full-screen coordinates every time
// and let the Canvas discard what falls outside.
//
// A plain function pointer rather than std::function: no allocation, no
// <functional>, and it costs one pointer of state.
using RenderFn = void (*)(Canvas& c, const void* ctx);

class BandRenderer final : public jpeg::BandSource {
public:
    // `buf` must hold at least width * maxBandRows pixels.
    //
    // maxBandRows must be at least the encoder's MCU height: 8 for 4:4:4, 16
    // for 4:2:0. A smaller band is refused rather than silently truncated --
    // see band().
    BandRenderer(Color* buf, int16_t width, int16_t height, int16_t maxBandRows,
                 RenderFn fn, const void* ctx)
        : buf_(buf), w_(width), h_(height), maxRows_(maxBandRows),
          fn_(fn), ctx_(ctx) {}

    const uint16_t* band(int y, int rows) override;

    // How many bytes a buffer for this geometry needs. Use it to size the
    // allocation rather than repeating the arithmetic at the call site.
    static size_t bufferBytes(int16_t width, int16_t bandRows) {
        return static_cast<size_t>(width) * static_cast<size_t>(bandRows)
               * sizeof(Color);
    }

    // Number of times the scene was re-drawn. Only of interest to tests and to
    // the health panel, where an unexpected count means the encoder is asking
    // for bands in an order we did not anticipate.
    uint32_t bandsRendered() const { return count_; }

private:
    Color* buf_;
    int16_t w_, h_, maxRows_;
    RenderFn fn_;
    const void* ctx_;
    uint32_t count_ = 0;
};

// --- ready-made scenes -------------------------------------------------------

// ctx is a `const ScreenModel*`.
void renderCommuteBand(Canvas& c, const void* ctx);

// The idle screen takes two loose strings rather than a model, so they get a
// tiny struct to travel in.
struct IdleModel {
    const char* nowText;
    const char* note;
};

// ctx is a `const IdleModel*`.
void renderIdleBand(Canvas& c, const void* ctx);

} // namespace render
