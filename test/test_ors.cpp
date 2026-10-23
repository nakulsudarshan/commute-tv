#include "test.h"
#include "../src/ors/ors_client.h"

#include <cstdio>
#include <cstring>

using namespace ors;

namespace {

struct StubHttp final : net::Client {
    int status = 200;
    net::Error error = net::Error::None;
    const char* body = "";
    char lastUrl[256] = {0};
    char lastBody[256] = {0};
    char lastAuth[128] = {0};
    int posts = 0;
    size_t chunk = 5;

    net::Result get(const char*, const net::Header*, size_t, net::BodySink&) override {
        return net::Result{};
    }
    net::Result post(const char* url, const char*, const char* b, size_t bLen,
                     const net::Header* headers, size_t headerCount,
                     net::BodySink& out) override {
        ++posts;
        std::snprintf(lastUrl, sizeof lastUrl, "%s", url);
        const size_t n = bLen < sizeof lastBody - 1 ? bLen : sizeof lastBody - 1;
        std::memcpy(lastBody, b, n);
        lastBody[n] = '\0';
        for (size_t i = 0; i < headerCount; ++i)
            if (std::strcmp(headers[i].name, "Authorization") == 0)
                std::snprintf(lastAuth, sizeof lastAuth, "%s", headers[i].value);

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
};

Config testConfig() {
    Config c;
    c.apiKey = "5b3ce35978611110000001635examplekey";
    return c;
}

Query aachenRide() {
    Query q;
    q.fromLat = 50.784294; q.fromLon = 6.052599;      // home, Hoern
    q.toLat   = 50.778091; q.toLon   = 6.078766;      // SuperC
    return q;
}

// The shape confirmed by a live call in tools/check_env.py.
const char* kGoodBody = R"JSON({
  "routes": [
    {
      "summary": { "distance": 3860.4, "duration": 786.2 },
      "ascent": 40.0,
      "descent": 77.0,
      "bbox": [6.0526, 50.7781, 6.0788, 50.7843]
    }
  ],
  "metadata": { "service": "routing" }
})JSON";

bool has(const char* hay, const char* needle) {
    return std::strstr(hay, needle) != nullptr;
}

} // namespace

