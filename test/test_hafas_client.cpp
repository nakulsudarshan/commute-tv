#include "test.h"
#include "../src/hafas/hafas_client.h"

#include <cstdio>
#include <cstring>

using namespace hafas;

namespace {

struct StubHttp final : net::Client {
    int status = 200;
    net::Error error = net::Error::None;
    const char* body = "";
    char lastUrl[768] = {0};
    int gets = 0;
    size_t chunk = 11;

    net::Result get(const char* url, const net::Header*, size_t,
                    net::BodySink& out) override {
        ++gets;
        std::snprintf(lastUrl, sizeof lastUrl, "%s", url);
        net::Result r;
        r.status = status;
        r.error = error;
        if (error != net::Error::None) return r;
        const size_t len = std::strlen(body);
        for (size_t i = 0; i < len; i += chunk) {
            const size_t take = (len - i < chunk) ? (len - i) : chunk;
            if (!out.write(body + i, take)) { r.error = net::Error::Aborted; return r; }
        }
        r.bytes = len;
        return r;
    }
    net::Result post(const char*, const char*, const char*, size_t,
                     const net::Header*, size_t, net::BodySink&) override {
        return net::Result{};
    }
};

Endpoint testEndpoint() {
    Endpoint ep;
    ep.env = Env::Demo;
    ep.accessId = "00000000-1111-2222-3333-444444444444";
    return ep;
}

TripQuery aachenTrip() {
    TripQuery q;
    q.originLat = 50.784294; q.originLon = 6.052599;
    q.destLat = 50.778091;   q.destLon = 6.078766;
    q.dateDays = core::parseDate("2026-08-25");
    q.timeOfDayMin = core::parseTimeOfDay("15:30");
    return q;
}

const char* kTripBody =
    "{\"Trip\":[{\"idx\":0,\"transferCount\":0,\"LegList\":{\"Leg\":["
    "{\"type\":\"JNY\",\"name\":\"Bus 12\",\"reachable\":true,\"direction\":\"Uniklinik\","
    "\"Origin\":{\"name\":\"Aachen, H\\u00f6rn Br\\u00fccke\",\"time\":\"15:28:00\","
    "\"date\":\"2026-08-25\",\"rtTime\":\"15:30:00\",\"rtDate\":\"2026-08-25\"},"
    "\"Destination\":{\"name\":\"Aachen, Ponttor\",\"time\":\"15:49:00\","
    "\"date\":\"2026-08-25\"},"
    "\"Product\":[{\"line\":\"12\",\"catOut\":\"Bus\",\"catOutS\":\"NFB\"}]}"
    "]}}],\"serverVersion\":\"2.51.0\",\"dialectVersion\":\"2.51\","
    "\"planRtTs\":\"2026-08-25T15:20:00.000+02:00\"}";

// The shape confirmed by the recorded location_stop / location_address fixtures.
const char* kPlaceBody =
    "{\"stopLocationOrCoordLocation\":["
    "{\"StopLocation\":{\"id\":\"A=1@O=Aachen, Ponttor@\",\"extId\":\"1055\","
    "\"name\":\"Aachen, Ponttor\",\"lon\":6.077841,\"lat\":50.781709,\"weight\":236}},"
    "{\"CoordLocation\":{\"name\":\"Aachen - Aachen, Templergraben 55\",\"type\":\"ADR\","
    "\"lon\":6.077939,\"lat\":50.777583}}"
    "],\"serverVersion\":\"2.51.0\",\"requestId\":\"x\"}";

bool has(const char* hay, const char* needle) {
    return std::strstr(hay, needle) != nullptr;
}

} // namespace

