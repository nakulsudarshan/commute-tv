// ors_client.h - how hard is the bike ride, really.
//
// openrouteservice is in this project for exactly one thing: **elevation**.
// Distance and duration could come from anywhere; the ascent is what decides
// whether cycling to Melaten in August is a pleasant fifteen minutes or an
// arrival that needs a shower. `core::comfort` weighs it directly.
//
// ⚠️ COORDINATES ARE [LONGITUDE, LATITUDE].
//
// That is the opposite order to Google Maps, which is where every coordinate in
// this project was copied from. Transposed, Aachen (50.78 N, 6.05 E) becomes a
// point in Somalia, and the API answers with a routing failure rather than
// anything that looks like a coordinate problem. `tools/check_env.py` exists
// largely to catch this, and buildRequestBody() is asserted against the exact
// expected JSON for the same reason.
//
// Called rarely: a home/destination pair's route does not change, so the result
// is cached and steady-state usage is zero calls. The free tier allows 2000 a
// day, which this could not exhaust if it tried.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../core/textutil.h"

#include "../core/types.h"
#include "../net/http.h"

namespace ors {

struct Config {
    const char* apiKey = "";
    const char* baseUrl = "https://api.openrouteservice.org/v2/directions";

    // cycling-regular is the profile that matches a normal bike on normal
    // roads. cycling-road would flatter the estimate; cycling-electric would
    // make the ascent meaningless, which is the one number we are here for.
    const char* profile = "cycling-regular";
};

struct Query {
    double fromLat = 0.0, fromLon = 0.0;
    double toLat = 0.0, toLon = 0.0;
};

enum class Status : uint8_t {
    Ok,
    BadConfig,      // no key, no profile, a coordinate that is not on Earth
    Transport,      // could not reach the service
    KeyRejected,    // 401/403
    QuotaExceeded,  // 429 -- the key is fine, the day's allowance is not
    HttpError,
    BadResponse,    // malformed, or truncated
    NoRoute,        // it answered, and there is no way to cycle there
};

const char* statusName(Status s);

// The POST body. Exposed because the coordinate order is the thing most likely
// to be wrong, and a test that reads the literal JSON is the clearest way to
// keep it right.
int buildRequestBody(const Query& q, char* out, size_t outCap);

// The URL for a profile: <base>/<profile>/json
int buildUrl(const Config& cfg, char* out, size_t outCap);

class Client {
public:
    Client(const Config& cfg, net::Client& http) : cfg_(cfg), http_(&http) {}

    Status route(const Query& q, core::CycleRoute& out);

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

} // namespace ors
