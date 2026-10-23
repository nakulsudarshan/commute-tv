#include "test.h"
#include "../src/hafas/hafas_parse.h"
#include "../src/core/timeutil.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

using core::LegMode;

namespace {

// A trimmed but faithful excerpt of a real AVV trip response. Every quirk here
// was observed in `fixtures/trip_arrival_realtime.json`: Product as an array,
// times under Origin.time / Origin.rtTime, realtime present on some legs and
// absent on others, a non-numeric line, umlauts in stop names, and a
// JourneyDetail carrying its own `time` fields at a deeper level.
//
// It is embedded rather than loaded because the recorded fixtures are not
// committed (they are keyed to one person's home address), and the parser must
// still be covered on a fresh checkout. The real files are additionally parsed
// further down when they exist.
const char* kTripDoc = R"JSON({
  "Trip": [
    {
      "idx": 0,
      "tripId": "C-0",
      "duration": "PT34M",
      "rtDuration": "PT36M",
      "transferCount": 1,
      "ctxRecon": "T$A=1@O=Aachen@L=1431@a=128@$A=1@O=Laurensberg@L=1501@$202608221926$202608221930$Netliner$$1$$$$$$",
      "LegList": {
        "Leg": [
          {
            "idx": "0", "name": "Fußweg", "type": "WALK", "duration": "PT8M",
            "Origin": { "name": "50.775300, 6.083900", "type": "ADR",
                        "time": "19:18:00", "date": "2026-08-22" },
            "Destination": { "name": "Aachen, Hörn Brücke", "type": "ST",
                             "time": "19:26:00", "date": "2026-08-22" },
            "Product": [ { "name": "Fußweg", "internalName": "Fußweg" } ]
          },
          {
            "idx": "1", "name": "Netliner", "type": "JNY",
            "direction": "Netliner Aachener Norden", "reachable": true,
            "Origin": { "name": "Aachen, Hörn Brücke", "type": "ST",
                        "prognosisType": "PROGNOSED",
                        "time": "19:26:00", "date": "2026-08-22",
                        "rtTime": "19:28:00", "rtDate": "2026-08-22" },
            "Destination": { "name": "Aachen, Laurensberg", "type": "ST",
                             "time": "19:30:00", "date": "2026-08-22",
                             "rtTime": "19:32:00", "rtDate": "2026-08-22" },
            "Product": [ { "name": "Netliner", "displayNumber": "Netliner",
                           "line": "Netliner", "catOut": "Netliner",
                           "catOutS": "NET", "cls": "512" } ],
            "Notes": { "Note": [ { "key": "TW", "value": "no wheelchair access" } ] },
            "JourneyDetail": {
              "Stops": { "Stop": [
                { "name": "Aachen, Hörn Brücke", "time": "19:26:00", "date": "2026-08-22" },
                { "name": "Aachen, Kullenhofstraße", "time": "19:28:00", "date": "2026-08-22" },
                { "name": "Aachen, Laurensberg", "time": "19:30:00", "date": "2026-08-22" }
              ] }
            }
          },
          {
            "idx": "2", "name": "Bus 47", "type": "JNY",
            "direction": "Hüls Gewerbegebiet", "reachable": true,
            "Origin": { "name": "Aachen, Laurensberg", "type": "ST",
                        "platform": { "type": "X", "text": "H.1" },
                        "time": "19:35:00", "date": "2026-08-22" },
            "Destination": { "name": "Aachen, Scheibenstraße", "type": "ST",
                             "time": "19:52:00", "date": "2026-08-22" },
            "Product": [ { "name": "Bus 47", "displayNumber": "47", "line": "47",
                           "catOut": "Bus", "catOutS": "NFB", "cls": "128" } ]
          }
        ]
      }
    },
    {
      "idx": 1, "transferCount": 1,
      "LegList": { "Leg": [
        { "idx": "0", "type": "WALK",
          "Origin": { "name": "50.775300, 6.083900", "time": "19:40:00", "date": "2026-08-22" },
          "Destination": { "name": "Aachen, Ponttor", "time": "19:48:00", "date": "2026-08-22" },
          "Product": [ { "name": "Fußweg" } ] },
        { "idx": "1", "type": "JNY", "reachable": false, "direction": "Brand",
          "Origin": { "name": "Aachen, Ponttor", "time": "19:50:00", "date": "2026-08-22" },
          "Destination": { "name": "Aachen, Rothe Erde", "time": "20:11:00", "date": "2026-08-22" },
          "Product": [ { "name": "Bus 45", "line": "45", "catOut": "Bus", "catOutS": "NFB" } ] }
      ] }
    },
    {
      "idx": 2, "transferCount": 0,
      "LegList": { "Leg": [
        { "idx": "0", "type": "JNY", "reachable": true, "cancelled": true, "direction": "Eilendorf",
          "Origin": { "name": "Aachen, Ponttor", "time": "20:05:00", "date": "2026-08-22" },
          "Destination": { "name": "Aachen, Rothe Erde", "time": "20:26:00", "date": "2026-08-22" },
          "Product": [ { "name": "RB20", "line": "RB20", "catOut": "RB", "catOutS": "NRB" } ] }
      ] }
    },
    {
      "idx": 3, "transferCount": 0,
      "LegList": { "Leg": [
        { "idx": "0", "type": "JNY", "reachable": true, "direction": "Nachtbus",
          "Origin": { "name": "Aachen, Bushof", "time": "23:50:00", "date": "2026-08-22" },
          "Destination": { "name": "Aachen, Brand", "time": "24:05:00", "date": "2026-08-22" },
          "Product": [ { "name": "Bus N1", "line": "N1", "catOut": "Bus", "catOutS": "NFB" } ] }
      ] }
    }
  ],
  "serverVersion": "2.51.0",
  "dialectVersion": "2.51",
  "planRtTs": "2026-08-22T18:46:45.000+02:00",
  "requestId": "default-request-id",
  "scrB": "1|OB|MT#14", "scrF": "1|OF|MT#14"
})JSON";

