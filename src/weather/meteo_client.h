// meteo_client.h - the weather at the moment you would be cycling.
//
// Not the weather now. An alert opens thirty minutes before departure, and the
// question the comfort score is really asking is "what will it be like when I
// am actually on the bike" -- which may be an hour away and, in Aachen, quite
// different.
//
// Open-Meteo takes `start_hour`/`end_hour`, so the request asks for exactly the
// hour in question and the response carries one value per variable. That keeps
// the response to a few hundred bytes and removes the only fiddly part of the
// parse, which would otherwise be finding the right index in three parallel
// arrays.
//
// No API key, and plain HTTP on purpose: this is a public forecast, there is
// nothing to protect, and skipping TLS saves the handshake and its memory on a
// board that has to hold a JPEG at the same time.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../core/textutil.h"

#include "../core/timeutil.h"
#include "../core/types.h"
#include "../net/http.h"

namespace meteo {

struct Config {
    const char* baseUrl = "http://api.open-meteo.com/v1/forecast";

    // Asking for the calendar's own zone means the times that come back are
    // local wall clock, exactly like HAFAS and Google Calendar. Nothing in this
    // firmware converts a timezone anywhere.
    const char* timeZone = "Europe/Berlin";
};

struct Query {
    double lat = 0.0, lon = 0.0;
    core::Timestamp at;      // the hour you would be riding
};

enum class Status : uint8_t {
    Ok,
    BadConfig,
    Transport,
    HttpError,
    BadResponse,
    NoData,        // it answered, and had nothing for that hour
};

const char* statusName(Status s);

int buildUrl(const Config& cfg, const Query& q, char* out, size_t outCap);

class Client {
public:
    Client(const Config& cfg, net::Client& http) : cfg_(cfg), http_(&http) {}

    Status fetch(const Query& q, core::Weather& out);

    int lastHttpStatus() const { return lastHttpStatus_; }
    const char* lastError() const { return lastError_; }

private:
    Config cfg_;
    net::Client* http_;
    int lastHttpStatus_ = 0;
    // Copied, never aliased. An earlier version pointed lastError_ straight at
    // a char[] inside a handler that lived on the stack of the call -- the
    // message read fine in the non-sanitised build and was a use-after-return
    // the moment anyone actually looked at it. A buffer makes that impossible
    // rather than merely unlikely.
    void setError(const char* s) {
        core::copyStr(lastError_, sizeof lastError_, s ? s : "",
                      s ? std::strlen(s) : 0);
    }

    char lastError_[96] = {0};
};

} // namespace meteo
