#include "test.h"
#include "../src/google/calendar.h"
#include "../src/google/jwt.h"
#include "../src/core/timeutil.h"
#include "../src/core/urlencode.h"

#include <cstdio>
#include <cstring>

using namespace google;

namespace {

// --- stubs -------------------------------------------------------------------

// Records exactly what it was asked to sign. The RSA itself is mbedTLS's
// problem on the device; what this project can get wrong is *what* gets signed,
// and that is what this captures.
struct StubSigner final : Signer {
    char lastInput[1024] = {0};
    size_t lastInputLen = 0;
    int calls = 0;
    bool fail = false;
    size_t sigLen = 256;                    // RSA-2048

    size_t signRs256(const uint8_t* data, size_t len, uint8_t* out, size_t outCap) override {
        ++calls;
        lastInputLen = len < sizeof lastInput - 1 ? len : sizeof lastInput - 1;
        std::memcpy(lastInput, data, lastInputLen);
        lastInput[lastInputLen] = '\0';
        if (fail || outCap < sigLen) return 0;
        // A deterministic, obviously-not-random signature: byte i = i.
        for (size_t i = 0; i < sigLen; ++i) out[i] = static_cast<uint8_t>(i & 0xFF);
        return sigLen;
    }
};

// Serves canned responses, in small chunks, so every client is exercised
// against a body that arrives in pieces rather than all at once.
struct StubHttp final : net::Client {
    struct Canned {
        int status = 200;
        net::Error error = net::Error::None;
        const char* body = "";
    };

    Canned tokenReply;
    Canned eventsReply;
    Canned eventsReply2;                    // used after a 401 retry
    bool useSecondEventsReply = false;

    int getCalls = 0, postCalls = 0;
    char lastUrl[768] = {0};
    char lastAuth[128] = {0};
    char lastBody[1400] = {0};
    char lastContentType[64] = {0};
    size_t chunk = 7;

    net::Result deliver(const Canned& c, net::BodySink& sink) {
        net::Result r;
        r.status = c.status;
        r.error = c.error;
        if (c.error != net::Error::None) return r;

        const size_t n = std::strlen(c.body);
        for (size_t i = 0; i < n; i += chunk) {
            const size_t take = (n - i < chunk) ? (n - i) : chunk;
            if (!sink.write(c.body + i, take)) {
                r.error = net::Error::Aborted;
                r.bytes = i + take;
                return r;
            }
        }
        r.bytes = n;
        return r;
    }

    net::Result get(const char* url, const net::Header* headers, size_t headerCount,
                    net::BodySink& body) override {
        ++getCalls;
        std::snprintf(lastUrl, sizeof lastUrl, "%s", url);
        lastAuth[0] = '\0';
        for (size_t i = 0; i < headerCount; ++i)
            if (std::strcmp(headers[i].name, "Authorization") == 0)
                std::snprintf(lastAuth, sizeof lastAuth, "%s", headers[i].value);
        const Canned& c = useSecondEventsReply ? eventsReply2 : eventsReply;
        return deliver(c, body);
    }

    net::Result post(const char* url, const char* contentType,
                     const char* body, size_t bodyLen,
                     const net::Header*, size_t, net::BodySink& out) override {
        ++postCalls;
        std::snprintf(lastUrl, sizeof lastUrl, "%s", url);
        std::snprintf(lastContentType, sizeof lastContentType, "%s", contentType);
        const size_t n = bodyLen < sizeof lastBody - 1 ? bodyLen : sizeof lastBody - 1;
        std::memcpy(lastBody, body, n);
        lastBody[n] = '\0';
        return deliver(tokenReply, out);
    }
};

Config testConfig() {
    Config c;
    c.clientEmail = "commute-tv-calendar@commute-tv.iam.gserviceaccount.com";
    c.calendarId = "abc123@group.calendar.google.com";
    return c;
}

bool has(const char* hay, const char* needle) {
    return std::strstr(hay, needle) != nullptr;
}

// Pulls the base64url middle segment of a JWT and decodes it far enough to
// check the claims are there. Decoding properly is not the point; the point is
// that the claim set is what we think it is.
void jwtPart(const char* jwt, int index, char* out, size_t outCap) {
    const char* p = jwt;
    for (int i = 0; i < index; ++i) {
        p = std::strchr(p, '.');
        if (!p) { out[0] = '\0'; return; }
        ++p;
    }
    const char* end = std::strchr(p, '.');
    size_t n = end ? static_cast<size_t>(end - p) : std::strlen(p);
    if (n >= outCap) n = outCap - 1;
    std::memcpy(out, p, n);
    out[n] = '\0';
}

size_t b64UrlDecode(const char* in, char* out, size_t outCap) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };
    size_t w = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (const char* p = in; *p; ++p) {
        const int v = val(*p);
        if (v < 0) continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (w + 1 < outCap) out[w++] = static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    out[w] = '\0';
    return w;
}

