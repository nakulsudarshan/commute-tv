#include "ors_client.h"

#include <cstdio>
#include <cstring>

#include "../json/json_stream.h"
#include "../net/json_sink.h"

namespace ors {
namespace {

struct RouteHandler final : json::Handler {
    core::CycleRoute* out = nullptr;
    bool sawRoute = false;
    bool haveAscent = false;
    bool haveDescent = false;
    char error[80] = {0};

    void onEnter(const json::Path& p, bool isArray) override {
        if (!isArray && p.depth() == 2 && p.is("routes[0]")) sawRoute = true;
    }

    void onScalar(const json::Path& p, json::Type t, const char* text, size_t len,
                  bool) override {
        // openrouteservice reports its own failures in an error object, with a
        // 200 in some cases.
        if (p.is("error.message") || p.is("error")) {
            if (t == json::Type::String && error[0] == '\0') {
                const size_t n = len < sizeof error - 1 ? len : sizeof error - 1;
                std::memcpy(error, text, n);
                error[n] = '\0';
            }
            return;
        }
        if (!p.startsWith("routes[0]")) return;

        bool okNum = false;
        const double v = json::toDouble(text, &okNum);
        if (!okNum) return;

        if (p.is("routes[0].summary.distance")) {
            out->distanceKm = static_cast<float>(v / 1000.0);   // metres in
        } else if (p.is("routes[0].summary.duration")) {
            out->durationMin = static_cast<int32_t>((v + 30.0) / 60.0);  // seconds in
        } else if (p.is("routes[0].ascent") || p.is("routes[0].summary.ascent")) {
            out->ascentM = static_cast<float>(v);
            haveAscent = true;
        } else if (p.is("routes[0].descent") || p.is("routes[0].summary.descent")) {
            out->descentM = static_cast<float>(v);
            haveDescent = true;
        }
    }
};

// Write a double with fixed decimals without printf's float support, which is
// disabled in some embedded builds. Six places is ~10 cm of longitude.
int fixed6(double v, char* out, size_t cap) {
    const bool neg = v < 0;
    if (neg) v = -v;
    const long whole = static_cast<long>(v);
    double frac = v - static_cast<double>(whole);
    long micros = static_cast<long>(frac * 1000000.0 + 0.5);
    long w = whole;
    if (micros >= 1000000) { micros -= 1000000; ++w; }
    return std::snprintf(out, cap, "%s%ld.%06ld", neg ? "-" : "", w, micros);
}

bool onEarth(double lat, double lon) {
    return lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0 &&
           (lat != 0.0 || lon != 0.0);
}

} // namespace

const char* statusName(Status s) {
    switch (s) {
        case Status::Ok:            return "ok";
        case Status::BadConfig:     return "bad config";
        case Status::Transport:     return "transport";
        case Status::KeyRejected:   return "key rejected";
        case Status::QuotaExceeded: return "quota";
        case Status::HttpError:     return "http error";
        case Status::BadResponse:   return "bad response";
        case Status::NoRoute:       return "no route";
    }
    return "?";
}

int buildRequestBody(const Query& q, char* out, size_t outCap) {
    if (out == nullptr || outCap == 0) return -1;
    if (!onEarth(q.fromLat, q.fromLon) || !onEarth(q.toLat, q.toLon)) return -1;

    char a[24], b[24], c[24], d[24];
    // LONGITUDE FIRST. Read that again before changing this line.
    if (fixed6(q.fromLon, a, sizeof a) < 0) return -1;
    if (fixed6(q.fromLat, b, sizeof b) < 0) return -1;
    if (fixed6(q.toLon,   c, sizeof c) < 0) return -1;
    if (fixed6(q.toLat,   d, sizeof d) < 0) return -1;

    // elevation:true is the whole reason this service is in the project.
    const int n = std::snprintf(out, outCap,
        "{\"coordinates\":[[%s,%s],[%s,%s]],\"elevation\":true,\"units\":\"m\"}",
        a, b, c, d);
    if (n < 0 || static_cast<size_t>(n) >= outCap) return -1;
    return n;
}

int buildUrl(const Config& cfg, char* out, size_t outCap) {
    if (out == nullptr || outCap == 0) return -1;
    if (cfg.baseUrl == nullptr || cfg.profile == nullptr || cfg.profile[0] == '\0')
        return -1;
    const int n = std::snprintf(out, outCap, "%s/%s/json", cfg.baseUrl, cfg.profile);
    if (n < 0 || static_cast<size_t>(n) >= outCap) return -1;
    return n;
}

Status Client::route(const Query& q, core::CycleRoute& out) {
    out = core::CycleRoute{};
    if (cfg_.apiKey == nullptr || cfg_.apiKey[0] == '\0') {
        setError("no openrouteservice key");
        return Status::BadConfig;
    }

    char url[192], body[192];
    if (buildUrl(cfg_, url, sizeof url) < 0) {
        setError("could not build the url");
        return Status::BadConfig;
    }
    if (buildRequestBody(q, body, sizeof body) < 0) {
        setError("coordinates are not usable");
        return Status::BadConfig;
    }

    RouteHandler h;
    core::CycleRoute scratch;
    h.out = &scratch;
    json::Reader rd(h);
    net::JsonSink sink(rd);

    const net::Header headers[] = {
        {"Authorization", cfg_.apiKey},
        {"Content-Type", "application/json"},
        {"Accept", "application/json"},
    };
    const net::Result r = http_->post(url, "application/json", body,
                                      std::strlen(body), headers, 3, sink);
    lastHttpStatus_ = r.status;

    if (r.error != net::Error::None && r.error != net::Error::Aborted) {
        setError(net::errorName(r.error));
        return Status::Transport;
    }
    if (r.status == 401 || r.status == 403) {
        setError("openrouteservice rejected the key");
        return Status::KeyRejected;
    }
    if (r.status == 429) {
        // The key is fine; the day's 2000 are gone. Worth its own status so the
        // caller can keep the cached route rather than treat it as a failure.
        setError("daily quota exhausted");
        return Status::QuotaExceeded;
    }
    if (!sink.finish()) {
        setError(rd.error());
        return Status::BadResponse;
    }
    if (r.status < 200 || r.status >= 300) {
        setError(h.error[0] ? h.error : "unexpected http status");
        return Status::HttpError;
    }
    if (!h.sawRoute) {
        setError(h.error[0] ? h.error : "no route to that place by bike");
        return Status::NoRoute;
    }

    // Elevation is the only reason we ask. A route without it is not a usable
    // answer, however well-formed: the comfort score would silently treat a
    // climb as flat, which is exactly the misleading-zero this project keeps
    // refusing to produce.
    if (!h.haveAscent || !h.haveDescent) {
        setError("no elevation in the response");
        return Status::BadResponse;
    }
    if (scratch.durationMin <= 0 || scratch.distanceKm <= 0.0f) {
        setError("route had no distance or duration");
        return Status::BadResponse;
    }

    scratch.valid = true;
    out = scratch;
    setError("");
    return Status::Ok;
}

} // namespace ors
