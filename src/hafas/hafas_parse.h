// hafas_parse.h - turn an AVV HAFAS `trip` response into core::Itinerary.
//
// Streaming, by construction: bytes go in as they come off the socket and only
// the fields we display come out. Nothing here allocates, and the response is
// never held whole -- see json/json_stream.h for why that matters on a board
// whose largest contiguous allocation is 110 KB.
//
// WHAT THE REAL RESPONSES LOOK LIKE
// ---------------------------------
// Every shape below was read out of the recorded fixtures in `fixtures/`, not
// out of the XSD -- which is version 2.49.0 while the server reports 2.51.0.
//
//   * `Leg.type` is the string "WALK" or "JNY". There is no mode enum.
//   * `Leg.Product` is a JSON ARRAY (length 1 on every leg observed, walk legs
//     included), so the fields live at Product[0]. The object form is accepted
//     too, in case a future dialect flattens it.
//   * Times are `Origin.time` / `Origin.rtTime` -- NOT `rtDepTime`/`rtArrTime`,
//     which belong to the departure- and arrival-board services.
//   * `rtTime` is absent on some legs of a realtime response and present on
//     others. Falling back to planned is the normal case, not the exception.
//   * `cancelled` is absent when nothing is cancelled; `reachable` appears only
//     on JNY legs.
//   * `line` is not always numeric: the on-demand shuttle's line is the string
//     "Netliner".
//   * Walk legs name their endpoints as raw coordinate strings
//     ("50.784294, 6.052599"), which is why the display takes the boarding stop
//     from the first transit leg.
//
// A 200 response can still carry errorCode/errorText, so the envelope is
// always read and hasError() always checked.
#pragma once
#include <cstddef>
#include <cstdint>

#include "../core/types.h"
#include "../json/json_stream.h"

namespace hafas {

// The attributes every HAFAS service response carries (CommonResponseType,
// rest-2.49.0.xsd line 48).
struct Envelope {
    char serverVersion[16] = {0};
    char dialectVersion[16] = {0};

    // When HAFAS last updated its realtime picture. A better staleness signal
    // than our own fetch time: it says how fresh the upstream data is, not how
    // recently we asked.
    core::Timestamp planRtTs;

    char errorCode[24] = {0};
    char errorText[96] = {0};

    bool hasError() const { return errorCode[0] != '\0'; }
};

struct TripResult {
    Envelope env;

    core::Itinerary trips[core::kMaxCandidates];
    uint8_t tripCount = 0;      // itineraries actually stored

    // Diagnostics. All three being zero is the normal case; a non-zero value is
    // worth showing on the health panel, because it means the response held
    // more than we can represent and the choice we made may not be the best
    // one available.
    uint8_t tripsSeen = 0;      // itineraries present in the response
    uint8_t tripsDropped = 0;   // discarded: more legs than kMaxLegs
    // A stop name or line was longer than its field. Deliberately not set for
    // `direction`, which is a route description and overflows on ordinary
    // responses -- a flag that is always true tells you nothing.
    bool textTruncated = false;

    bool empty() const { return tripCount == 0; }
};

// Feed the body in whatever chunks arrive; call finish() at the end.
//
// finish() returning false means the document was malformed or cut short. That
// is not the same as "no connections", and the two must not be conflated: a
// truncated body with two complete trips in it would otherwise look like a
// successful answer that happens to be missing the best option.
class TripParser final : private json::Handler {
public:
    explicit TripParser(TripResult& out) : out_(&out), rd_(*this) {}

    bool feed(const char* data, size_t n) { return rd_.feed(data, n); }
    bool finish();

    bool failed() const { return rd_.failed(); }
    const char* error() const { return rd_.error(); }

private:
    void onScalar(const json::Path& p, json::Type t, const char* text,
                  size_t len, bool truncated) override;
    void onEnter(const json::Path& p, bool isArray) override;
    void onLeave(const json::Path& p, bool isArray) override;

    void beginTrip(int idx);
    void endTrip();
    void beginLeg(int idx);
    void endLeg();

    void envelopeField(const char* key, const char* text, size_t len);
    void tripField(const char* key, json::Type t, const char* text);
    void legField(const json::Path& p, json::Type t, const char* text, size_t len);

    TripResult* out_;
    json::Reader rd_;

    core::Itinerary cur_{};      // the trip being assembled
    bool inTrip_ = false;
    bool legOverflow_ = false;
    bool anyLegCancelled_ = false;
    bool allReachable_ = true;

    core::Leg leg_{};            // the leg being assembled
    bool inLeg_ = false;

    // Date and time arrive as separate members and in no guaranteed order, so
    // they are held until the leg closes and combined then.
    struct StopTimes {
        char date[12], time[12], rtDate[12], rtTime[12];
    };
    StopTimes org_{}, dst_{};

    char catOut_[20] = {0};
    char catOutS_[12] = {0};
    char prodLine_[20] = {0};
    char prodDisplayNum_[20] = {0};
    char legName_[28] = {0};
    bool isWalk_ = false;
};

// Convenience for a body already in memory (tests, and any transport that
// hands over the whole thing). Returns false on a malformed or truncated
// document; `err` receives the reason when given.
bool parseTripResponse(const char* body, size_t len, TripResult& out,
                       const char** err = nullptr);

// The mode a HAFAS product category maps to. Exposed for testing, and because
// the table is the kind of thing that needs to be looked at rather than buried.
core::LegMode legModeFor(const char* catOut, const char* catOutS);

} // namespace hafas