// --- helpers ---------------------------------------------------------------

bool allPrintableAscii(const char* s) {
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p)
        if (*p < 0x20 || *p > 0x7E) return false;
    return true;
}

// Feed a document in fixed-size chunks. 0 means all at once.
bool parseChunked(const char* body, size_t len, size_t chunk, hafas::TripResult& out) {
    hafas::TripParser tp(out);
    bool ok = true;
    if (chunk == 0) {
        ok = tp.feed(body, len);
    } else {
        for (size_t i = 0; i < len && ok; i += chunk)
            ok = tp.feed(body + i, (len - i < chunk) ? (len - i) : chunk);
    }
    return ok && tp.finish();
}

// A canonical text form, so two parses can be compared and a mismatch prints
// something legible.
void describe(const hafas::TripResult& r, char* out, size_t outSize) {
    size_t w = 0;
    auto put = [&](const char* fmt, auto... args) {
        if (w < outSize) {
            const int n = std::snprintf(out + w, outSize - w, fmt, args...);
            if (n > 0) w += static_cast<size_t>(n);
            if (w > outSize) w = outSize;
        }
    };
    put("srv=%s dia=%s rt=%ld err=%s seen=%u count=%u dropped=%u\n",
        r.env.serverVersion, r.env.dialectVersion,
        static_cast<long>(r.env.planRtTs.min), r.env.errorCode,
        r.tripsSeen, r.tripCount, r.tripsDropped);
    for (uint8_t i = 0; i < r.tripCount; ++i) {
        const core::Itinerary& it = r.trips[i];
        put(" trip %u legs=%u tx=%ld canc=%d reach=%d dep=%ld arr=%ld\n",
            i, it.legCount, static_cast<long>(it.transfers), it.cancelled ? 1 : 0,
            it.reachable ? 1 : 0, static_cast<long>(it.departure().min),
            static_cast<long>(it.arrival().min));
        for (uint8_t j = 0; j < it.legCount; ++j) {
            const core::Leg& g = it.legs[j];
            put("  leg %u mode=%d line=%s from=%s to=%s dir=%s dep=%ld rtdep=%ld arr=%ld rtarr=%ld canc=%d\n",
                j, static_cast<int>(g.mode), g.line, g.fromName, g.toName, g.direction,
                static_cast<long>(g.plannedDep.min), static_cast<long>(g.rtDep.min),
                static_cast<long>(g.plannedArr.min), static_cast<long>(g.rtArr.min),
                g.cancelled ? 1 : 0);
        }
    }
}

// --- recorded fixtures (optional) -------------------------------------------