const char* kEventsBody = R"JSON({
  "kind": "calendar#events",
  "items": [
    {
      "summary": "Zahnarzttermin",
      "location": "Aachen, Hörn Brücke",
      "status": "confirmed",
      "start": { "dateTime": "2026-08-24T15:30:00+02:00", "timeZone": "Europe/Berlin" },
      "end":   { "dateTime": "2026-08-24T16:00:00+02:00", "timeZone": "Europe/Berlin" }
    },
    {
      "summary": "Standup",
      "status": "confirmed",
      "start": { "dateTime": "2026-08-24T09:00:00+02:00" }
    },
    {
      "summary": "Urlaub",
      "location": "Bruessel",
      "status": "confirmed",
      "start": { "date": "2026-08-25" }
    },
    {
      "summary": "Abgesagt",
      "location": "Aachen, Ponttor",
      "status": "cancelled",
      "start": { "dateTime": "2026-08-24T18:00:00+02:00" }
    },
    {
      "summary": "Kein Ort mehr",
      "location": "",
      "status": "confirmed",
      "start": { "dateTime": "2026-08-24T19:00:00+02:00" }
    },
    {
      "summary": "Vorlesung",
      "location": "RWTH Hauptgebäude, Templergraben 55",
      "status": "confirmed",
      "start": { "dateTime": "2026-08-24T11:00:00+02:00" }
    }
  ]
})JSON";

} // namespace