void test_ors() {
    t::suite("ors / the coordinate order that ruins everything");
    {
        char body[256];
        CHECK(buildRequestBody(aachenRide(), body, sizeof body) > 0);
        // LONGITUDE FIRST. Google Maps gives lat,lon; openrouteservice wants
        // lon,lat. Transposed, Aachen lands in Somalia and the API reports a
        // routing failure that looks nothing like a coordinate problem.
        CHECK_STR(body,
            "{\"coordinates\":[[6.052599,50.784294],[6.078766,50.778091]],"
            "\"elevation\":true,\"units\":\"m\"}");
        // The longitude of Aachen is single-digit and the latitude is fifty-
        // something, so the transposition is visible in the literal above.
        CHECK(has(body, "[[6.0"));
        CHECK(!has(body, "[[50."));

        // elevation is the entire reason this service is in the project.
        CHECK(has(body, "\"elevation\":true"));
    }
    {
        // Southern and western hemispheres, and a zero component.
        Query q;
        q.fromLat = -33.865143; q.fromLon = 151.209900;   // Sydney
        q.toLat = 51.500000;    q.toLon = -0.126000;      // London
        char body[256];
        CHECK(buildRequestBody(q, body, sizeof body) > 0);
        CHECK_STR(body,
            "{\"coordinates\":[[151.209900,-33.865143],[-0.126000,51.500000]],"
            "\"elevation\":true,\"units\":\"m\"}");
    }
    {
        char body[256];
        // 0,0 is in the Atlantic and is the shape of an unfilled config entry.
        Query zero;
        CHECK_EQ(buildRequestBody(zero, body, sizeof body), -1);
        Query offEarth = aachenRide();
        offEarth.toLat = 91.0;
        CHECK_EQ(buildRequestBody(offEarth, body, sizeof body), -1);
        char tiny[16];
        CHECK_EQ(buildRequestBody(aachenRide(), tiny, sizeof tiny), -1);
    }

    t::suite("ors / the url");
    {
        char url[192];
        CHECK(buildUrl(testConfig(), url, sizeof url) > 0);
        CHECK_STR(url, "https://api.openrouteservice.org/v2/directions/cycling-regular/json");
        Config electric = testConfig();
        electric.profile = "cycling-electric";
        CHECK(buildUrl(electric, url, sizeof url) > 0);
        CHECK(has(url, "/cycling-electric/json"));
        Config noProfile = testConfig();
        noProfile.profile = "";
        CHECK_EQ(buildUrl(noProfile, url, sizeof url), -1);
    }

    t::suite("ors / reading a route");
    {
        StubHttp http;
        http.body = kGoodBody;
        Client c(testConfig(), http);
        core::CycleRoute route;
        CHECK_EQ(static_cast<int>(c.route(aachenRide(), route)), static_cast<int>(Status::Ok));
        CHECK(route.valid);
        CHECK_NEAR(route.distanceKm, 3.8604, 0.001);      // metres in, km out
        CHECK_EQ(route.durationMin, 13);                  // 786 s, rounded
        CHECK_NEAR(route.ascentM, 40.0, 0.01);
        CHECK_NEAR(route.descentM, 77.0, 0.01);
        // The key goes in bare, not as a Bearer token.
        CHECK_STR(http.lastAuth, "5b3ce35978611110000001635examplekey");
    }
    {
        // Some responses carry ascent inside summary instead of beside it.
        StubHttp http;
        http.body = "{\"routes\":[{\"summary\":{\"distance\":1000,\"duration\":300,"
                    "\"ascent\":12.5,\"descent\":3.25}}]}";
        Client c(testConfig(), http);
        core::CycleRoute route;
        CHECK_EQ(static_cast<int>(c.route(aachenRide(), route)), static_cast<int>(Status::Ok));
        CHECK_NEAR(route.ascentM, 12.5, 0.01);
        CHECK_NEAR(route.descentM, 3.25, 0.01);
        CHECK_EQ(route.durationMin, 5);
    }
    {
        // A route with no elevation is not a usable answer, however well-formed.
        // Accepting it would make the comfort score treat a climb as flat --
        // the misleading zero this project keeps refusing to produce.
        StubHttp http;
        http.body = "{\"routes\":[{\"summary\":{\"distance\":3860,\"duration\":786}}]}";
        Client c(testConfig(), http);
        core::CycleRoute route;
        CHECK_EQ(static_cast<int>(c.route(aachenRide(), route)),
                 static_cast<int>(Status::BadResponse));
        CHECK(!route.valid);
    }
    {
        StubHttp http;
        http.body = "{\"routes\":[]}";
        Client c(testConfig(), http);
        core::CycleRoute route;
        CHECK_EQ(static_cast<int>(c.route(aachenRide(), route)), static_cast<int>(Status::NoRoute));
        CHECK(!route.valid);
    }

    t::suite("ors / failures worth telling apart");
    {
        // 429 is not a failure of the key, and not a reason to throw away a
        // cached route -- the free tier is 2000 a day and this is cached.
        StubHttp http;
        http.status = 429;
        http.body = "{\"error\":\"Rate limit exceeded\"}";
        Client c(testConfig(), http);
        core::CycleRoute route;
        CHECK_EQ(static_cast<int>(c.route(aachenRide(), route)),
                 static_cast<int>(Status::QuotaExceeded));
    }
    {
        StubHttp http;
        http.status = 403;
        http.body = "{\"error\":{\"message\":\"Access denied\"}}";
        Client c(testConfig(), http);
        core::CycleRoute route;
        CHECK_EQ(static_cast<int>(c.route(aachenRide(), route)),
                 static_cast<int>(Status::KeyRejected));
    }
    {
        StubHttp http;
        http.error = net::Error::Timeout;
        Client c(testConfig(), http);
        core::CycleRoute route;
        CHECK_EQ(static_cast<int>(c.route(aachenRide(), route)),
                 static_cast<int>(Status::Transport));
    }
    {
        StubHttp http;
        http.body = "{\"routes\":[{\"summary\":{\"distance\":386";   // cut short
        Client c(testConfig(), http);
        core::CycleRoute route;
        CHECK_EQ(static_cast<int>(c.route(aachenRide(), route)),
                 static_cast<int>(Status::BadResponse));
        CHECK(!route.valid);
    }
    {
        StubHttp http;
        Config noKey = testConfig();
        noKey.apiKey = "";
        Client c(noKey, http);
        core::CycleRoute route;
        CHECK_EQ(static_cast<int>(c.route(aachenRide(), route)),
                 static_cast<int>(Status::BadConfig));
        CHECK_EQ(http.posts, 0);
    }

    t::suite("ors / status names");
    CHECK_STR(statusName(Status::Ok), "ok");
    CHECK_STR(statusName(Status::QuotaExceeded), "quota");
    CHECK_STR(statusName(Status::NoRoute), "no route");
}
