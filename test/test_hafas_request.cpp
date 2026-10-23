#include "test.h"
#include "../src/hafas/hafas_request.h"
#include "../src/core/timeutil.h"
#include <cstring>
using namespace hafas;

static bool has(const char* hay, const char* needle) { return std::strstr(hay, needle) != nullptr; }

void test_hafas_request() {
    t::suite("hafas / url encoding");
    char buf[128];
    CHECK(urlEncode("simple", buf, sizeof buf) > 0);      CHECK_STR(buf, "simple");
    CHECK(urlEncode("a b", buf, sizeof buf) > 0);         CHECK_STR(buf, "a%20b");
    CHECK(urlEncode("Aachen, Ponttor", buf, sizeof buf) > 0);
    CHECK_STR(buf, "Aachen%2C%20Ponttor");
    CHECK(urlEncode("a&b=c", buf, sizeof buf) > 0);       CHECK_STR(buf, "a%26b%3Dc");
    // Umlauts are UTF-8 multibyte and must be encoded byte-wise.
    CHECK(urlEncode("K\xc3\xb6ln", buf, sizeof buf) > 0); CHECK_STR(buf, "K%C3%B6ln");
    // Buffer overflow must be reported, never truncated silently.
    char tiny[4];
    CHECK_EQ(urlEncode("abcdefgh", tiny, sizeof tiny), -1);

    t::suite("hafas / fixed-point formatting");
    CHECK(formatFixed(50.7753, 6, buf, sizeof buf) > 0);  CHECK_STR(buf, "50.775300");
    CHECK(formatFixed(6.0839, 6, buf, sizeof buf) > 0);   CHECK_STR(buf, "6.083900");
    CHECK(formatFixed(-1.5, 2, buf, sizeof buf) > 0);     CHECK_STR(buf, "-1.50");
    CHECK(formatFixed(0.0, 3, buf, sizeof buf) > 0);      CHECK_STR(buf, "0.000");
    // Rounds rather than truncating.
    CHECK(formatFixed(1.99999, 3, buf, sizeof buf) > 0);  CHECK_STR(buf, "2.000");

    t::suite("hafas / trip url");
    // A fake key with the real shape. The live AVV accessId belongs in .env
    // and nowhere else -- least of all in a source file that goes to git.
    Endpoint ep; ep.env = Env::Demo; ep.accessId = "00000000-1111-2222-3333-444444444444";
    TripQuery q;
    q.originLat = 50.7753; q.originLon = 6.0839;      // Aachen centre
    q.destLat   = 50.7660; q.destLon   = 6.0450;
    q.dateDays = core::parseDate("2026-08-13");
    q.timeOfDayMin = core::parseTimeOfDay("15:30");

    char url[768];
    const int n = buildTripUrl(ep, q, url, sizeof url);
    CHECK(n > 0);
    CHECK(has(url, "https://avv.demo.hafas.de/restproxy/trip?"));
    CHECK(has(url, "accessId=00000000-1111-2222-3333-444444444444"));
    CHECK(has(url, "format=json"));                     // XML is the default -- must be explicit
    CHECK(has(url, "originCoordLat=50.775300"));
    CHECK(has(url, "originCoordLong=6.083900"));
    CHECK(has(url, "destCoordLat=50.766000"));
    CHECK(has(url, "date=2026-08-13"));
    CHECK(has(url, "time=15:30"));
    CHECK(has(url, "searchForArrival=1"));

    // Response-size switches. These matter: measured against real AVV data,
    // Leg.Freq was 52% of an 89 KB response and we never display it. withFreq
    // defaults to 1 server-side, so omitting the parameter is not the same as
    // setting it to 0.
    CHECK(has(url, "withFreq=0"));
    CHECK(has(url, "tariff=0"));
    CHECK(has(url, "trafficMessages=0"));
    CHECK(has(url, "poly=0"));
    CHECK(has(url, "passlist=0"));

    // ...and they can be turned back on if a later feature needs them.
    {
        TripQuery fat = q;
        fat.withFreq = true;
        fat.tariff = true;
        char fatUrl[768];
        CHECK(buildTripUrl(ep, fat, fatUrl, sizeof fatUrl) > 0);
        CHECK(has(fatUrl, "withFreq=1"));
        CHECK(has(fatUrl, "tariff=1"));
        CHECK(has(fatUrl, "trafficMessages=0"));
    }
    CHECK(has(url, "rtMode=REALTIME"));
    CHECK(has(url, "originWalk=1"));
    CHECK(has(url, "destWalk=1"));
    CHECK(has(url, "numF=3"));
    CHECK(has(url, "poly=0"));
    CHECK(has(url, "passlist=0"));

    // Production base URL, same key.
    ep.env = Env::Prod;
    CHECK(buildTripUrl(ep, q, url, sizeof url) > 0);
    CHECK(has(url, "https://auskunft.avv.de/restproxy/trip?"));

    // numF is clamped to the documented 1..6 range.
    ep.env = Env::Demo;
    q.numF = 99; CHECK(buildTripUrl(ep, q, url, sizeof url) > 0); CHECK(has(url, "numF=6"));
    q.numF = 0;  CHECK(buildTripUrl(ep, q, url, sizeof url) > 0); CHECK(has(url, "numF=1"));
    q.numF = 3;

    // A too-small buffer fails loudly rather than emitting a truncated URL,
    // which would otherwise produce a confusing HTTP 400 at runtime.
    char small[40];
    CHECK_EQ(buildTripUrl(ep, q, small, sizeof small), -1);

    t::suite("hafas / other endpoints");
    CHECK(buildLocationNameUrl(ep, "Aachen, Ponttor", 5, url, sizeof url) > 0);
    CHECK(has(url, "location.name?"));
    CHECK(has(url, "input=Aachen%2C%20Ponttor"));
    CHECK(has(url, "maxNo=5"));

    CHECK(buildJourneyPosUrl(ep, "1|123|0|80|13082026", url, sizeof url) > 0);
    CHECK(has(url, "journeyPos?"));
    CHECK(has(url, "id=1%7C123%7C0%7C80%7C13082026"));
}