void test_google() {
    t::suite("google / base64url");
    {
        char b[64];
        auto enc = [&](const char* s) {
            base64UrlEncode(reinterpret_cast<const uint8_t*>(s), std::strlen(s), b, sizeof b);
            return b;
        };
        CHECK_STR(enc(""), "");
        CHECK_STR(enc("f"), "Zg");
        CHECK_STR(enc("fo"), "Zm8");
        CHECK_STR(enc("foo"), "Zm9v");
        CHECK_STR(enc("foob"), "Zm9vYg");
        CHECK_STR(enc("fooba"), "Zm9vYmE");
        CHECK_STR(enc("foobar"), "Zm9vYmFy");

        // The two characters that differ from standard base64. Getting these
        // wrong yields "Invalid JWT" from Google and no clue which half is bad.
        const uint8_t urlChars[] = {0xFB, 0xFF};
        base64UrlEncode(urlChars, 2, b, sizeof b);
        CHECK_STR(b, "-_8");
        CHECK(std::strchr(b, '+') == nullptr);
        CHECK(std::strchr(b, '/') == nullptr);
        // And no padding, ever.
        base64UrlEncode(reinterpret_cast<const uint8_t*>("f"), 1, b, sizeof b);
        CHECK(std::strchr(b, '=') == nullptr);

        // A buffer that does not fit reports failure rather than truncating: a
        // truncated signature is a signature over nothing.
        char tiny[4];
        CHECK_EQ(base64UrlEncode(reinterpret_cast<const uint8_t*>("foobar"), 6, tiny, sizeof tiny), 0u);
        CHECK_STR(tiny, "");
    }

    t::suite("google / the JWT claim set");
    {
        char jwt[1024];
        const int n = buildSigningInput("sa@project.iam.gserviceaccount.com",
                                        "https://www.googleapis.com/auth/calendar.readonly",
                                        "https://oauth2.googleapis.com/token",
                                        1787577000, 3600, jwt, sizeof jwt);
        CHECK(n > 0);

        char part[512], decoded[512];
        jwtPart(jwt, 0, part, sizeof part);
        b64UrlDecode(part, decoded, sizeof decoded);
        CHECK_STR(decoded, "{\"alg\":\"RS256\",\"typ\":\"JWT\"}");

        jwtPart(jwt, 1, part, sizeof part);
        b64UrlDecode(part, decoded, sizeof decoded);
        CHECK(has(decoded, "\"iss\":\"sa@project.iam.gserviceaccount.com\""));
        CHECK(has(decoded, "\"aud\":\"https://oauth2.googleapis.com/token\""));
        CHECK(has(decoded, "\"scope\":\"https://www.googleapis.com/auth/calendar.readonly\""));
        CHECK(has(decoded, "\"iat\":1787577000"));
        CHECK(has(decoded, "\"exp\":1787580600"));
        // No `sub`: impersonation needs domain-wide delegation, which a
        // consumer account cannot grant. Its presence would be a silent 401.
        CHECK(!has(decoded, "\"sub\""));

        // Exactly two segments before signing, and no stray padding.
        int dots = 0;
        for (const char* p = jwt; *p; ++p) if (*p == '.') ++dots;
        CHECK_EQ(dots, 1);
    }
    {
        // Google caps the assertion at one hour. Asking for more must be
        // clamped rather than rejected by the server an hour into a debugging
        // session.
        char jwt[1024], part[512], decoded[512];
        buildSigningInput("a@b.c", "s", "aud", 1000, 99999, jwt, sizeof jwt);
        jwtPart(jwt, 1, part, sizeof part);
        b64UrlDecode(part, decoded, sizeof decoded);
        CHECK(has(decoded, "\"iat\":1000"));
        CHECK(has(decoded, "\"exp\":4600"));
        // A non-positive lifetime is a configuration mistake, not a request for
        // an already-expired token.
        buildSigningInput("a@b.c", "s", "aud", 1000, 0, jwt, sizeof jwt);
        jwtPart(jwt, 1, part, sizeof part);
        b64UrlDecode(part, decoded, sizeof decoded);
        CHECK(has(decoded, "\"exp\":4600"));
    }
    {
        char tiny[32];
        CHECK_EQ(buildSigningInput("a@b.c", "s", "aud", 1000, 60, tiny, sizeof tiny), -1);
    }

    t::suite("google / the signed assertion");
    {
        StubSigner signer;
        char jwt[1024];
        const int n = buildAssertion("sa@p.iam.gserviceaccount.com", "scope",
                                     "https://oauth2.googleapis.com/token",
                                     1787577000, 3600, signer, jwt, sizeof jwt);
        CHECK(n > 0);
        CHECK_EQ(signer.calls, 1);

        // What was signed must be exactly the header.claims prefix of the
        // finished token -- not a rebuild of it, which could differ.
        char expected[1024];
        buildSigningInput("sa@p.iam.gserviceaccount.com", "scope",
                          "https://oauth2.googleapis.com/token",
                          1787577000, 3600, expected, sizeof expected);
        CHECK_STR(signer.lastInput, expected);
        CHECK_EQ(std::strncmp(jwt, expected, std::strlen(expected)), 0);
        CHECK_EQ(jwt[std::strlen(expected)], '.');

        int dots = 0;
        for (const char* p = jwt; *p; ++p) if (*p == '.') ++dots;
        CHECK_EQ(dots, 2);
    }
    {
        StubSigner signer;
        signer.fail = true;
        char jwt[1024];
        CHECK_EQ(buildAssertion("a@b.c", "s", "aud", 1000, 60, signer, jwt, sizeof jwt), -1);
    }

    t::suite("google / the token request body");
    {
        // The literal prefix in calendar.cpp must be exactly what urlEncode
        // produces, or the two drift apart silently.
        char enc[128];
        CHECK(core::urlEncode("urn:ietf:params:oauth:grant-type:jwt-bearer",
                              enc, sizeof enc) > 0);
        char expected[192];
        std::snprintf(expected, sizeof expected, "grant_type=%s&assertion=", enc);
        CHECK_STR(kTokenGrantPrefix, expected);

        char body[512];
        CHECK(buildTokenRequestBody("header.claims.sig", body, sizeof body) > 0);
        CHECK_STR(body, "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer"
                        "&assertion=header.claims.sig");
    }

    t::suite("google / the events url");
    {
        Config cfg = testConfig();
        Query q;
        q.from = core::parseDateTime("2026-08-24", "13:00");
        q.to   = core::parseDateTime("2026-08-25", "13:00");
        q.utcOffsetMin = 120;
        q.maxResults = 6;

        char url[512];
        CHECK(buildEventsUrl(cfg, q, url, sizeof url) > 0);

        // The bug this test exists for: '+' in an RFC 3339 offset decodes as a
        // space unless escaped, which shifts the window by an hour or makes the
        // timestamp invalid.
        CHECK(has(url, "timeMin=2026-08-24T13%3A00%3A00%2B02%3A00"));
        CHECK(has(url, "timeMax=2026-08-25T13%3A00%3A00%2B02%3A00"));
        CHECK(std::strstr(url, "timeMin=2026-08-24T13:00:00+02:00") == nullptr);

        CHECK(has(url, "/calendars/abc123%40group.calendar.google.com/events"));
        CHECK(has(url, "singleEvents=true"));
        CHECK(has(url, "orderBy=startTime"));    // requires singleEvents
        CHECK(has(url, "showDeleted=false"));
        CHECK(has(url, "maxResults=6"));
        CHECK(has(url, "timeZone=Europe%2FBerlin"));
        CHECK(has(url, "fields=items%28summary%2Clocation%2Cstatus%2Cstart%29%2CnextPageToken"));

        // Negative offsets, and a winter offset, both format correctly.
        q.utcOffsetMin = 60;
        CHECK(buildEventsUrl(cfg, q, url, sizeof url) > 0);
        CHECK(has(url, "T13%3A00%3A00%2B01%3A00"));
        q.utcOffsetMin = -330;
        CHECK(buildEventsUrl(cfg, q, url, sizeof url) > 0);
        CHECK(has(url, "T13%3A00%3A00-05%3A30"));

        // maxResults is clamped to what Google accepts.
        q.maxResults = 9000;
        CHECK(buildEventsUrl(cfg, q, url, sizeof url) > 0);
        CHECK(has(url, "maxResults=250"));
        q.maxResults = 0;
        CHECK(buildEventsUrl(cfg, q, url, sizeof url) > 0);
        CHECK(has(url, "maxResults=1"));

        // Refuses rather than emitting a URL that would return the wrong window.
        Query bad;
        CHECK_EQ(buildEventsUrl(cfg, bad, url, sizeof url), -1);
        char tiny[32];
        CHECK_EQ(buildEventsUrl(cfg, q, tiny, sizeof tiny), -1);
        Config noCal = cfg;
        noCal.calendarId = "";
        CHECK_EQ(buildEventsUrl(noCal, q, url, sizeof url), -1);
    }

    t::suite("google / token exchange");
    {
        StubSigner signer;
        StubHttp http;
        Token token;
        http.tokenReply.body = "{\"access_token\":\"ya29.a0AfH6SMB-example-token\","
                               "\"expires_in\":3599,\"token_type\":\"Bearer\","
                               "\"scope\":\"https://www.googleapis.com/auth/calendar.readonly\"}";
        Client c(testConfig(), http, signer, token);

        CHECK_EQ(static_cast<int>(c.ensureToken(1787577000)), static_cast<int>(Status::Ok));
        CHECK_EQ(http.postCalls, 1);
        CHECK_STR(http.lastUrl, "https://oauth2.googleapis.com/token");
        CHECK_STR(http.lastContentType, "application/x-www-form-urlencoded");
        CHECK(has(http.lastBody, "grant_type=urn%3Aietf"));
        CHECK(has(http.lastBody, "&assertion="));
        CHECK_STR(token.value(), "ya29.a0AfH6SMB-example-token");
        CHECK_STR(token.authorization(), "Bearer ya29.a0AfH6SMB-example-token");
        CHECK_EQ(token.expiresAtUtcSec, 1787577000 + 3599);

        // A held token is reused until the refresh margin, then re-minted.
        CHECK_EQ(static_cast<int>(c.ensureToken(1787577100)), static_cast<int>(Status::Ok));
        CHECK_EQ(http.postCalls, 1);
        CHECK_EQ(static_cast<int>(c.ensureToken(1787577000 + 3599 - 299)), static_cast<int>(Status::Ok));
        CHECK_EQ(http.postCalls, 2);
    }
    {
        // Google documents no maximum token length. A long one must survive
        // whole: the reader's default 128-byte value buffer would have
        // truncated it into a token that authenticates nothing.
        StubSigner signer;
        StubHttp http;
        Token token;
        char big[2200];
        char body[2400];
        std::memset(big, 'A', 1800);
        big[1800] = '\0';
        std::snprintf(body, sizeof body, "{\"access_token\":\"%s\",\"expires_in\":3599}", big);
        http.tokenReply.body = body;
        Client c(testConfig(), http, signer, token);
        CHECK_EQ(static_cast<int>(c.ensureToken(1000)), static_cast<int>(Status::Ok));
        CHECK_EQ(std::strlen(token.value()), 1800u);
        CHECK_EQ(std::strncmp(token.value(), big, 1800), 0);
    }
    {
        // Google's own error text is the difference between "the key is wrong"
        // and "the clock is wrong", so it is kept.
        StubSigner signer;
        StubHttp http;
        Token token;
        http.tokenReply.status = 400;
        http.tokenReply.body = "{\"error\":\"invalid_grant\","
                               "\"error_description\":\"Invalid JWT Signature.\"}";
        Client c(testConfig(), http, signer, token);
        CHECK_EQ(static_cast<int>(c.ensureToken(1000)), static_cast<int>(Status::AuthFailed));
        CHECK_STR(c.lastGoogleError(), "invalid_grant");
        CHECK(!token.usable(1000, 0));
    }
    {
        // A token response with no expires_in must not be treated as expiring
        // immediately -- that would mint a new token on every single request.
        StubSigner signer;
        StubHttp http;
        Token token;
        http.tokenReply.body = "{\"access_token\":\"tok\",\"token_type\":\"Bearer\"}";
        Client c(testConfig(), http, signer, token);
        CHECK_EQ(static_cast<int>(c.ensureToken(1000)), static_cast<int>(Status::Ok));
        CHECK_EQ(token.expiresAtUtcSec, 1000 + 3600);
    }
    {
        // "3.9" is not a lifetime. Taking its integral part would make the
        // token expire in three seconds and re-mint one on every poll -- and
        // the plausibility fallback would never fire, because 3 looks valid.
        StubSigner signer;
        StubHttp http;
        Token token;
        http.tokenReply.body = "{\"access_token\":\"tok\",\"expires_in\":3.9}";
        Client c(testConfig(), http, signer, token);
        CHECK_EQ(static_cast<int>(c.ensureToken(1000)), static_cast<int>(Status::Ok));
        CHECK_EQ(token.expiresAtUtcSec, 1000 + 3600);

        // And a value past what a 32-bit long can hold must not wrap into
        // something plausible: 4294967596 wraps to 300, which passes every
        // range check there is.
        StubHttp http3;
        Token token3;
        http3.tokenReply.body = "{\"access_token\":\"tok\",\"expires_in\":4294967596}";
        Client c3(testConfig(), http3, signer, token3);
        CHECK_EQ(static_cast<int>(c3.ensureToken(1000)), static_cast<int>(Status::Ok));
        CHECK_EQ(token3.expiresAtUtcSec, 1000 + 3600);
    }
    {
        StubSigner signer;
        StubHttp http;
        Token token;
        http.tokenReply.error = net::Error::Timeout;
        Client c(testConfig(), http, signer, token);
        CHECK_EQ(static_cast<int>(c.ensureToken(1000)), static_cast<int>(Status::Transport));

        StubHttp http2;
        Token token2;
        http2.tokenReply.body = "{\"access_token\":\"tok\",\"expires_in\":36";  // cut off
        Client c2(testConfig(), http2, signer, token2);
        CHECK_EQ(static_cast<int>(c2.ensureToken(1000)), static_cast<int>(Status::BadResponse));
    }

    t::suite("google / reading the calendar");
    {
        StubSigner signer;
        StubHttp http;
        Token token;
        http.tokenReply.body = "{\"access_token\":\"tok\",\"expires_in\":3599}";
        http.eventsReply.body = kEventsBody;
        Client c(testConfig(), http, signer, token);

        Query q;
        q.from = core::parseDateTime("2026-08-24", "08:00");
        q.to   = core::parseDateTime("2026-08-25", "08:00");
        q.utcOffsetMin = 120;

        EventList list;
        CHECK_EQ(static_cast<int>(c.listEvents(q, 1000, list)), static_cast<int>(Status::Ok));
        CHECK_STR(http.lastAuth, "Bearer tok");

        // Six events in, two kept: only the ones that name a place at a time.
        CHECK_EQ(list.seen, 6);
        CHECK_EQ(list.count, 2);
        CHECK_EQ(list.skippedNoLocation, 2);   // no location field, and location:""
        CHECK_EQ(list.skippedAllDay, 1);
        CHECK_EQ(list.skippedCancelled, 1);
        CHECK(!list.more);

        // Order is the order Google sent, which orderBy=startTime made
        // chronological -- the parser must not reorder or drop into gaps.
        CHECK_STR(list.events[0].title, "Zahnarzttermin");
        CHECK_STR(list.events[0].location, "Aachen, Hoern Bruecke");   // folded for the font
        CHECK_EQ(list.events[0].start.min, core::parseDateTime("2026-08-24", "15:30").min);
        CHECK(!list.events[0].allDay);
        CHECK(list.events[0].actionable());

        CHECK_STR(list.events[1].title, "Vorlesung");
        CHECK_STR(list.events[1].location, "RWTH Hauptgebaeude, Templergraben 55");
        CHECK_EQ(list.events[1].start.min, core::parseDateTime("2026-08-24", "11:00").min);
        CHECK(list.events[1].actionable());
    }
    {
        // More matching events than we can carry is worth knowing about: it is
        // the only one of these counters that could hide the appointment that
        // mattered.
        StubSigner signer;
        StubHttp http;
        Token token;
        http.tokenReply.body = "{\"access_token\":\"tok\",\"expires_in\":3599}";

        char body[4096];
        size_t w = 0;
        w += static_cast<size_t>(std::snprintf(body + w, sizeof body - w, "{\"items\":["));
        for (int i = 0; i < 9; ++i)
            w += static_cast<size_t>(std::snprintf(body + w, sizeof body - w,
                "%s{\"summary\":\"E%d\",\"location\":\"Ponttor\",\"status\":\"confirmed\","
                "\"start\":{\"dateTime\":\"2026-08-24T1%d:00:00+02:00\"}}", i ? "," : "", i, i));
        w += static_cast<size_t>(std::snprintf(body + w, sizeof body - w, "]}"));
        http.eventsReply.body = body;

        Client c(testConfig(), http, signer, token);
        Query q;
        q.from = core::parseDateTime("2026-08-24", "08:00");
        q.to = core::parseDateTime("2026-08-25", "08:00");
        EventList list;
        CHECK_EQ(static_cast<int>(c.listEvents(q, 1000, list)), static_cast<int>(Status::Ok));
        CHECK_EQ(list.count, kMaxEvents);
        CHECK_EQ(list.seen, 9);
        CHECK(list.more);
    }
    {
        // A nextPageToken means the same thing: there was more than we asked for.
        StubSigner signer;
        StubHttp http;
        Token token;
        http.tokenReply.body = "{\"access_token\":\"tok\",\"expires_in\":3599}";
        http.eventsReply.body = "{\"items\":[],\"nextPageToken\":\"CigKGjBpMD\"}";
        Client c(testConfig(), http, signer, token);
        Query q;
        q.from = core::parseDateTime("2026-08-24", "08:00");
        q.to = core::parseDateTime("2026-08-25", "08:00");
        EventList list;
        CHECK_EQ(static_cast<int>(c.listEvents(q, 1000, list)), static_cast<int>(Status::Ok));
        CHECK_EQ(list.count, 0);
        CHECK(list.more);
        CHECK(list.empty());
    }
    {
        // A revoked token: one retry with a fresh one, and exactly one.
        StubSigner signer;
        StubHttp http;
        Token token;
        http.tokenReply.body = "{\"access_token\":\"tok\",\"expires_in\":3599}";
        http.eventsReply.status = 401;
        http.eventsReply.body = "{\"error\":{\"code\":401,\"message\":\"Invalid Credentials\"}}";
        http.eventsReply2.status = 200;
        http.eventsReply2.body = "{\"items\":[{\"summary\":\"T\",\"location\":\"Ponttor\","
                                 "\"status\":\"confirmed\","
                                 "\"start\":{\"dateTime\":\"2026-08-24T12:00:00+02:00\"}}]}";

        struct Flip final : net::Client {
            StubHttp* inner;
            net::Result get(const char* url, const net::Header* h, size_t n,
                            net::BodySink& b) override {
                const net::Result r = inner->get(url, h, n, b);
                inner->useSecondEventsReply = true;   // the retry sees a good reply
                return r;
            }
            net::Result post(const char* url, const char* ct, const char* body, size_t len,
                             const net::Header* h, size_t n, net::BodySink& o) override {
                return inner->post(url, ct, body, len, h, n, o);
            }
        } flip;
        flip.inner = &http;

        Client c(testConfig(), flip, signer, token);
        Query q;
        q.from = core::parseDateTime("2026-08-24", "08:00");
        q.to = core::parseDateTime("2026-08-25", "08:00");
        EventList list;
        CHECK_EQ(static_cast<int>(c.listEvents(q, 1000, list)), static_cast<int>(Status::Ok));
        CHECK_EQ(http.getCalls, 2);            // original plus one retry
        CHECK_EQ(http.postCalls, 2);           // and a fresh token for it
        CHECK_EQ(list.count, 1);
    }
    {
        // A 401 that stays a 401 must give up rather than loop.
        StubSigner signer;
        StubHttp http;
        Token token;
        http.tokenReply.body = "{\"access_token\":\"tok\",\"expires_in\":3599}";
        http.eventsReply.status = 401;
        http.eventsReply.body = "{\"error\":{\"code\":401,\"message\":\"Invalid Credentials\"}}";
        Client c(testConfig(), http, signer, token);
        Query q;
        q.from = core::parseDateTime("2026-08-24", "08:00");
        q.to = core::parseDateTime("2026-08-25", "08:00");
        EventList list;
        CHECK_EQ(static_cast<int>(c.listEvents(q, 1000, list)), static_cast<int>(Status::AuthFailed));
        CHECK_EQ(http.getCalls, 2);
    }
    {
        // A body cut off mid-document is a failure, never a short list.
        StubSigner signer;
        StubHttp http;
        Token token;
        http.tokenReply.body = "{\"access_token\":\"tok\",\"expires_in\":3599}";
        http.eventsReply.body = "{\"items\":[{\"summary\":\"A\",\"location\":\"P\",";
        Client c(testConfig(), http, signer, token);
        Query q;
        q.from = core::parseDateTime("2026-08-24", "08:00");
        q.to = core::parseDateTime("2026-08-25", "08:00");
        EventList list;
        CHECK_EQ(static_cast<int>(c.listEvents(q, 1000, list)), static_cast<int>(Status::BadResponse));
        // Nothing partial escapes. The body had one complete event in it before
        // it stopped; a caller checking emptiness rather than status would
        // otherwise render half a day's appointments as the whole truth.
        CHECK_EQ(list.count, 0);
        CHECK(list.empty());
    }
    {
        // Same rule for an auth failure that carries an error document.
        StubSigner signer;
        StubHttp http;
        Token token;
        http.tokenReply.body = "{\"access_token\":\"tok\",\"expires_in\":3599}";
        http.eventsReply.status = 403;
        http.eventsReply.body = "{\"items\":[{\"summary\":\"A\",\"location\":\"P\","
                                "\"status\":\"confirmed\","
                                "\"start\":{\"dateTime\":\"2026-08-24T12:00:00+02:00\"}}],"
                                "\"error\":{\"code\":403,\"message\":\"Forbidden\"}}";
        Client c(testConfig(), http, signer, token);
        Query q;
        q.from = core::parseDateTime("2026-08-24", "08:00");
        q.to = core::parseDateTime("2026-08-25", "08:00");
        EventList list;
        CHECK_EQ(static_cast<int>(c.listEvents(q, 1000, list)), static_cast<int>(Status::AuthFailed));
        CHECK_EQ(list.count, 0);
    }
    {
        // An event that names a place but whose start will not parse is a
        // parsing problem, not a missing location -- counted separately so the
        // health panel does not send the next person looking in the wrong place.
        StubSigner signer;
        StubHttp http;
        Token token;
        http.tokenReply.body = "{\"access_token\":\"tok\",\"expires_in\":3599}";
        http.eventsReply.body = "{\"items\":[{\"summary\":\"A\",\"location\":\"Ponttor\","
                                "\"status\":\"confirmed\","
                                "\"start\":{\"dateTime\":\"not-a-time\"}}]}";
        Client c(testConfig(), http, signer, token);
        Query q;
        q.from = core::parseDateTime("2026-08-24", "08:00");
        q.to = core::parseDateTime("2026-08-25", "08:00");
        EventList list;
        CHECK_EQ(static_cast<int>(c.listEvents(q, 1000, list)), static_cast<int>(Status::Ok));
        CHECK_EQ(list.count, 0);
        CHECK_EQ(list.skippedNoStart, 1);
        CHECK_EQ(list.skippedNoLocation, 0);
    }
    {
        // A far-future all-day event is something a person can actually type
        // into Google Calendar, and days * 1440 overflows an int32 past about
        // the year 6000. Dropped cleanly rather than undefined.
        StubSigner signer;
        StubHttp http;
        Token token;
        http.tokenReply.body = "{\"access_token\":\"tok\",\"expires_in\":3599}";
        http.eventsReply.body = "{\"items\":[{\"summary\":\"Far\",\"location\":\"P\","
                                "\"status\":\"confirmed\",\"start\":{\"date\":\"9999-12-31\"}}]}";
        Client c(testConfig(), http, signer, token);
        Query q;
        q.from = core::parseDateTime("2026-08-24", "08:00");
        q.to = core::parseDateTime("2026-08-25", "08:00");
        EventList list;
        CHECK_EQ(static_cast<int>(c.listEvents(q, 1000, list)), static_cast<int>(Status::Ok));
        CHECK_EQ(list.count, 0);
    }
    {
        StubSigner signer;
        StubHttp http;
        Token token;
        Config bad = testConfig();
        bad.calendarId = "";
        Client c(bad, http, signer, token);
        Query q;
        q.from = core::parseDateTime("2026-08-24", "08:00");
        q.to = core::parseDateTime("2026-08-25", "08:00");
        EventList list;
        CHECK_EQ(static_cast<int>(c.listEvents(q, 1000, list)), static_cast<int>(Status::BadConfig));
        CHECK_EQ(http.getCalls, 0);
    }

    t::suite("google / status names");
    CHECK_STR(statusName(Status::Ok), "ok");
    CHECK_STR(statusName(Status::AuthFailed), "auth failed");
    CHECK_STR(statusName(Status::BadResponse), "bad response");
}
