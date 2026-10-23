#include "test.h"
#include "../src/json/json_stream.h"

#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

// Records every event as one line, so two parses can be compared as text and a
// failure prints something a human can read rather than a struct dump.
struct Log : json::Handler {
    char buf[16384] = {0};
    size_t n = 0;

    void add(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

    void writePath(const json::Path& p) {
        for (int i = 0; i < p.depth(); ++i) {
            if (p.index(i) >= 0) add("[%d]", p.index(i));
            else                 add("%s%s", i ? "." : "", p.key(i));
        }
        if (p.depth() == 0) add("$");
    }

    void onScalar(const json::Path& p, json::Type t, const char* text, size_t len,
                  bool truncated) override {
        static const char* kNames[] = {"null", "bool", "num", "str"};
        writePath(p);
        add(" = %s(%s)%s len=%zu\n", kNames[static_cast<int>(t)], text,
            truncated ? " TRUNC" : "", len);
    }
    void onEnter(const json::Path& p, bool isArray) override {
        writePath(p); add(" %s\n", isArray ? "[" : "{");
    }
    void onLeave(const json::Path& p, bool isArray) override {
        writePath(p); add(" %s\n", isArray ? "]" : "}");
    }
};

void Log::add(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (n < sizeof buf - 1) {
        const int w = std::vsnprintf(buf + n, sizeof buf - n, fmt, ap);
        if (w > 0) n += static_cast<size_t>(w);
        if (n > sizeof buf - 1) n = sizeof buf - 1;
    }
    va_end(ap);
}

// Parse in fixed-size chunks. chunk == 0 means "all at once".
bool run(const char* doc, size_t chunk, Log& log, const char** err = nullptr) {
    json::Reader rd(log);
    const size_t len = std::strlen(doc);
    bool ok = true;
    if (chunk == 0) {
        ok = rd.feed(doc, len);
    } else {
        for (size_t i = 0; i < len && ok; i += chunk)
            ok = rd.feed(doc + i, (len - i < chunk) ? (len - i) : chunk);
    }
    ok = ok && rd.finish();
    if (err) *err = rd.error();
    return ok;
}

bool rejects(const char* doc) {
    Log log;
    return !run(doc, 0, log);
}

// A scalar-only handler, for tests that care about one value.
struct Grab : json::Handler {
    const char* want;
    char got[256] = {0};
    bool found = false;
    bool truncated = false;
    explicit Grab(const char* pattern) : want(pattern) {}
    void onScalar(const json::Path& p, json::Type, const char* text, size_t,
                  bool trunc) override {
        if (p.is(want)) {
            std::snprintf(got, sizeof got, "%s", text);
            found = true;
            truncated = trunc;
        }
    }
};

const char* grab(const char* doc, const char* pattern) {
    static char result[256];
    Grab g(pattern);
    json::Reader rd(g);
    rd.feed(doc, std::strlen(doc));
    rd.finish();
    std::snprintf(result, sizeof result, "%s", g.found ? g.got : "(not found)");
    return result;
}

} // namespace

