#include "meteo_client.h"

#include <cstdio>
#include <cstring>

#include "../core/urlencode.h"
#include "../json/json_stream.h"
#include "../net/json_sink.h"

namespace meteo {
namespace {

// The response holds three parallel arrays under `hourly`, plus the `time`
// array that says which hour each index is. Open-Meteo emits `time` first, so a
// single pass can find the index and then read it out of the others -- but
// relying on field order would be a quiet way to be wrong, so the values are
// kept for every index up to a small cap and selected at the end.
constexpr int kMaxHours = 4;   // a start_hour/end_hour request returns 1 or 2

struct HourlyHandler final : json::Handler {
    core::Timestamp want;

    core::Timestamp times[kMaxHours];
    float temp[kMaxHours] = {0};
    float precip[kMaxHours] = {0};
    float wind[kMaxHours] = {0};
    bool haveTemp[kMaxHours] = {false};
    bool havePrecip[kMaxHours] = {false};
    bool haveWind[kMaxHours] = {false};
    int count = 0;

    char error[80] = {0};

    void onScalar(const json::Path& p, json::Type t, const char* text, size_t len,
                  bool) override {
        if (p.is("reason") || p.is("error")) {
            if (t == json::Type::String && error[0] == '\0') {
                const size_t n = len < sizeof error - 1 ? len : sizeof error - 1;
                std::memcpy(error, text, n);
                error[n] = '\0';
            }
            return;
        }
        if (!p.startsWith("hourly")) return;

        const int i = p.nthIndex(0);
        if (i < 0 || i >= kMaxHours) return;
        if (i + 1 > count) count = i + 1;

        if (p.is("hourly.time[*]")) {
            // "2026-08-25T14:00" -- local wall clock, because the request named
            // the timezone. Read exactly like a HAFAS timestamp.
            times[i] = core::parseIso8601Local(text);
            return;
        }

        // A null is not a zero. Open-Meteo returns null for a variable a model
        // does not carry, and "no rain probability" must never become "0% rain".
        if (t != json::Type::Number) return;
        bool okNum = false;
        const double v = json::toDouble(text, &okNum);
        if (!okNum) return;

        if (p.is("hourly.temperature_2m[*]"))            { temp[i] = static_cast<float>(v);   haveTemp[i] = true; }
        else if (p.is("hourly.precipitation_probability[*]")) { precip[i] = static_cast<float>(v); havePrecip[i] = true; }
        else if (p.is("hourly.wind_speed_10m[*]"))       { wind[i] = static_cast<float>(v);   haveWind[i] = true; }
    }

    // The index whose hour matches, or -1. Falls back to the only entry when
    // exactly one came back, which is the normal case for a one-hour request.
    int pick() const {
        if (want.valid()) {
            for (int i = 0; i < count; ++i) {
                if (!times[i].valid()) continue;
                // Same hour, not same minute: the forecast is hourly.
                if (times[i].min / 60 == want.min / 60) return i;
            }
        }
        return count == 1 ? 0 : -1;
    }
};

} // namespace

const char* statusName(Status s) {
    switch (s) {
        case Status::Ok:          return "ok";
        case Status::BadConfig:   return "bad config";
        case Status::Transport:   return "transport";
        case Status::HttpError:   return "http error";
        case Status::BadResponse: return "bad response";
        case Status::NoData:      return "no data";
    }
    return "?";
}

int buildUrl(const Config& cfg, const Query& q, char* out, size_t outCap) {
    if (out == nullptr || outCap == 0 || cfg.baseUrl == nullptr) return -1;
    if (!q.at.valid()) return -1;
    if (q.lat < -90.0 || q.lat > 90.0 || q.lon < -180.0 || q.lon > 180.0) return -1;

    // Round down to the hour: the forecast is hourly, and asking for 14:37
    // returns nothing.
    int32_t hourMin = q.at.min - (q.at.min % 60);
    if (q.at.min < 0 && q.at.min % 60 != 0) hourMin -= 60;

    char from[32], to[32];
    core::formatRfc3339(core::makeTs(hourMin), 0, from);
    core::formatRfc3339(core::makeTs(hourMin + 60), 0, to);
    // start_hour wants "2026-08-25T14:00" -- no seconds, no offset. The offset
    // would be wrong to send anyway: `timezone` already says which zone these
    // are in, and sending both invites disagreement.
    from[16] = '\0';
    to[16] = '\0';

    char tz[48];
    if (core::urlEncode(cfg.timeZone ? cfg.timeZone : "UTC", tz, sizeof tz) < 0) return -1;
    char fromEnc[48], toEnc[48];
    if (core::urlEncode(from, fromEnc, sizeof fromEnc) < 0) return -1;
    if (core::urlEncode(to, toEnc, sizeof toEnc) < 0) return -1;

    char latBuf[24], lonBuf[24];
    std::snprintf(latBuf, sizeof latBuf, "%.4f", q.lat);   // ~11 m, ample for weather
    std::snprintf(lonBuf, sizeof lonBuf, "%.4f", q.lon);

    const int n = std::snprintf(out, outCap,
        "%s?latitude=%s&longitude=%s"
        "&hourly=temperature_2m,precipitation_probability,wind_speed_10m"
        "&timezone=%s&start_hour=%s&end_hour=%s",
        cfg.baseUrl, latBuf, lonBuf, tz, fromEnc, toEnc);
    if (n < 0 || static_cast<size_t>(n) >= outCap) return -1;
    return n;
}

Status Client::fetch(const Query& q, core::Weather& out) {
    out = core::Weather{};

    char url[320];
    if (buildUrl(cfg_, q, url, sizeof url) < 0) {
        setError("could not build the forecast url");
        return Status::BadConfig;
    }

    HourlyHandler h;
    h.want = q.at;
    json::Reader rd(h);
    net::JsonSink sink(rd);

    const net::Header headers[] = {{"Accept", "application/json"}};
    const net::Result r = http_->get(url, headers, 1, sink);
    lastHttpStatus_ = r.status;

    if (r.error != net::Error::None && r.error != net::Error::Aborted) {
        setError(net::errorName(r.error));
        return Status::Transport;
    }
    if (!sink.finish()) {
        setError(rd.error());
        return Status::BadResponse;
    }
    if (r.status < 200 || r.status >= 300) {
        setError(h.error[0] ? h.error : "unexpected http status");
        return Status::HttpError;
    }

    const int i = h.pick();
    if (i < 0) {
        setError("no forecast for that hour");
        return Status::NoData;
    }

    // Temperature is the one the comfort score cannot do without. Rain and wind
    // missing is survivable -- they stay at their neutral defaults and the
    // score says so -- but a missing temperature would make it guess.
    if (!h.haveTemp[i]) {
        setError("no temperature for that hour");
        return Status::NoData;
    }

    out.tempC = h.temp[i];
    if (h.havePrecip[i]) out.precipProbPct = h.precip[i];
    if (h.haveWind[i]) out.windKph = h.wind[i];
    out.valid = true;
    setError("");
    return Status::Ok;
}

} // namespace meteo
