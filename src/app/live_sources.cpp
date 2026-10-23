#include "live_sources.h"

#include <cstdio>
#include <cstring>

namespace app {
namespace {

// A refusal is the far end saying no to US -- a key, a quota, a denial -- as
// opposed to us failing to reach it. The orchestrator backs off hard on the
// first and retries the second, and getting this mapping wrong is how an API
// key gets banned.
// A 24-character note is what reaches the OLED. Spending four of them on the
// HTTP status is the best trade available: "ors http" says something broke,
// "ors http 400" says which half of the system to look at.
Outcome withStatus(const char* prefix, int http, bool refused) {
    char n[24];
    if (http > 0) std::snprintf(n, sizeof n, "%s %d", prefix, http);
    else std::snprintf(n, sizeof n, "%s", prefix);
    return Outcome::fail(n, refused);
}

Outcome fromGoogle(google::Status s, const char* detail) {
    switch (s) {
        case google::Status::Ok:          return Outcome::good();
        case google::Status::AuthFailed:  return Outcome::fail("auth refused", true);
        case google::Status::BadConfig:   return Outcome::fail("cal config", true);
        case google::Status::Transport:   return Outcome::fail("no reply");
        case google::Status::HttpError:   return Outcome::fail(detail[0] ? detail : "http");
        case google::Status::BadResponse: return Outcome::fail("bad reply");
    }
    return Outcome::fail("cal?");
}

Outcome fromHafas(hafas::Status s, const char* detail) {
    switch (s) {
        case hafas::Status::Ok:          return Outcome::good();
        case hafas::Status::NoResult:    return Outcome::good();   // an answer, not a failure
        case hafas::Status::Denied:      return Outcome::fail("avv denied", true);
        case hafas::Status::BadConfig:   return Outcome::fail("avv config", true);
        case hafas::Status::Transport:   return Outcome::fail("avv no reply");
        case hafas::Status::HttpError:   return Outcome::fail(detail[0] ? detail : "avv http");
        case hafas::Status::BadResponse: return Outcome::fail("avv bad reply");
    }
    return Outcome::fail("avv?");
}

int32_t daysOf(core::Timestamp t) { return t.min / 1440; }
int32_t minuteOf(core::Timestamp t) { return t.min % 1440; }

} // namespace

void LiveSources::note(const char* what, int http, const char* detail) {
    if (!d_.log) return;
    char buf[140];
    std::snprintf(buf, sizeof buf, "%s: http %d%s%s", what, http,
                  (detail && detail[0]) ? " -- " : "", detail ? detail : "");
    d_.log(buf);
}

Outcome LiveSources::events(const Clock& now, int32_t lookaheadMin,
                            google::EventList& out) {
    if (!d_.calendar) return Outcome::fail("no calendar", true);

    const google::Status tok = d_.calendar->ensureToken(now.utcSec);
    if (tok != google::Status::Ok) {
        note("google token", d_.calendar->lastHttpStatus(), d_.calendar->lastError());
        return fromGoogle(tok, d_.calendar->lastError());
    }

    google::Query q;
    q.from = now.local;
    q.to = core::addMinutes(now.local, lookaheadMin);
    q.utcOffsetMin = now.utcOffsetMin;
    q.maxResults = google::kMaxEvents;

    const google::Status s = d_.calendar->listEvents(q, now.utcSec, out);
    if (s != google::Status::Ok) {
        note("calendar", d_.calendar->lastHttpStatus(), d_.calendar->lastError());
    } else if (d_.log) {
        char buf[100];
        std::snprintf(buf, sizeof buf,
                      "calendar: %u seen, %u usable (no location %u, all-day %u)",
                      static_cast<unsigned>(out.seen), static_cast<unsigned>(out.count),
                      static_cast<unsigned>(out.skippedNoLocation),
                      static_cast<unsigned>(out.skippedAllDay));
        d_.log(buf);
    }
    return fromGoogle(s, d_.calendar->lastError());
}

Outcome LiveSources::resolve(const char* text, double& lat, double& lon) {
    if (!d_.transit) return Outcome::fail("no transit", true);

    hafas::PlaceList list;
    const hafas::Status s = d_.transit->resolve(text, list);
    const Outcome o = fromHafas(s, d_.transit->lastError());
    if (!o.ok) {
        note("resolve", d_.transit->lastHttpStatus(), d_.transit->lastError());
        return o;
    }
    // Prefer an actual stop: a StopLocation can be departed from, an address
    // cannot, and conflating them sends the journey to the wrong kind of thing.
    const hafas::Place* best = nullptr;
    for (uint8_t i = 0; i < list.count; ++i) {
        const hafas::Place& p = list.places[i];
        if (!p.valid()) continue;
        if (p.isStop) { best = &p; break; }
        if (!best) best = &p;
    }

    // ⚠️ SAY WHICH ONE IT PICKED, not merely how many there were. A free-text
    // search that fuzzily matches a shop name to an unrelated stop fails
    // SILENTLY -- the journey plans perfectly, to the wrong place. The only way
    // that is ever noticed is if the log names the thing it chose.
    if (d_.log) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "resolve \"%s\": %u seen, %u usable -> %s%s",
                      text, static_cast<unsigned>(list.seen),
                      static_cast<unsigned>(list.count),
                      best ? best->name : "nothing",
                      best ? (best->isStop ? " (stop)" : " (address)") : "");
        d_.log(buf);
    }

    // Nothing there is an ANSWER, and a different one from "we could not ask".
    // ensurePlace() retries a Google Maps business name with the street line
    // underneath it, and only this tells it that retrying is worth anything.
    if (!best) return Outcome::missing("place?");

    lat = best->lat;
    lon = best->lon;
    return Outcome::good();
}

