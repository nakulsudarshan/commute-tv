#include "band_renderer.h"

namespace render {

const uint16_t* BandRenderer::band(int y, int rows) {
    // Refuse rather than truncate. A band shorter than the encoder asked for
    // would leave the tail of the strip holding the previous band's pixels,
    // which produces a plausible-looking image with a corrupt stripe in it --
    // far worse to diagnose than a clean failure.
    if (buf_ == nullptr || fn_ == nullptr) return nullptr;
    if (rows <= 0 || rows > maxRows_) return nullptr;
    if (y < 0 || y >= h_ || y + rows > h_) return nullptr;

    // A Canvas that reports the full screen but writes only this strip. Every
    // layout routine draws its usual coordinates; Canvas::pixel discards
    // whatever lands outside.
    Canvas c(buf_, w_, h_, static_cast<int16_t>(y), static_cast<int16_t>(rows));
    fn_(c, ctx_);
    ++count_;
    return buf_;
}

void renderCommuteBand(Canvas& c, const void* ctx) {
    if (ctx == nullptr) return;
    renderCommute(c, *static_cast<const ScreenModel*>(ctx));
}

void renderIdleBand(Canvas& c, const void* ctx) {
    if (ctx == nullptr) return;
    const IdleModel* m = static_cast<const IdleModel*>(ctx);
    renderIdle(c, m->nowText, m->note);
}

} // namespace render