// The recorded responses are wrapped: {"_recorded_utc":..., "body": {...}}.
// Find the body by matching brackets while respecting string literals.
bool extractBody(const char* doc, size_t len, size_t& start, size_t& end) {
    const char* key = std::strstr(doc, "\"body\"");
    if (key == nullptr) return false;
    size_t i = static_cast<size_t>(key - doc) + 6;
    while (i < len && (doc[i] == ' ' || doc[i] == ':' || doc[i] == '\n' ||
                       doc[i] == '\r' || doc[i] == '\t')) ++i;
    if (i >= len || (doc[i] != '{' && doc[i] != '[')) return false;

    start = i;
    int depth = 0;
    bool inStr = false, esc = false;
    for (; i < len; ++i) {
        const char c = doc[i];
        if (inStr) {
            if (esc)            esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"')  inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') {
            --depth;
            if (depth == 0) { end = i + 1; return true; }
        }
    }
    return false;
}

char* g_fileBuf = nullptr;

// Fixtures live next to the sources, and `make test` runs from commute-tv/.
// Nothing fails when they are absent -- they are deliberately not committed.
bool loadFixtureBody(const char* name, const char*& body, size_t& len) {
    char path[256];
    const char* dirs[] = { "fixtures/", "commute-tv/fixtures/", "../fixtures/" };
    std::FILE* f = nullptr;
    for (const char* d : dirs) {
        std::snprintf(path, sizeof path, "%s%s", d, name);
        f = std::fopen(path, "rb");
        if (f) break;
    }
    if (!f) return false;

    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) { std::fclose(f); return false; }

    std::free(g_fileBuf);
    g_fileBuf = static_cast<char*>(std::malloc(static_cast<size_t>(size) + 1));
    if (!g_fileBuf) { std::fclose(f); return false; }
    const size_t got = std::fread(g_fileBuf, 1, static_cast<size_t>(size), f);
    g_fileBuf[got] = '\0';
    std::fclose(f);

    size_t s = 0, e = 0;
    if (!extractBody(g_fileBuf, got, s, e)) return false;
    body = g_fileBuf + s;
    len = e - s;
    return true;
}

} // namespace

