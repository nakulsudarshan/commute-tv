// hafas_client.h - the piece between the URL builder and the parser.
//
// hafas_request.h knows how to phrase a question and hafas_parse.h knows how to
// read the answer; this puts a socket between them. Nothing more: the request
// shaping and the response reading are where the difficulty lives and both are
// already tested on their own.
//
// Two calls, and the difference matters:
//
//   trip()     coordinates -> itineraries. What the alert runs on.
//   resolve()  free text   -> a coordinate. What turns "Ponttor" in a calendar
//              entry into somewhere a journey can be planned to.
//
// resolve() is a guess and is treated as one: the caller should try
// core::PlaceBook first, because an exact coordinate always beats a search that
// might return a different Ponttor.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../core/textutil.h"
#include "../core/types.h"
#include "../net/http.h"
#include "hafas_parse.h"
#include "hafas_request.h"

namespace hafas {

enum class Status : uint8_t {
    Ok,
    BadConfig,     // no access id, or a query that cannot be phrased
    Transport,     // could not reach AVV
    Denied,        // the access id was refused
    HttpError,
    BadResponse,   // malformed, or the body stopped early
    NoResult,      // it answered, and there is nothing there
};

const char* statusName(Status s);

// One candidate for a piece of location text.
struct Place {
    char   name[core::kNameLen] = {0};
    char   extId[16] = {0};        // set for stops, empty for addresses and POIs
    double lat = 0.0, lon = 0.0;
    bool   isStop = false;         // a stop, rather than an address or POI
    bool valid() const { return lat != 0.0 || lon != 0.0; }
};

constexpr int kMaxPlaces = 4;

struct PlaceList {
    Place places[kMaxPlaces];
    uint8_t count = 0;
    uint8_t seen = 0;
    bool empty() const { return count == 0; }
};

class Client {
public:
    Client(const Endpoint& ep, net::Client& http) : ep_(ep), http_(&http) {}

    Status trip(const TripQuery& q, TripResult& out);
    Status resolve(const char* text, PlaceList& out, int maxNo = kMaxPlaces);

    int lastHttpStatus() const { return lastHttpStatus_; }
    const char* lastError() const { return lastError_; }

    // AVV's own error code when it sent one, e.g. "API_AUTH". Worth keeping
    // separate: a 401 with API_AUTH is a wrong key, while a 401 without one is
    // usually a proxy in front of it.
    const char* lastAvvError() const { return avvError_; }

private:
    Status run(const char* url, json::Handler& h, json::Reader& rd);
    void setError(const char* s) {
        core::copyStr(lastError_, sizeof lastError_, s ? s : "",
                      s ? std::strlen(s) : 0);
    }

    Endpoint ep_;
    net::Client* http_;
    int lastHttpStatus_ = 0;
    char lastError_[96] = {0};
    char avvError_[24] = {0};
};

} // namespace hafas
