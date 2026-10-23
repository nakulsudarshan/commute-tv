#include "test.h"
#include "../src/weather/meteo_client.h"

#include <cstdio>
#include <cstring>

using namespace meteo;

namespace {

struct StubHttp final : net::Client {
    int status = 200;
    net::Error error = net::Error::None;
    const char* body = "";
    char lastUrl[384] = {0};
    int gets = 0;
    size_t chunk = 9;

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

Query aachenAt(const char* date, const char* time) {
    Query q;
    q.lat = 50.7843;
    q.lon = 6.0526;
    q.at = core::parseDateTime(date, time);
    return q;
}

bool has(const char* hay, const char* needle) {
    return std::strstr(hay, needle) != nullptr;
}

} // namespace

void test_meteo() {
    t::suite("meteo / asking for one hour");
    {
        char url[384];
        Config cfg;
        CHECK(buildUrl(cfg, aachenAt("2026-08-25", "14:00"), url, sizeof url) > 0);
        CHECK(has(url, "latitude=50.7843"));
        CHECK(has(url, "longitude=6.0526"));
        CHECK(has(url, "hourly=temperature_2m,precipitation_probability,wind_speed_10m"));
        CHECK(has(url, "timezone=Europe%2FBerlin"));
        // start_hour keeps the response to a single value per variable, which
        // is what makes the parse trivial and the body a few hundred bytes.
        CHECK(has(url, "start_hour=2026-08-25T14%3A00"));
        CHECK(has(url, "end_hour=2026-08-25T15%3A00"));
        // No seconds, and no offset: `timezone` already says which zone these
        // are in, and sending both invites a disagreement.
        CHECK(!has(url, "T14%3A00%3A00"));
        CHECK(!has(url, "%2B02%3A00"));
    }
    {
        // The forecast is hourly, so a departure at 14:37 must ask for 14:00.
        // Asking for 14:37 returns nothing at all.
        char url[384];
        Config cfg;
        CHECK(buildUrl(cfg, aachenAt("2026-08-25", "14:37"), url, sizeof url) > 0);
        CHECK(has(url, "start_hour=2026-08-25T14%3A00"));
        CHECK(has(url, "end_hour=2026-08-25T15%3A00"));
    }
    {
        // Crossing midnight rolls the date, like everywhere else in this code.
        char url[384];
        Config cfg;
        CHECK(buildUrl(cfg, aachenAt("2026-08-25", "23:30"), url, sizeof url) > 0);
        CHECK(has(url, "start_hour=2026-08-25T23%3A00"));
        CHECK(has(url, "end_hour=2026-08-26T00%3A00"));
    }
    {
        char url[384];
        Config cfg;
        Query bad;                       // no timestamp
        CHECK_EQ(buildUrl(cfg, bad, url, sizeof url), -1);
        Query offEarth = aachenAt("2026-08-25", "14:00");
        offEarth.lat = 91.0;
        CHECK_EQ(buildUrl(cfg, offEarth, url, sizeof url), -1);
        char tiny[32];
        CHECK_EQ(buildUrl(cfg, aachenAt("2026-08-25", "14:00"), tiny, sizeof tiny), -1);
    }

    t::suite("meteo / reading the forecast");
    {
        StubHttp http;
        http.body = R"JSON({
          "latitude": 50.78, "longitude": 6.05, "timezone": "Europe/Berlin",
          "hourly_units": {"temperature_2m":"°C","wind_speed_10m":"km/h"},
          "hourly": {
            "time": ["2026-08-25T14:00","2026-08-25T15:00"],
            "temperature_2m": [21.4, 22.1],
            "precipitation_probability": [15, 40],
            "wind_speed_10m": [11.2, 13.0]
          }
        })JSON";
        Config cfg;
        Client c(cfg, http);
        core::Weather w;
        CHECK_EQ(static_cast<int>(c.fetch(aachenAt("2026-08-25", "14:00"), w)),
                 static_cast<int>(Status::Ok));
        CHECK(w.valid);
        // The hour asked for, not simply the first one in the array.
        CHECK_NEAR(w.tempC, 21.4, 0.01);
        CHECK_NEAR(w.precipProbPct, 15.0, 0.01);
        CHECK_NEAR(w.windKph, 11.2, 0.01);
    }
    {
        // The second hour, to prove the index is chosen and not assumed.
        StubHttp http;
        http.body = "{\"hourly\":{\"time\":[\"2026-08-25T14:00\",\"2026-08-25T15:00\"],"
                    "\"temperature_2m\":[21.4,22.1],"
                    "\"precipitation_probability\":[15,40],"
                    "\"wind_speed_10m\":[11.2,13.0]}}";
        Config cfg;
        Client c(cfg, http);
        core::Weather w;
        CHECK_EQ(static_cast<int>(c.fetch(aachenAt("2026-08-25", "15:00"), w)),
                 static_cast<int>(Status::Ok));
        CHECK_NEAR(w.tempC, 22.1, 0.01);
        CHECK_NEAR(w.precipProbPct, 40.0, 0.01);
    }
    {
        // A single-hour response, which is what start_hour normally produces.
        StubHttp http;
        http.body = "{\"hourly\":{\"time\":[\"2026-08-25T14:00\"],"
                    "\"temperature_2m\":[19.0],"
                    "\"precipitation_probability\":[5],"
                    "\"wind_speed_10m\":[7.5]}}";
        Config cfg;
        Client c(cfg, http);
        core::Weather w;
        CHECK_EQ(static_cast<int>(c.fetch(aachenAt("2026-08-25", "14:00"), w)),
                 static_cast<int>(Status::Ok));
        CHECK_NEAR(w.tempC, 19.0, 0.01);
    }
    {
        // Negative temperatures are the ones that matter most to the score.
        StubHttp http;
        http.body = "{\"hourly\":{\"time\":[\"2026-01-15T08:00\"],"
                    "\"temperature_2m\":[-4.5],"
                    "\"precipitation_probability\":[80],"
                    "\"wind_speed_10m\":[26.4]}}";
        Config cfg;
        Client c(cfg, http);
        core::Weather w;
        CHECK_EQ(static_cast<int>(c.fetch(aachenAt("2026-01-15", "08:00"), w)),
                 static_cast<int>(Status::Ok));
        CHECK_NEAR(w.tempC, -4.5, 0.01);
        CHECK_NEAR(w.windKph, 26.4, 0.01);
    }

    t::suite("meteo / a null is not a zero");
    {
        // Open-Meteo returns null for a variable a model does not carry.
        // Reading that as 0 would turn "we do not know if it will rain" into
        // "it will not rain" -- and the comfort score would recommend the bike.
        StubHttp http;
        http.body = "{\"hourly\":{\"time\":[\"2026-08-25T14:00\"],"
                    "\"temperature_2m\":[18.0],"
                    "\"precipitation_probability\":[null],"
                    "\"wind_speed_10m\":[null]}}";
        Config cfg;
        Client c(cfg, http);
        core::Weather w;
        CHECK_EQ(static_cast<int>(c.fetch(aachenAt("2026-08-25", "14:00"), w)),
                 static_cast<int>(Status::Ok));
        CHECK(w.valid);
        CHECK_NEAR(w.tempC, 18.0, 0.01);
        // Left at the struct's neutral defaults rather than a measured zero.
        CHECK_NEAR(w.precipProbPct, 0.0, 0.01);
        CHECK_NEAR(w.windKph, 0.0, 0.01);
    }
    {
        // No temperature at all is not survivable: the score would have to
        // guess, and guessing is what this project does not do.
        StubHttp http;
        http.body = "{\"hourly\":{\"time\":[\"2026-08-25T14:00\"],"
                    "\"temperature_2m\":[null],"
                    "\"precipitation_probability\":[10]}}";
        Config cfg;
        Client c(cfg, http);
        core::Weather w;
        CHECK_EQ(static_cast<int>(c.fetch(aachenAt("2026-08-25", "14:00"), w)),
                 static_cast<int>(Status::NoData));
        CHECK(!w.valid);
    }

    t::suite("meteo / failures");
    {
        // The hour asked for is simply not in the reply.
        StubHttp http;
        http.body = "{\"hourly\":{\"time\":[\"2026-08-25T09:00\",\"2026-08-25T10:00\"],"
                    "\"temperature_2m\":[12.0,13.0]}}";
        Config cfg;
        Client c(cfg, http);
        core::Weather w;
        CHECK_EQ(static_cast<int>(c.fetch(aachenAt("2026-08-25", "14:00"), w)),
                 static_cast<int>(Status::NoData));
        CHECK(!w.valid);
    }
    {
        StubHttp http;
        http.status = 400;
        http.body = "{\"error\":true,\"reason\":\"Value cannot be parsed\"}";
        Config cfg;
        Client c(cfg, http);
        core::Weather w;
        CHECK_EQ(static_cast<int>(c.fetch(aachenAt("2026-08-25", "14:00"), w)),
                 static_cast<int>(Status::HttpError));
        CHECK_STR(c.lastError(), "Value cannot be parsed");
    }
    {
        StubHttp http;
        http.error = net::Error::Unreachable;
        Config cfg;
        Client c(cfg, http);
        core::Weather w;
        CHECK_EQ(static_cast<int>(c.fetch(aachenAt("2026-08-25", "14:00"), w)),
                 static_cast<int>(Status::Transport));
    }
    {
        StubHttp http;
        http.body = "{\"hourly\":{\"time\":[\"2026-08-25T14:00\"],\"temperature_2m\":[21";
        Config cfg;
        Client c(cfg, http);
        core::Weather w;
        CHECK_EQ(static_cast<int>(c.fetch(aachenAt("2026-08-25", "14:00"), w)),
                 static_cast<int>(Status::BadResponse));
        CHECK(!w.valid);
    }

    t::suite("meteo / status names");
    CHECK_STR(statusName(Status::Ok), "ok");
    CHECK_STR(statusName(Status::NoData), "no data");
}