Outcome LiveSources::trip(const TripRequest& q, core::Itinerary* out, int cap,
                          int& count) {
    count = 0;
    if (!d_.transit) return Outcome::fail("no transit", true);
    if (!q.arriveBy.valid()) return Outcome::fail("no event time");

    hafas::TripQuery tq;
    tq.originLat = q.fromLat;
    tq.originLon = q.fromLon;
    tq.destLat = q.toLat;
    tq.destLon = q.toLon;
    tq.dateDays = daysOf(q.arriveBy);
    tq.timeOfDayMin = minuteOf(q.arriveBy);
    tq.searchForArrival = true;      // "be there by", not "leave at"
    tq.numF = core::kMaxCandidates;

    tripScratch_ = hafas::TripResult{};
    const hafas::Status s = d_.transit->trip(tq, tripScratch_);
    const Outcome o = fromHafas(s, d_.transit->lastError());
    if (!o.ok) {
        note("trip", d_.transit->lastHttpStatus(), d_.transit->lastError());
        return o;
    }
    if (d_.log) {
        char buf[110];
        std::snprintf(buf, sizeof buf, "trip: %u itinerar%s (%u dropped, too many legs)",
                      static_cast<unsigned>(tripScratch_.tripCount),
                      tripScratch_.tripCount == 1 ? "y" : "ies",
                      static_cast<unsigned>(tripScratch_.tripsDropped));
        d_.log(buf);
    }

    for (uint8_t i = 0; i < tripScratch_.tripCount && count < cap; ++i) {
        out[count++] = tripScratch_.trips[i];
    }
    return Outcome::good();
}

Outcome LiveSources::cycle(const TripRequest& q, core::CycleRoute& out) {
    out = core::CycleRoute{};
    if (!d_.cycling) return Outcome::fail("no ors", true);

    ors::Query oq;
    oq.fromLat = q.fromLat;
    oq.fromLon = q.fromLon;
    oq.toLat = q.toLat;
    oq.toLon = q.toLon;

    const ors::Status s = d_.cycling->route(oq, out);
    switch (s) {
        case ors::Status::Ok:
            return Outcome::good();
        case ors::Status::NoRoute:
            // A real answer: there is no way to cycle there. Reporting it as a
            // failure would have the orchestrator ask again for ever.
            out.valid = false;
            return Outcome::good();
        case ors::Status::KeyRejected:
            note("ors", d_.cycling->lastHttpStatus(), d_.cycling->lastError());
            return withStatus("ors key", d_.cycling->lastHttpStatus(), true);
        case ors::Status::QuotaExceeded:
            note("ors", d_.cycling->lastHttpStatus(), d_.cycling->lastError());
            return Outcome::fail("ors quota", true);
        case ors::Status::BadConfig:     return Outcome::fail("ors config", true);
        case ors::Status::Transport:
            note("ors", 0, d_.cycling->lastError());
            return Outcome::fail("ors no reply");
        case ors::Status::HttpError:
            note("ors", d_.cycling->lastHttpStatus(), d_.cycling->lastError());
            return withStatus("ors http", d_.cycling->lastHttpStatus(), false);
        case ors::Status::BadResponse:
            note("ors", d_.cycling->lastHttpStatus(), d_.cycling->lastError());
            return Outcome::fail("ors bad reply");
    }
    return Outcome::fail("ors?");
}