void test_hafas_parse() {
    t::suite("hafas parse / product category to mode");
    CHECK_EQ(static_cast<int>(hafas::legModeFor("Bus", "NFB")),      static_cast<int>(LegMode::Bus));
    CHECK_EQ(static_cast<int>(hafas::legModeFor("RB", "NRB")),       static_cast<int>(LegMode::Train));
    CHECK_EQ(static_cast<int>(hafas::legModeFor("Netliner", "NET")), static_cast<int>(LegMode::Bus));
    // catOutS wins when both are present, and matching is case-insensitive.
    CHECK_EQ(static_cast<int>(hafas::legModeFor("nonsense", "nfb")), static_cast<int>(LegMode::Bus));
    // With no short code, the long name's leading word decides.
    CHECK_EQ(static_cast<int>(hafas::legModeFor("Bus 47", "")),      static_cast<int>(LegMode::Bus));
    CHECK_EQ(static_cast<int>(hafas::legModeFor("Stra\xc3\x9f""enbahn", "")), static_cast<int>(LegMode::Tram));
    CHECK_EQ(static_cast<int>(hafas::legModeFor("Strassenbahn", "")),static_cast<int>(LegMode::Tram));
    CHECK_EQ(static_cast<int>(hafas::legModeFor("RE 1", "")),        static_cast<int>(LegMode::Train));
    // An unknown category is Other, never Walk: mistaking transit for a walk
    // would drop the leg out of firstTransitLeg() and blank the display.
    CHECK_EQ(static_cast<int>(hafas::legModeFor("Seilbahn", "SB")),  static_cast<int>(LegMode::Other));
    CHECK_EQ(static_cast<int>(hafas::legModeFor("", "")),            static_cast<int>(LegMode::Other));
    CHECK_EQ(static_cast<int>(hafas::legModeFor(nullptr, nullptr)),  static_cast<int>(LegMode::Other));

    t::suite("hafas parse / a real trip response");
    hafas::TripResult r;
    const char* err = nullptr;
    CHECK(hafas::parseTripResponse(kTripDoc, std::strlen(kTripDoc), r, &err));
    CHECK_STR(err, "");

    CHECK_STR(r.env.serverVersion, "2.51.0");
    CHECK_STR(r.env.dialectVersion, "2.51");
    CHECK(!r.env.hasError());
    CHECK(r.env.planRtTs.valid());
    CHECK_EQ(r.env.planRtTs.min, core::parseDateTime("2026-08-22", "18:46").min);

    CHECK_EQ(r.tripsSeen, 4);
    CHECK_EQ(r.tripCount, 4);
    CHECK_EQ(r.tripsDropped, 0);

    {   // Trip 0: three legs, the shape the display actually consumes.
        const core::Itinerary& it = r.trips[0];
        CHECK_EQ(it.legCount, 3);
        CHECK_EQ(it.transfers, 1);
        CHECK(it.usable());
        CHECK(!it.cancelled);
        CHECK(it.reachable);

        CHECK_EQ(static_cast<int>(it.legs[0].mode), static_cast<int>(LegMode::Walk));
        CHECK_EQ(static_cast<int>(it.legs[1].mode), static_cast<int>(LegMode::Bus));
        CHECK_EQ(static_cast<int>(it.legs[2].mode), static_cast<int>(LegMode::Bus));

        // Umlauts are folded, because the font has no glyphs for them and an
        // unfolded name renders as "Hrn Brcke".
        CHECK_STR(it.legs[0].toName, "Aachen, Hoern Bruecke");
        CHECK_STR(it.legs[2].toName, "Aachen, Scheibenstrasse");
        CHECK_STR(it.legs[2].direction, "Huels Gewerbegebiet");
        CHECK(allPrintableAscii(it.legs[0].toName));
        CHECK(allPrintableAscii(it.legs[2].direction));

        // Walk legs name their endpoints as coordinates, so the boarding stop
        // has to come from the first transit leg.
        CHECK_STR(it.legs[0].fromName, "50.775300, 6.083900");
        const core::Leg* first = it.firstTransitLeg();
        CHECK(first != nullptr);
        CHECK_STR(first->line, "Netliner");        // line is not always numeric
        CHECK_STR(first->fromName, "Aachen, Hoern Bruecke");
        CHECK_STR(it.legs[2].line, "47");

        // Deeper `time` fields inside JourneyDetail must not leak into the leg.
        CHECK_EQ(it.legs[1].plannedDep.min, core::parseDateTime("2026-08-22", "19:26").min);
        CHECK_EQ(it.legs[1].rtDep.min,      core::parseDateTime("2026-08-22", "19:28").min);
        CHECK_EQ(it.legs[1].depDelayMin(), 2);
        CHECK_EQ(it.legs[1].depTime().min, it.legs[1].rtDep.min);

        // Leg 2 carries no realtime at all -- the fallback is the normal case.
        CHECK(!it.legs[2].rtDep.valid());
        CHECK_EQ(it.legs[2].depTime().min, it.legs[2].plannedDep.min);
        CHECK_EQ(it.legs[2].depDelayMin(), 0);

        // Itinerary-level times come from the first and last legs.
        CHECK_EQ(it.departure().min, core::parseDateTime("2026-08-22", "19:18").min);
        CHECK_EQ(it.arrival().min,   core::parseDateTime("2026-08-22", "19:52").min);
        CHECK_EQ(it.depDelayMin(), 0);             // the walk has no delay
    }
    {   // Trip 1: a broken interchange. `reachable: false` on any leg means the
        // itinerary is not one to act on.
        const core::Itinerary& it = r.trips[1];
        CHECK(!it.reachable);
        CHECK(!it.usable());
        CHECK(!it.cancelled);
    }
    {   // Trip 2: a cancelled leg cancels the itinerary.
        const core::Itinerary& it = r.trips[2];
        CHECK(it.legs[0].cancelled);
        CHECK(it.cancelled);
        CHECK(!it.usable());
        CHECK_EQ(static_cast<int>(it.legs[0].mode), static_cast<int>(LegMode::Train));
    }
    {   // Trip 3: "24:05:00" is a legal HAFAS time meaning 00:05 the next day.
        const core::Itinerary& it = r.trips[3];
        CHECK(it.usable());
        CHECK_EQ(it.arrival().min, core::parseDateTime("2026-08-23", "00:05").min);
        CHECK_EQ(core::diffMinutes(it.arrival(), it.departure()), 15);
    }

    t::suite("hafas parse / chunk boundaries");
    {
        char whole[8192];
        hafas::TripResult a;
        CHECK(parseChunked(kTripDoc, std::strlen(kTripDoc), 0, a));
        describe(a, whole, sizeof whole);

        bool same = true;
        size_t badAt = 0;
        for (size_t c = 1; c <= 64 && same; ++c) {
            hafas::TripResult b;
            char text[8192];
            if (!parseChunked(kTripDoc, std::strlen(kTripDoc), c, b)) { same = false; badAt = c; break; }
            describe(b, text, sizeof text);
            if (std::strcmp(text, whole) != 0) { same = false; badAt = c; }
        }
        if (!same) std::printf("       first differing chunk size: %zu\n", badAt);
        CHECK(same);
    }

    t::suite("hafas parse / errors and edges");
    {   // A 401 body is a valid document carrying an error. HTTP status is the
        // transport's business; this layer must read errorCode either way.
        const char* doc = "{\"serverVersion\":\"2.51.0\",\"dialectVersion\":\"2.51\","
                          "\"errorCode\":\"API_AUTH\","
                          "\"errorText\":\"access denied for X on trip identified by trip\","
                          "\"requestId\":\"default-request-id\"}";
        hafas::TripResult e;
        CHECK(hafas::parseTripResponse(doc, std::strlen(doc), e));
        CHECK(e.env.hasError());
        CHECK_STR(e.env.errorCode, "API_AUTH");
        CHECK_STR(e.env.errorText, "access denied for X on trip identified by trip");
        CHECK_EQ(e.tripCount, 0);
        CHECK(e.empty());
    }
    {   // No connections is a valid answer and must not look like a failure.
        const char* doc = "{\"Trip\":[],\"serverVersion\":\"2.51.0\"}";
        hafas::TripResult e;
        CHECK(hafas::parseTripResponse(doc, std::strlen(doc), e));
        CHECK(!e.env.hasError());
        CHECK_EQ(e.tripCount, 0);
        CHECK_EQ(e.tripsSeen, 0);
    }
    {   // A body cut off mid-document is a failure, never a short result.
        char doc[600];
        std::snprintf(doc, sizeof doc, "%.*s", 500, kTripDoc);
        hafas::TripResult e;
        const char* why = nullptr;
        CHECK(!hafas::parseTripResponse(doc, std::strlen(doc), e, &why));
        CHECK(why != nullptr && why[0] != '\0');
    }
    {   // More itineraries than we can hold: keep the first kMaxCandidates and
        // say how many there were, rather than pretending we saw them all.
        char doc[4096];
        size_t w = 0;
        w += static_cast<size_t>(std::snprintf(doc + w, sizeof doc - w, "{\"Trip\":["));
        for (int i = 0; i < 9; ++i) {
            w += static_cast<size_t>(std::snprintf(doc + w, sizeof doc - w,
                "%s{\"idx\":%d,\"LegList\":{\"Leg\":[{\"type\":\"JNY\","
                "\"Origin\":{\"name\":\"A\",\"time\":\"08:0%d:00\",\"date\":\"2026-08-22\"},"
                "\"Destination\":{\"name\":\"B\",\"time\":\"08:3%d:00\",\"date\":\"2026-08-22\"},"
                "\"Product\":[{\"line\":\"1\",\"catOutS\":\"NFB\"}]}]}}",
                i ? "," : "", i, i, i));
        }
        w += static_cast<size_t>(std::snprintf(doc + w, sizeof doc - w, "]}"));
        hafas::TripResult e;
        CHECK(hafas::parseTripResponse(doc, w, e));
        CHECK_EQ(e.tripsSeen, 9);
        CHECK_EQ(e.tripCount, core::kMaxCandidates);
        CHECK_EQ(e.trips[0].legs[0].plannedDep.min, core::parseDateTime("2026-08-22", "08:00").min);
    }
    {   // An itinerary with more legs than we can hold is dropped whole. Keeping
        // the first six would put the wrong arrival time on the screen, which is
        // worse than showing one connection fewer.
        char doc[4096];
        size_t w = 0;
        w += static_cast<size_t>(std::snprintf(doc + w, sizeof doc - w,
            "{\"Trip\":[{\"idx\":0,\"LegList\":{\"Leg\":["));
        for (int i = 0; i < 8; ++i) {
            w += static_cast<size_t>(std::snprintf(doc + w, sizeof doc - w,
                "%s{\"type\":\"JNY\","
                "\"Origin\":{\"name\":\"A\",\"time\":\"0%d:00:00\",\"date\":\"2026-08-22\"},"
                "\"Destination\":{\"name\":\"B\",\"time\":\"0%d:30:00\",\"date\":\"2026-08-22\"},"
                "\"Product\":[{\"line\":\"1\",\"catOutS\":\"NFB\"}]}",
                i ? "," : "", i + 1, i + 1));
        }
        w += static_cast<size_t>(std::snprintf(doc + w, sizeof doc - w, "]}}]}"));
        hafas::TripResult e;
        CHECK(hafas::parseTripResponse(doc, w, e));
        CHECK_EQ(e.tripsSeen, 1);
        CHECK_EQ(e.tripCount, 0);
        CHECK_EQ(e.tripsDropped, 1);
    }
    {   // Product flattened to an object rather than an array -- not what the
        // server sends today, but the difference must not silently blank the
        // line number if a dialect bump changes it.
        const char* doc = "{\"Trip\":[{\"LegList\":{\"Leg\":[{\"type\":\"JNY\","
            "\"Origin\":{\"name\":\"A\",\"time\":\"08:00:00\",\"date\":\"2026-08-22\"},"
            "\"Destination\":{\"name\":\"B\",\"time\":\"08:30:00\",\"date\":\"2026-08-22\"},"
            "\"Product\":{\"line\":\"3B\",\"catOut\":\"Bus\",\"catOutS\":\"NFB\"}}]}}]}";
        hafas::TripResult e;
        CHECK(hafas::parseTripResponse(doc, std::strlen(doc), e));
        CHECK_EQ(e.tripCount, 1);
        CHECK_STR(e.trips[0].legs[0].line, "3B");
        CHECK_EQ(static_cast<int>(e.trips[0].legs[0].mode), static_cast<int>(LegMode::Bus));
    }
    {   // An empty Product array must not be indexed. The line falls back to
        // the leg's own name.
        const char* doc = "{\"Trip\":[{\"LegList\":{\"Leg\":[{\"type\":\"JNY\",\"name\":\"Bus 3B\","
            "\"Origin\":{\"name\":\"A\",\"time\":\"08:00:00\",\"date\":\"2026-08-22\"},"
            "\"Destination\":{\"name\":\"B\",\"time\":\"08:30:00\",\"date\":\"2026-08-22\"},"
            "\"Product\":[]}]}}]}";
        hafas::TripResult e;
        CHECK(hafas::parseTripResponse(doc, std::strlen(doc), e));
        CHECK_EQ(e.tripCount, 1);
        CHECK_STR(e.trips[0].legs[0].line, "Bus 3B");
        CHECK_EQ(static_cast<int>(e.trips[0].legs[0].mode), static_cast<int>(LegMode::Other));
    }
    {   // Unknown fields, at every level, are ignored rather than fatal. The XSD
        // we were given is 2.49.0 and the server reports 2.51.0.
        const char* doc = "{\"Trip\":[{\"futureThing\":{\"a\":[1,2,{\"b\":null}]},"
            "\"LegList\":{\"Leg\":[{\"type\":\"JNY\",\"newField\":true,"
            "\"Origin\":{\"name\":\"A\",\"time\":\"08:00:00\",\"date\":\"2026-08-22\",\"extra\":[]},"
            "\"Destination\":{\"name\":\"B\",\"time\":\"08:30:00\",\"date\":\"2026-08-22\"},"
            "\"Product\":[{\"line\":\"1\",\"catOutS\":\"NFB\",\"unknown\":{\"deep\":1}}]}]}}],"
            "\"somethingNew\":42}";
        hafas::TripResult e;
        CHECK(hafas::parseTripResponse(doc, std::strlen(doc), e));
        CHECK_EQ(e.tripCount, 1);
        CHECK_STR(e.trips[0].legs[0].line, "1");
    }

    // --- the recorded responses, when they are on this machine ---------------
    t::suite("hafas parse / recorded AVV fixtures");
    const char* body = nullptr;
    size_t blen = 0;
    if (!loadFixtureBody("trip_arrival_realtime.json", body, blen)) {
        std::printf("  (skipped: no fixtures/ on this machine -- run\n"
                    "   tools/record_fixtures.py during German office hours)\n");
    } else {
        hafas::TripResult live;
        CHECK(parseChunked(body, blen, 0, live));
        CHECK(!live.env.hasError());
        CHECK_STR(live.env.serverVersion, "2.51.0");
        CHECK(live.env.planRtTs.valid());
        CHECK(live.tripCount > 0);
        CHECK_EQ(live.tripsDropped, 0);
        CHECK_EQ(live.tripCount, live.tripsSeen);

        bool structureOk = true, textOk = true, transitOk = true;
        for (uint8_t i = 0; i < live.tripCount; ++i) {
            const core::Itinerary& it = live.trips[i];
            if (it.legCount == 0 || !it.departure().valid() || !it.arrival().valid() ||
                it.arrival() < it.departure()) structureOk = false;
            if (it.firstTransitLeg() == nullptr) transitOk = false;
            for (uint8_t j = 0; j < it.legCount; ++j) {
                const core::Leg& g = it.legs[j];
                if (!allPrintableAscii(g.fromName) || !allPrintableAscii(g.toName) ||
                    !allPrintableAscii(g.line) || !allPrintableAscii(g.direction))
                    textOk = false;
                if (!g.plannedDep.valid() || !g.plannedArr.valid()) structureOk = false;
            }
        }
        CHECK(structureOk);
        CHECK(textOk);
        CHECK(transitOk);

        // At least one leg must carry a realtime time, or this fixture is not
        // exercising the realtime path at all and a parser that never read
        // rtTime would pass every check above.
        bool anyRealtime = false;
        for (uint8_t i = 0; i < live.tripCount; ++i)
            for (uint8_t j = 0; j < live.trips[i].legCount; ++j)
                if (live.trips[i].legs[j].rtDep.valid()) anyRealtime = true;
        CHECK(anyRealtime);

        // The same bytes delivered in different-sized pieces must give the same
        // answer -- this is the property the socket depends on.
        char ref[16384];
        describe(live, ref, sizeof ref);
        bool same = true;
        for (size_t c : {size_t(1), size_t(7), size_t(512), size_t(4096)}) {
            hafas::TripResult b;
            char text[16384];
            if (!parseChunked(body, blen, c, b)) { same = false; break; }
            describe(b, text, sizeof text);
            if (std::strcmp(text, ref) != 0) { same = false; break; }
        }
        CHECK(same);

        // The same query with rtMode=OFF: no realtime anywhere, so every leg
        // must fall back to planned and report no delay. This is the baseline
        // that makes the delay arithmetic above meaningful.
        if (loadFixtureBody("trip_arrival_planned.json", body, blen)) {
            hafas::TripResult plan;
            CHECK(parseChunked(body, blen, 0, plan));
            CHECK(plan.tripCount > 0);
            bool noRealtime = true;
            for (uint8_t i = 0; i < plan.tripCount; ++i)
                for (uint8_t j = 0; j < plan.trips[i].legCount; ++j) {
                    const core::Leg& g = plan.trips[i].legs[j];
                    if (g.rtDep.valid() || g.rtArr.valid() || g.depDelayMin() != 0)
                        noRealtime = false;
                    if (g.depTime() != g.plannedDep) noRealtime = false;
                }
            CHECK(noRealtime);
        }

        // The forward search (searchForArrival=0) is a different query against
        // the same schema. It must parse identically -- if it did not, the
        // parser would be keyed to one call rather than to the response shape.
        if (loadFixtureBody("trip_departure_realtime.json", body, blen)) {
            hafas::TripResult fwd;
            CHECK(parseChunked(body, blen, 0, fwd));
            CHECK(fwd.tripCount > 0);
            CHECK(!fwd.env.hasError());
            bool fwdOk = true;
            for (uint8_t i = 0; i < fwd.tripCount; ++i) {
                const core::Itinerary& it = fwd.trips[i];
                if (it.legCount == 0 || it.arrival() < it.departure() ||
                    it.firstTransitLeg() == nullptr) fwdOk = false;
            }
            CHECK(fwdOk);
        }

        if (loadFixtureBody("error_bad_accessid.json", body, blen)) {
            hafas::TripResult e;
            CHECK(parseChunked(body, blen, 0, e));
            CHECK(e.env.hasError());
            CHECK_STR(e.env.errorCode, "API_AUTH");
            CHECK_EQ(e.tripCount, 0);
        }

        // Responses from other services are valid JSON with no Trip in them.
        // Handing one to this parser must produce nothing, not a failure.
        for (const char* other : {"location_stop.json", "location_nonsense.json",
                                  "him_messages.json", "datainfo.json"}) {
            if (!loadFixtureBody(other, body, blen)) continue;
            hafas::TripResult e;
            CHECK(parseChunked(body, blen, 0, e));
            CHECK_EQ(e.tripCount, 0);
        }
    }

    std::free(g_fileBuf);
    g_fileBuf = nullptr;
}