void test_json() {
    t::suite("json / structure");
    {
        Log l;
        CHECK(run("{\"a\":1,\"b\":\"x\"}", 0, l));
        CHECK_STR(l.buf,
                  "$ {\n"
                  "a = num(1) len=1\n"
                  "b = str(x) len=1\n"
                  "$ }\n");
    }
    {
        Log l;
        CHECK(run("[10,20,30]", 0, l));
        CHECK_STR(l.buf,
                  "$ [\n"
                  "[0] = num(10) len=2\n"
                  "[1] = num(20) len=2\n"
                  "[2] = num(30) len=2\n"
                  "$ ]\n");
    }
    {   // empty containers must not push a phantom component
        Log l;
        CHECK(run("{\"a\":[],\"b\":{}}", 0, l));
        CHECK_STR(l.buf, "$ {\na [\na ]\nb {\nb }\n$ }\n");
    }
    {   // a bare scalar is a legal document
        Log l;
        CHECK(run("  42  ", 0, l));
        CHECK_STR(l.buf, "$ = num(42) len=2\n");
    }
    {   // whitespace everywhere
        Log l;
        CHECK(run("\n{\t\"a\" :\r [ 1 , 2 ] }\n", 0, l));
        CHECK_STR(l.buf, "$ {\na [\na[0] = num(1) len=1\na[1] = num(2) len=1\na ]\n$ }\n");
    }

    t::suite("json / paths");
    {
        const char* doc =
            "{\"Trip\":[{\"LegList\":{\"Leg\":[{\"Origin\":{\"time\":\"19:26:00\"}},"
            "{\"Origin\":{\"time\":\"19:30:00\"}}]}}]}";
        CHECK_STR(grab(doc, "Trip[*].LegList.Leg[1].Origin.time"), "19:30:00");
        CHECK_STR(grab(doc, "Trip[0].LegList.Leg[0].Origin.time"), "19:26:00");
        CHECK_STR(grab(doc, "Trip[*].LegList.Leg[*].Origin.time"), "19:30:00"); // last wins
        // A pattern must account for the whole path, not just a prefix.
        CHECK_STR(grab(doc, "Trip[*].LegList.Leg[*].Origin"), "(not found)");
        // Wrong index, wrong key, key where an index belongs.
        CHECK_STR(grab(doc, "Trip[1].LegList.Leg[0].Origin.time"), "(not found)");
        CHECK_STR(grab(doc, "Trip[*].LegList.Leg[*].Destination.time"), "(not found)");
        CHECK_STR(grab(doc, "Trip.LegList.Leg[*].Origin.time"), "(not found)");
    }
    {
        // The trap this parser exists to avoid: the same key name at three
        // different depths. A scanner looking for "time" would take the wrong one.
        const char* doc =
            "{\"Leg\":[{\"Origin\":{\"time\":\"A\"},\"Destination\":{\"time\":\"B\"},"
            "\"JourneyDetail\":{\"Stops\":{\"Stop\":[{\"time\":\"C\"},{\"time\":\"D\"}]}}}]}";
        CHECK_STR(grab(doc, "Leg[0].Origin.time"), "A");
        CHECK_STR(grab(doc, "Leg[0].Destination.time"), "B");
        CHECK_STR(grab(doc, "Leg[0].JourneyDetail.Stops.Stop[1].time"), "D");
    }
    {
        struct Idx : json::Handler {
            int a = -9, b = -9, depth = 0;
            void onScalar(const json::Path& p, json::Type, const char*, size_t, bool) override {
                if (p.is("x[*].y[*].z")) { a = p.nthIndex(0); b = p.nthIndex(1); depth = p.depth(); }
            }
        } idx;
        json::Reader rd(idx);
        const char* doc = "{\"x\":[{},{},{\"y\":[{},{\"z\":1}]}]}";
        CHECK(rd.feed(doc, std::strlen(doc)));
        CHECK(rd.finish());
        CHECK_EQ(idx.a, 2);
        CHECK_EQ(idx.b, 1);
        CHECK_EQ(idx.depth, 5);
    }
    {
        struct Sub : json::Handler {
            int inside = 0, outside = 0;
            void onScalar(const json::Path& p, json::Type, const char*, size_t, bool) override {
                if (p.startsWith("a.b")) ++inside; else ++outside;
            }
        } sub;
        json::Reader rd(sub);
        const char* doc = "{\"a\":{\"b\":{\"c\":1,\"d\":{\"e\":2}},\"f\":3},\"g\":4}";
        CHECK(rd.feed(doc, std::strlen(doc)));
        CHECK(rd.finish());
        CHECK_EQ(sub.inside, 2);
        CHECK_EQ(sub.outside, 2);
    }

    t::suite("json / strings and escapes");
    CHECK_STR(grab("{\"s\":\"a\\\"b\\\\c\\/d\"}", "s"), "a\"b\\c/d");
    CHECK_STR(grab("{\"s\":\"a\\nb\\tc\"}", "s"), "a\nb\tc");
    // \u escapes become UTF-8, which is what the fold step downstream expects.
    CHECK_STR(grab("{\"s\":\"H\\u00f6rn\"}", "s"), "H\xc3\xb6rn");
    CHECK_STR(grab("{\"s\":\"Stra\\u00dfe\"}", "s"), "Stra\xc3\x9f""e");
    // Raw UTF-8 in the document passes through untouched.
    CHECK_STR(grab("{\"s\":\"H\xc3\xb6rn Br\xc3\xbc""cke\"}", "s"), "H\xc3\xb6rn Br\xc3\xbc""cke");
    // A surrogate pair is one code point, not two.
    CHECK_STR(grab("{\"s\":\"\\ud83d\\ude00\"}", "s"), "\xf0\x9f\x98\x80");
    // A lone surrogate is replaced rather than emitted as invalid UTF-8.
    CHECK_STR(grab("{\"s\":\"a\\ud83db\"}", "s"), "a\xef\xbf\xbd""b");
    CHECK_STR(grab("{\"s\":\"a\\udc00b\"}", "s"), "a\xef\xbf\xbd""b");
    // An embedded NUL would silently truncate a C string, so it is replaced.
    CHECK_STR(grab("{\"s\":\"a\\u0000b\"}", "s"), "a\xef\xbf\xbd""b");
    // Escaped member names work too.
    CHECK_STR(grab("{\"a\\u0062c\":\"v\"}", "abc"), "v");

    t::suite("json / numbers and literals");
    CHECK_STR(grab("{\"n\":-12}", "n"), "-12");
    CHECK_STR(grab("{\"n\":3.25}", "n"), "3.25");
    CHECK_STR(grab("{\"n\":1e-3}", "n"), "1e-3");
    CHECK_STR(grab("{\"b\":true}", "b"), "true");
    CHECK_STR(grab("{\"b\":false}", "b"), "false");
    CHECK_STR(grab("{\"b\":null}", "b"), "null");
    {
        bool ok = false;
        CHECK_EQ(json::toLong("42", &ok), 42);      CHECK(ok);
        CHECK_EQ(json::toLong("-7", &ok), -7);      CHECK(ok);
        CHECK_EQ(json::toLong("0", &ok), 0);        CHECK(ok);
        // A float is not an integer, and saying so beats silently truncating.
        CHECK_EQ(json::toLong("3.9", &ok), 3);      CHECK(!ok);
        CHECK_EQ(json::toLong("", &ok), 0);         CHECK(!ok);
        CHECK_EQ(json::toLong("abc", &ok), 0);      CHECK(!ok);
        // Saturating, not wrapping. `long` is 32 bits on the ESP32, so any
        // JSON integer past 2^31 gets here -- and a wrapped value is the worst
        // kind of wrong, because it looks like a plausible small number.
        CHECK_EQ(json::toLong("99999999999999999999", &ok), LONG_MAX); CHECK(!ok);
        CHECK_EQ(json::toLong("-99999999999999999999", &ok), LONG_MIN); CHECK(!ok);
        // toDouble is hand-rolled rather than strtod, which is locale
        // sensitive -- a device that came up in a comma-decimal locale would
        // read 3.86 km as 3 km and say nothing.
        bool okd = false;
        CHECK_NEAR(json::toDouble("3.86", &okd), 3.86, 1e-9);      CHECK(okd);
        CHECK_NEAR(json::toDouble("-4.5", &okd), -4.5, 1e-9);      CHECK(okd);
        CHECK_NEAR(json::toDouble("786", &okd), 786.0, 1e-9);      CHECK(okd);
        CHECK_NEAR(json::toDouble("0.001", &okd), 0.001, 1e-9);    CHECK(okd);
        CHECK_NEAR(json::toDouble("1e3", &okd), 1000.0, 1e-9);     CHECK(okd);
        CHECK_NEAR(json::toDouble("1.5e-2", &okd), 0.015, 1e-9);   CHECK(okd);
        CHECK_NEAR(json::toDouble(".5", &okd), 0.5, 1e-9);         CHECK(okd);
        // null is not zero: a missing rain probability must never read as
        // "no rain", which is what would put someone on a bike in a storm.
        CHECK_NEAR(json::toDouble("null", &okd), 0.0, 1e-9);       CHECK(!okd);
        CHECK_NEAR(json::toDouble("", &okd), 0.0, 1e-9);           CHECK(!okd);
        CHECK_NEAR(json::toDouble("12abc", &okd), 0.0, 1e-9);      CHECK(!okd);
        CHECK_NEAR(json::toDouble(nullptr, &okd), 0.0, 1e-9);      CHECK(!okd);

        CHECK(json::toBool("true"));
        CHECK(!json::toBool("false"));
        CHECK(!json::toBool(nullptr));
    }

    t::suite("json / an external value buffer");
    {
        // Values longer than the built-in buffer survive when the caller
        // supplies a bigger one -- this is what carries a Google access token.
        char big[600];
        char doc[700];
        char payload[500];
        std::memset(payload, 'z', sizeof payload - 1);
        payload[sizeof payload - 1] = '\0';
        std::snprintf(doc, sizeof doc, "{\"tok\":\"%s\",\"n\":7}", payload);

        struct H : json::Handler {
            char tok[600] = {0};
            long n = 0;
            bool trunc = true;
            void onScalar(const json::Path& p, json::Type, const char* text, size_t,
                          bool t2) override {
                if (p.is("tok")) { std::snprintf(tok, sizeof tok, "%s", text); trunc = t2; }
                if (p.is("n")) n = json::toLong(text);
            }
        } h;
        json::Reader rd(h, big, sizeof big);
        CHECK(rd.feed(doc, std::strlen(doc)));
        CHECK(rd.finish());
        CHECK_EQ(std::strlen(h.tok), 499u);
        CHECK(!h.trunc);
        CHECK_EQ(h.n, 7);       // a later short value must not be lost either
    }
    {
        // A buffer too small to hold a member NAME would truncate keys, so
        // every pattern would silently stop matching while the parse reported
        // success. Rather than trust the caller's arithmetic, fall back.
        char tiny[8];
        struct H : json::Handler {
            bool sawKey = false;
            void onScalar(const json::Path& p, json::Type, const char*, size_t, bool) override {
                if (p.is("access_token")) sawKey = true;
            }
        } h;
        json::Reader rd(h, tiny, sizeof tiny);
        CHECK(rd.valueCapacity() >= json::kMaxKeyLen);
        const char* doc = "{\"access_token\":\"abc\"}";
        CHECK(rd.feed(doc, std::strlen(doc)));
        CHECK(rd.finish());
        CHECK(h.sawKey);
    }
    {   // A null buffer must not be dereferenced.
        struct H : json::Handler {
            int n = 0;
            void onScalar(const json::Path&, json::Type, const char*, size_t, bool) override { ++n; }
        } h;
        json::Reader rd(h, nullptr, 0);
        CHECK(rd.feed("{\"a\":1}", 7));
        CHECK(rd.finish());
        CHECK_EQ(h.n, 1);
    }

    t::suite("json / chunk boundaries");
    {
        // The reader is fed by a socket, so a token can straddle any boundary --
        // including the middle of a \uXXXX escape. Every possible split of this
        // document must produce identical events.
        const char* doc =
            "{\"Trip\":[{\"idx\":\"0\",\"LegList\":{\"Leg\":["
            "{\"type\":\"WALK\",\"Origin\":{\"name\":\"Stra\\u00dfe\",\"time\":\"19:18:00\"}},"
            "{\"type\":\"JNY\",\"Product\":[{\"line\":\"47\"}],\"reachable\":true}"
            "]}}],\"planRtTs\":\"2026-08-22T18:46:45.000+02:00\"}";
        Log whole;
        CHECK(run(doc, 0, whole));

        bool allSame = true;
        size_t firstBad = 0;
        for (size_t c = 1; c <= std::strlen(doc); ++c) {
            Log piece;
            if (!run(doc, c, piece) || std::strcmp(piece.buf, whole.buf) != 0) {
                allSame = false; firstBad = c; break;
            }
        }
        if (!allSame) std::printf("       first differing chunk size: %zu\n", firstBad);
        CHECK(allSame);
    }

    t::suite("json / malformed input is rejected");
    CHECK(rejects("{"));                    // unterminated object
    CHECK(rejects("["));                    // unterminated array
    CHECK(rejects("{\"a\"}"));              // key with no value
    CHECK(rejects("{\"a\":}"));             // value missing
    CHECK(rejects("{\"a\":1,}"));           // trailing comma, object
    CHECK(rejects("[1,]"));                 // trailing comma, array
    CHECK(rejects("{\"a\":1}}"));           // extra close
    CHECK(rejects("{\"a\":1]"));            // mismatched close
    CHECK(rejects("tru"));                  // truncated literal
    CHECK(rejects("trueX"));                // literal with junk
    CHECK(rejects("\"unterminated"));       // string left open
    CHECK(rejects("{\"a\":\"x\\q\"}"));     // bad escape
    CHECK(rejects("{\"a\":\"x\\u00zz\"}")); // bad hex
    CHECK(rejects("1 2"));                  // two root values
    CHECK(rejects(""));                     // empty document
    CHECK(rejects("   "));                  // whitespace only
    CHECK(rejects("{\"a\":1"));             // cut short mid-object
    {   // A control character inside a string is illegal per RFC 8259.
        char doc[] = "{\"a\":\"x\tz\"}";
        Log l;
        CHECK(!run(doc, 0, l));
    }
    {   // The reason is reported, not just the failure.
        Log l; const char* err = nullptr;
        CHECK(!run("{\"a\":1", 0, l, &err));
        CHECK(err != nullptr && err[0] != '\0');
    }
    {   // A truncated body must never look like a successful parse: two
        // complete trips plus a cut third is a failure, not a result.
        Log l;
        CHECK(!run("{\"Trip\":[{\"idx\":\"0\"},{\"idx\":\"1\"},{\"idx", 0, l));
    }

    t::suite("json / bounded memory");
    {
        // A value longer than the buffer is truncated and flagged, and the
        // parse carries on correctly -- ctxRecon in a real response is far
        // longer than anything we keep.
        char doc[512];
        char big[300];
        std::memset(big, 'x', sizeof big);
        big[sizeof big - 1] = '\0';
        std::snprintf(doc, sizeof doc, "{\"big\":\"%s\",\"after\":\"ok\"}", big);

        struct H : json::Handler {
            bool sawTrunc = false;
            size_t bigLen = 0;
            char after[16] = {0};
            void onScalar(const json::Path& p, json::Type, const char* text, size_t len,
                          bool trunc) override {
                if (p.is("big")) { sawTrunc = trunc; bigLen = len; }
                if (p.is("after")) std::snprintf(after, sizeof after, "%s", text);
            }
        } h;
        json::Reader rd(h);
        CHECK(rd.feed(doc, std::strlen(doc)));
        CHECK(rd.finish());
        CHECK(h.sawTrunc);
        CHECK_EQ(h.bigLen, json::kMaxValueLen);
        CHECK_STR(h.after, "ok");
    }
    {
        // Nesting deeper than the path can track must not corrupt anything:
        // shallow values still arrive, the deep ones are simply not delivered,
        // and the document still validates.
        char doc[512] = {0};
        size_t w = 0;
        w += static_cast<size_t>(std::snprintf(doc + w, sizeof doc - w, "{\"top\":1,\"deep\":"));
        for (int i = 0; i < 40; ++i) doc[w++] = '[';
        doc[w++] = '9';
        for (int i = 0; i < 40; ++i) doc[w++] = ']';
        doc[w++] = '}';
        doc[w] = '\0';

        struct H : json::Handler {
            int top = 0, deepSeen = 0;
            bool overflowed = false;
            void onScalar(const json::Path& p, json::Type, const char* text, size_t, bool) override {
                if (p.is("top")) top = static_cast<int>(json::toLong(text));
                if (p.overflowed()) overflowed = true;
                if (std::strcmp(text, "9") == 0) ++deepSeen;
            }
        } h;
        json::Reader rd(h);
        CHECK(rd.feed(doc, std::strlen(doc)));
        CHECK(rd.finish());
        CHECK_EQ(h.top, 1);
        CHECK_EQ(h.deepSeen, 0);            // past kMaxDepth, so not delivered
        CHECK(rd.path().overflowed());
    }
    {
        // Beyond the container limit the reader gives up rather than losing
        // track of the balance.
        char doc[256] = {0};
        std::memset(doc, '[', 100);
        Log l;
        CHECK(!run(doc, 0, l));
    }
}