void test_hafas_client() {
    t::suite("hafas client / planning a trip");
    {
        StubHttp http;
        http.body = kTripBody;
        Client c(testEndpoint(), http);
        TripResult out;
        CHECK_EQ(static_cast<int>(c.trip(aachenTrip(), out)), static_cast<int>(Status::Ok));
        CHECK_EQ(http.gets, 1);
        CHECK(has(http.lastUrl, "/trip?"));
        CHECK(has(http.lastUrl, "searchForArrival=1"));
        CHECK(has(http.lastUrl, "withFreq=0"));       // 52% of the payload, off

        CHECK_EQ(out.tripCount, 1);
        CHECK_STR(out.env.serverVersion, "2.51.0");
        CHECK(out.env.planRtTs.valid());

        const core::Itinerary& it = out.trips[0];
        CHECK(it.usable());
        // Umlauts folded, realtime preferred over planned, mode from catOutS.
        CHECK_STR(it.legs[0].fromName, "Aachen, Hoern Bruecke");
        CHECK_EQ(it.legs[0].depDelayMin(), 2);
        CHECK_STR(it.legs[0].line, "12");
        CHECK_EQ(static_cast<int>(it.legs[0].mode), static_cast<int>(core::LegMode::Bus));
    }
    {
        // No itineraries is a real answer, and its own status -- the caller
        // shows "no connection", not an error.
        StubHttp http;
        http.body = "{\"Trip\":[],\"serverVersion\":\"2.51.0\"}";
        Client c(testEndpoint(), http);
        TripResult out;
        CHECK_EQ(static_cast<int>(c.trip(aachenTrip(), out)), static_cast<int>(Status::NoResult));
        CHECK(out.empty());
    }
    {
        // A 200 that carries errorCode is still a refusal. This is the case the
        // envelope exists for.
        StubHttp http;
        http.body = "{\"errorCode\":\"API_AUTH\",\"errorText\":\"access denied\","
                    "\"serverVersion\":\"2.51.0\"}";
        Client c(testEndpoint(), http);
        TripResult out;
        CHECK_EQ(static_cast<int>(c.trip(aachenTrip(), out)), static_cast<int>(Status::Denied));
        CHECK_STR(c.lastAvvError(), "API_AUTH");
        CHECK_STR(c.lastError(), "access denied");
    }
    {
        StubHttp http;
        http.status = 401;
        http.body = "{\"errorCode\":\"API_AUTH\",\"errorText\":\"nope\"}";
        Client c(testEndpoint(), http);
        TripResult out;
        CHECK_EQ(static_cast<int>(c.trip(aachenTrip(), out)), static_cast<int>(Status::Denied));
        CHECK_STR(c.lastAvvError(), "API_AUTH");
    }
    {
        // A body cut short must hand back NOTHING. Two complete itineraries
        // followed by a truncated third would otherwise look like a successful
        // answer that happens to be missing the best option.
        StubHttp http;
        http.body = "{\"Trip\":[{\"idx\":0,\"LegList\":{\"Leg\":[{\"type\":\"JNY\",";
        Client c(testEndpoint(), http);
        TripResult out;
        CHECK_EQ(static_cast<int>(c.trip(aachenTrip(), out)),
                 static_cast<int>(Status::BadResponse));
        CHECK_EQ(out.tripCount, 0);
    }
    {
        StubHttp http;
        http.error = net::Error::Timeout;
        Client c(testEndpoint(), http);
        TripResult out;
        CHECK_EQ(static_cast<int>(c.trip(aachenTrip(), out)),
                 static_cast<int>(Status::Transport));
    }
    {
        StubHttp http;
        Endpoint noKey = testEndpoint();
        noKey.accessId = "";
        Client c(noKey, http);
        TripResult out;
        CHECK_EQ(static_cast<int>(c.trip(aachenTrip(), out)),
                 static_cast<int>(Status::BadConfig));
        CHECK_EQ(http.gets, 0);          // never put a request on the wire
    }

    t::suite("hafas client / turning text into a coordinate");
    {
        StubHttp http;
        http.body = kPlaceBody;
        Client c(testEndpoint(), http);
        PlaceList out;
        CHECK_EQ(static_cast<int>(c.resolve("Ponttor", out)), static_cast<int>(Status::Ok));
        CHECK(has(http.lastUrl, "location.name?"));
        CHECK(has(http.lastUrl, "Ponttor"));

        CHECK_EQ(out.count, 2);
        CHECK_EQ(out.seen, 2);

        // A stop can be departed from and carries an extId; an address cannot
        // and does not. Conflating them would send a journey to the wrong kind
        // of thing.
        CHECK_STR(out.places[0].name, "Aachen, Ponttor");
        CHECK_STR(out.places[0].extId, "1055");
        CHECK(out.places[0].isStop);
        CHECK_NEAR(out.places[0].lat, 50.781709, 1e-6);
        CHECK_NEAR(out.places[0].lon, 6.077841, 1e-6);

        CHECK_STR(out.places[1].name, "Aachen - Aachen, Templergraben 55");
        CHECK_STR(out.places[1].extId, "");
        CHECK(!out.places[1].isStop);
        CHECK_NEAR(out.places[1].lat, 50.777583, 1e-6);
    }
    {
        // Umlauts are folded here too -- these names reach the screen.
        StubHttp http;
        http.body = "{\"stopLocationOrCoordLocation\":[{\"StopLocation\":"
                    "{\"name\":\"Aachen, Scheibenstra\\u00dfe\",\"extId\":\"1105\","
                    "\"lat\":50.7745,\"lon\":6.101365}}]}";
        Client c(testEndpoint(), http);
        PlaceList out;
        CHECK_EQ(static_cast<int>(c.resolve("Scheibenstrasse", out)),
                 static_cast<int>(Status::Ok));
        CHECK_STR(out.places[0].name, "Aachen, Scheibenstrasse");
    }
    {
        // An empty result set is a real answer: the location string was not
        // somewhere AVV knows.
        StubHttp http;
        http.body = "{\"stopLocationOrCoordLocation\":[],\"serverVersion\":\"2.51.0\"}";
        Client c(testEndpoint(), http);
        PlaceList out;
        CHECK_EQ(static_cast<int>(c.resolve("qqqq", out)), static_cast<int>(Status::NoResult));
        CHECK(out.empty());
    }
    {
        // More candidates than we can hold: keep the first, count the rest.
        StubHttp http;
        char body[1024];
        size_t w = 0;
        w += static_cast<size_t>(std::snprintf(body + w, sizeof body - w,
                                               "{\"stopLocationOrCoordLocation\":["));
        for (int i = 0; i < 7; ++i)
            w += static_cast<size_t>(std::snprintf(body + w, sizeof body - w,
                "%s{\"StopLocation\":{\"name\":\"Stop %d\",\"extId\":\"%d\","
                "\"lat\":50.7%d,\"lon\":6.0%d}}", i ? "," : "", i, i, i, i));
        w += static_cast<size_t>(std::snprintf(body + w, sizeof body - w, "]}"));
        http.body = body;
        Client c(testEndpoint(), http);
        PlaceList out;
        CHECK_EQ(static_cast<int>(c.resolve("Stop", out)), static_cast<int>(Status::Ok));
        CHECK_EQ(out.count, kMaxPlaces);
        CHECK_EQ(out.seen, 7);
    }
    {
        // An entry with no coordinate is not a place. Keeping it would send a
        // journey to 0,0 in the Atlantic.
        StubHttp http;
        http.body = "{\"stopLocationOrCoordLocation\":["
                    "{\"StopLocation\":{\"name\":\"Nowhere\",\"extId\":\"9\"}},"
                    "{\"StopLocation\":{\"name\":\"Real\",\"extId\":\"1\","
                    "\"lat\":50.78,\"lon\":6.05}}]}";
        Client c(testEndpoint(), http);
        PlaceList out;
        CHECK_EQ(static_cast<int>(c.resolve("x", out)), static_cast<int>(Status::Ok));
        CHECK_EQ(out.count, 1);
        CHECK_STR(out.places[0].name, "Real");
        CHECK_EQ(out.seen, 2);
    }
    {
        StubHttp http;
        Client c(testEndpoint(), http);
        PlaceList out;
        CHECK_EQ(static_cast<int>(c.resolve("", out)), static_cast<int>(Status::BadConfig));
        CHECK_EQ(static_cast<int>(c.resolve(nullptr, out)), static_cast<int>(Status::BadConfig));
        CHECK_EQ(http.gets, 0);
    }

    t::suite("hafas client / status names");
    CHECK_STR(statusName(Status::Ok), "ok");
    CHECK_STR(statusName(Status::Denied), "denied");
    CHECK_STR(statusName(Status::NoResult), "no result");
}
