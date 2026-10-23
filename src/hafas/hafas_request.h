// hafas_request.h - build AVV HAFAS ReST URLs.
//
// Pure string construction: no networking, no Arduino, so it is fully testable
// on the host. The client layer only has to perform the GET.
//
// Base URLs (from AVV):
//   demo: https://avv.demo.hafas.de/restproxy/
//   prod: https://auskunft.avv.de/restproxy/
//
// Note the demo system is documented as unavailable outside German office
// hours, which is why fixtures exist.
#pragma once
#include <cstddef>
#include <cstdint>
#include "../core/timeutil.h"
#include "../core/urlencode.h"

namespace hafas {

enum class Env : uint8_t { Demo, Prod };

struct Endpoint {
    Env env = Env::Demo;
    const char* accessId = "";
    const char* baseUrlOverride = nullptr;  // for tests / self-hosted proxies
    const char* baseUrl() const;
};

struct TripQuery {
    double originLat = 0, originLon = 0;
    double destLat = 0,   destLon = 0;

    // Local date/time. With searchForArrival the pair means "arrive by".
    int32_t dateDays = 0;      // days since epoch, from core::parseDate
    int32_t timeOfDayMin = 0;  // minutes past midnight

    bool searchForArrival = true;
    int numF = 3;              // HAFAS caps this at 6
    bool originWalk = true;
    bool destWalk = true;
    const char* rtMode = "REALTIME";

    // Response-size switches, all off by default. Measured against a real AVV
    // response (3 trips, Aachen, 22 Aug 2026): the reply was 89 KB compact, of
    // which Leg.Freq alone was 46 KB -- 52% of the payload, describing other
    // departures of the same line, which this project never shows. TariffResult
    // was another 4%.
    //
    // withFreq defaults to 1 server-side (spec 2.12, since 2.18) and tariff's
    // default is "configurable via provisioning", i.e. whatever AVV chose. Both
    // are therefore sent explicitly rather than assumed.
    bool withFreq = false;
    bool tariff = false;
    bool trafficMessages = false;
};

// Percent-encodes into out. Returns the number of chars written (excluding
// the terminator), or -1 if the buffer was too small.
//
// One implementation, in core, shared with the SmallTV client. Kept visible
// here because the HAFAS tests exercise it as part of URL construction.
inline int urlEncode(const char* in, char* out, size_t outSize) {
    return core::urlEncode(in, out, outSize);
}

// Writes a double with `decimals` places without pulling in printf's float
// support, which is disabled in some embedded builds.
int formatFixed(double v, int decimals, char* out, size_t outSize);

// Builds the full trip URL. Returns length written, or -1 if it did not fit.
int buildTripUrl(const Endpoint& ep, const TripQuery& q, char* out, size_t outSize);

// Location search by free text (used to resolve calendar location strings).
int buildLocationNameUrl(const Endpoint& ep, const char* input, int maxNo,
                         char* out, size_t outSize);

// Live vehicle position for a journey, keyed by the leg's JourneyDetailRef.
int buildJourneyPosUrl(const Endpoint& ep, const char* journeyRef,
                       char* out, size_t outSize);

} // namespace hafas