Outcome LiveSources::weather(double lat, double lon, core::Timestamp at,
                             core::Weather& out) {
    out = core::Weather{};
    if (!d_.forecast) return Outcome::fail("no meteo", true);

    meteo::Query q;
    q.lat = lat;
    q.lon = lon;
    q.at = at;

    const meteo::Status s = d_.forecast->fetch(q, out);
    switch (s) {
        case meteo::Status::Ok:
            return Outcome::good();
        case meteo::Status::NoData:
            // No forecast for that hour is not a fault. computeComfort() already
            // knows that missing weather must not be read as perfect weather.
            out.valid = false;
            return Outcome::good();
        case meteo::Status::BadConfig:   return Outcome::fail("wx config", true);
        case meteo::Status::Transport:
            note("weather", 0, d_.forecast->lastError());
            return Outcome::fail("wx no reply");
        case meteo::Status::HttpError:
            note("weather", d_.forecast->lastHttpStatus(), d_.forecast->lastError());
            return withStatus("wx http", d_.forecast->lastHttpStatus(), false);
        case meteo::Status::BadResponse:
            note("weather", d_.forecast->lastHttpStatus(), d_.forecast->lastError());
            return Outcome::fail("wx bad reply");
    }
    return Outcome::fail("wx?");
}

Outcome LiveSources::pingScreen(ScreenState& out) {
    out = ScreenState{};
    if (!d_.tv) return Outcome::fail("no tv", true);

    if (!d_.tv->ping()) return Outcome::fail(smalltv::errorText(d_.tv->lastError()));
    out.reachable = true;

    uint32_t freeBytes = 0;
    if (d_.tv->readFreeSpace(freeBytes)) {
        out.freeKnown = true;
        out.freeKb = freeBytes / 1024;
    }
    // A file of ours on the device while we are not showing anything. On a
    // ~1 MB partition this is the thing that eventually stops everything.
    out.imageLeftBehind = d_.tv->imageOnDevice() && !d_.tv->holdsScreen();
    return Outcome::good();
}

Outcome LiveSources::showScreen(const render::ScreenModel& m) {
    if (!d_.tv) return Outcome::fail("no tv", true);
    if (!d_.bandBuf || !d_.jpegBuf || d_.bandRows <= 0) {
        return Outcome::fail("no frame buf", true);
    }

    render::BandRenderer src(d_.bandBuf, render::kScreenW, render::kScreenH,
                             d_.bandRows, render::renderCommuteBand, &m);

    jpeg::BufferSink sink(d_.jpegBuf, d_.jpegCap);
    jpeg::Options opt;
    opt.quality = d_.jpegQuality;
    opt.sampling = jpeg::Sampling::S444;   // 4:2:0 softens small coloured text

    const int used = jpeg::encodeRgb565WithBudget(src, render::kScreenW,
                                                  render::kScreenH, opt,
                                                  d_.maxJpegBytes,
                                                  d_.jpegMinQuality, sink);
    if (used <= 0 || sink.overflowed()) return Outcome::fail("encode failed");
    lastJpegBytes_ = sink.size();
    lastJpegQuality_ = used;

    if (!d_.tv->takeScreen(sink.data(), sink.size())) {
        return Outcome::fail(smalltv::errorText(d_.tv->lastError()));
    }
    return Outcome::good();
}

Outcome LiveSources::releaseScreen() {
    if (!d_.tv) return Outcome::fail("no tv", true);
    if (!d_.tv->releaseScreen()) {
        return Outcome::fail(smalltv::errorText(d_.tv->lastError()));
    }
    return Outcome::good();
}

} // namespace app
