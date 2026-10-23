#include "comfort.h"

namespace core {

static float lerp(float a, float b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return a + (b - a) * t;
}

float temperatureScore(float x, const ComfortConfig& c) {
    if (x <= c.tColdFloor) return 0.0f;
    if (x < c.tModerateLo)
        return lerp(0.0f, c.vModerate, (x - c.tColdFloor) / (c.tModerateLo - c.tColdFloor));
    if (x < c.tIdealRampLo) return c.vModerate;
    if (x < c.tIdealLo)
        return lerp(c.vModerate, 1.0f, (x - c.tIdealRampLo) / (c.tIdealLo - c.tIdealRampLo));
    if (x <= c.tIdealHi) return 1.0f;
    if (x < c.tHeatCeil)
        return lerp(1.0f, 0.0f, (x - c.tIdealHi) / (c.tHeatCeil - c.tIdealHi));
    return 0.0f;
}

float effortScore(float ascentM, float distanceKm, const ComfortConfig& c) {
    // A zero-length route has no climb to speak of; treat as flat rather than
    // dividing by zero and producing a NaN that silently poisons the score.
    if (distanceKm <= 0.01f) return c.eFlat;
    if (ascentM < 0.0f) ascentM = 0.0f;

    const float rate = ascentM / distanceKm;
    if (rate <= c.cFlat) return c.eFlat;
    if (rate <= c.cGentle)
        return lerp(c.eFlat, c.eGentle, (rate - c.cFlat) / (c.cGentle - c.cFlat));
    if (rate <= c.cNoticeable)
        return lerp(c.eGentle, c.eNoticeable, (rate - c.cGentle) / (c.cNoticeable - c.cGentle));
    if (rate <= c.cHard)
        return lerp(c.eNoticeable, c.eHard, (rate - c.cNoticeable) / (c.cHard - c.cNoticeable));
    return c.eHard;
}

float weatherPenalty(float precipProbPct, float windKph, const ComfortConfig& c) {
    float w = 1.0f;
    if (precipProbPct > c.rainHiPct)       w *= c.wRainHi;
    else if (precipProbPct > c.rainMidPct) w *= c.wRainMid;

    if (windKph > c.windHiKph)       w *= c.wWindHi;
    else if (windKph > c.windMidKph) w *= c.wWindMid;
    return w;
}

const char* ComfortResult::label() const {
    // Thresholds duplicated from ComfortConfig defaults deliberately: label()
    // has no access to the config, and these are presentation-only.
    if (score >= 75) return "Ideal";
    if (score >= 45) return "OK";
    return "Rough";
}

const char* ComfortResult::reasonText() const {
    switch (reason) {
        case ComfortReason::Cold:  return "cold";
        case ComfortReason::Heat:  return "heat";
        case ComfortReason::Climb: return "climb";
        case ComfortReason::Rain:  return "rain";
        case ComfortReason::Wind:  return "wind";
        default: return "";
    }
}

ComfortResult computeComfort(const Weather& wx, const CycleRoute& r, const ComfortConfig& c) {
    ComfortResult out;
    if (!r.valid) return out;

    // Missing weather must not be silently treated as perfect weather. Assume
    // the ideal-band midpoint and no precipitation, and let the caller decide
    // whether to show the score at all.
    const float tempC = wx.valid ? wx.tempC : 20.0f;
    const float rain  = wx.valid ? wx.precipProbPct : 0.0f;
    const float wind  = wx.valid ? wx.windKph : 0.0f;

    out.t = temperatureScore(tempC, c);
    out.e = effortScore(r.ascentM, r.distanceKm, c);
    out.w = weatherPenalty(rain, wind, c);

    float s = (0.5f * out.t + 0.5f * out.e) * out.w * 100.0f;
    if (s < 0.0f) s = 0.0f;
    if (s > 100.0f) s = 100.0f;
    out.score = static_cast<int>(s + 0.5f);

    // Dominant reason: the factor costing the most. A bare number nobody can
    // explain is a number they stop trusting.
    float worst = 1.0f;
    out.reason = ComfortReason::None;
    auto consider = [&](float factor, ComfortReason why) {
        if (factor < worst) { worst = factor; out.reason = why; }
    };
    if (wx.valid) {
        if (rain > c.rainMidPct) consider(rain > c.rainHiPct ? c.wRainHi : c.wRainMid, ComfortReason::Rain);
        if (wind > c.windMidKph) consider(wind > c.windHiKph ? c.wWindHi : c.wWindMid, ComfortReason::Wind);
    }
    consider(out.e, ComfortReason::Climb);
    if (out.t < 1.0f) consider(out.t, tempC < c.tIdealLo ? ComfortReason::Cold : ComfortReason::Heat);
    return out;
}

} // namespace core
