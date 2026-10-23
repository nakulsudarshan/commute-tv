#include "hafas_client.h"

#include <cstdio>

#include "../net/json_sink.h"

namespace hafas {
namespace {

struct PlaceHandler final : json::Handler {
    PlaceList* out = nullptr;
    int maxWanted = kMaxPlaces;

    Place cur{};
    bool inEntry = false;
    bool isStop = false;
    char errorCode[24] = {0};

    void onEnter(const json::Path& p, bool isArray) override {
        if (isArray) return;
        // Each entry is a one-key wrapper: {"StopLocation": {...}} or
        // {"CoordLocation": {...}}. The union name is how the two are told
        // apart -- an address has no extId and cannot be departed from.
        if (p.depth() == 3 && p.startsWith("stopLocationOrCoordLocation[*]")) {
            const char* kind = p.key(2);
            if (std::strcmp(kind, "StopLocation") == 0) { inEntry = true; isStop = true; }
            else if (std::strcmp(kind, "CoordLocation") == 0) { inEntry = true; isStop = false; }
            else return;
            cur = Place{};
            if (out->seen < 255) ++out->seen;
        }
    }

    void onLeave(const json::Path& p, bool isArray) override {
        if (isArray || !inEntry) return;
        if (!(p.depth() == 3 && p.startsWith("stopLocationOrCoordLocation[*]"))) return;
        inEntry = false;
        if (!cur.valid() || cur.name[0] == '\0') return;
        cur.isStop = isStop;
        if (out->count < kMaxPlaces && out->count < maxWanted) {
            out->places[out->count] = cur;
            ++out->count;
        }
    }

    void onScalar(const json::Path& p, json::Type t, const char* text, size_t len,
                  bool) override {
        if (p.is("errorCode")) {
            core::copyStr(errorCode, sizeof errorCode, text, len);
            return;
        }
        if (!inEntry || p.depth() != 4) return;
        const char* f = p.key(3);
        if (std::strcmp(f, "name") == 0) {
            // Folded here for the same reason as everywhere else: the display
            // font has no glyph for an umlaut.
            core::foldInto(cur.name, sizeof cur.name, text, len);
        } else if (std::strcmp(f, "extId") == 0) {
            core::copyStr(cur.extId, sizeof cur.extId, text, len);
        } else if (t == json::Type::Number) {
            bool ok = false;
            const double v = json::toDouble(text, &ok);
            if (!ok) return;
            if (std::strcmp(f, "lat") == 0) cur.lat = v;
            else if (std::strcmp(f, "lon") == 0) cur.lon = v;
        }
    }
};

// The envelope carries errorCode even on a 200, so a trip parse that came back
// clean can still be an error document.
struct ErrorPeek final : json::Handler {
    char code[24] = {0};
    void onScalar(const json::Path& p, json::Type, const char* text, size_t len,
                  bool) override {
        if (p.is("errorCode")) core::copyStr(code, sizeof code, text, len);
    }
};

} // namespace

const char* statusName(Status s) {
    switch (s) {
        case Status::Ok:          return "ok";
        case Status::BadConfig:   return "bad config";
        case Status::Transport:   return "transport";
        case Status::Denied:      return "denied";
        case Status::HttpError:   return "http error";
        case Status::BadResponse: return "bad response";
        case Status::NoResult:    return "no result";
    }
    return "?";
}

Status Client::run(const char* url, json::Handler& h, json::Reader& rd) {
    (void)h;
    net::JsonSink sink(rd);
    const net::Header headers[] = {{"Accept", "application/json"}};
    const net::Result r = http_->get(url, headers, 1, sink);
    lastHttpStatus_ = r.status;

    if (r.error != net::Error::None && r.error != net::Error::Aborted) {
        setError(net::errorName(r.error));
        return Status::Transport;
    }
    // Parse first even on an error status: AVV's error documents are where
    // errorCode lives, and "API_AUTH" is the difference between a wrong key and
    // a proxy problem.
    const bool parsed = sink.finish();
    if (r.status == 401 || r.status == 403) {
        setError("AVV refused the access id");
        return Status::Denied;
    }
    if (!parsed) {
        setError(rd.error());
        return Status::BadResponse;
    }
    if (r.status < 200 || r.status >= 300) {
        setError("unexpected http status");
        return Status::HttpError;
    }
    setError("");
    return Status::Ok;
}

Status Client::trip(const TripQuery& q, TripResult& out) {
    out = TripResult{};
    avvError_[0] = '\0';
    if (ep_.accessId == nullptr || ep_.accessId[0] == '\0') {
        setError("no AVV access id");
        return Status::BadConfig;
    }

    char url[640];
    if (buildTripUrl(ep_, q, url, sizeof url) < 0) {
        setError("could not build the trip url");
        return Status::BadConfig;
    }

    TripParser tp(out);
    // TripParser owns its reader, so the shared run() helper cannot drive it.
    // Feeding it directly costs a few lines and keeps the streaming parser the
    // single place that knows how a trip response is shaped.
    struct Sink final : net::BodySink {
        TripParser* tp;
        bool write(const char* d, size_t n) override { return tp->feed(d, n); }
    } sink;
    sink.tp = &tp;

    const net::Header headers[] = {{"Accept", "application/json"}};
    const net::Result r = http_->get(url, headers, 1, sink);
    lastHttpStatus_ = r.status;

    if (r.error != net::Error::None && r.error != net::Error::Aborted) {
        setError(net::errorName(r.error));
        return Status::Transport;
    }
    const bool parsed = tp.finish();
    core::copyStr(avvError_, sizeof avvError_, out.env.errorCode,
                  std::strlen(out.env.errorCode));

    if (r.status == 401 || r.status == 403 || out.env.hasError()) {
        setError(out.env.errorText[0] ? out.env.errorText : "AVV refused the request");
        return Status::Denied;
    }
    if (!parsed) {
        setError("the trip response stopped early");
        out = TripResult{};        // never hand back a half-read itinerary list
        return Status::BadResponse;
    }
    if (r.status < 200 || r.status >= 300) {
        setError("unexpected http status");
        out = TripResult{};
        return Status::HttpError;
    }
    if (out.empty()) {
        setError("no itineraries");
        return Status::NoResult;
    }
    setError("");
    return Status::Ok;
}

Status Client::resolve(const char* text, PlaceList& out, int maxNo) {
    out = PlaceList{};
    avvError_[0] = '\0';
    if (ep_.accessId == nullptr || ep_.accessId[0] == '\0') {
        setError("no AVV access id");
        return Status::BadConfig;
    }
    if (text == nullptr || text[0] == '\0') {
        setError("nothing to resolve");
        return Status::BadConfig;
    }
    if (maxNo < 1) maxNo = 1;
    if (maxNo > kMaxPlaces) maxNo = kMaxPlaces;

    char url[512];
    if (buildLocationNameUrl(ep_, text, maxNo, url, sizeof url) < 0) {
        setError("could not build the location url");
        return Status::BadConfig;
    }

    PlaceHandler h;
    h.out = &out;
    h.maxWanted = maxNo;
    json::Reader rd(h);
    const Status s = run(url, h, rd);
    if (h.errorCode[0]) core::copyStr(avvError_, sizeof avvError_, h.errorCode,
                                      std::strlen(h.errorCode));
    if (s != Status::Ok) {
        out = PlaceList{};
        return s;
    }
    if (out.empty()) {
        setError("nowhere by that name");
        return Status::NoResult;
    }
    return Status::Ok;
}

} // namespace hafas
