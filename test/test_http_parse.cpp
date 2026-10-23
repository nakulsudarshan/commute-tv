#include "test.h"
#include "../src/net/http_parse.h"
#include "../src/google/calendar.h"

#include <cstring>
#include <string>
#include <vector>

using namespace net;

namespace {

struct Collect : BodySink {
    std::string body;
    size_t refuseAfter = SIZE_MAX;
    bool write(const char* data, size_t n) override {
        if (body.size() + n > refuseAfter) return false;
        body.append(data, n);
        return true;
    }
};

// Feed a whole response in one go.
struct Run {
    Collect sink;
    ResponseReader rd{sink};
    bool fed = false, finished = false;

    explicit Run(const std::string& raw, bool close = true) {
        fed = rd.feed(raw.data(), raw.size());
        if (close) finished = rd.finish();
    }
};

// Feed the same response in fixed-size pieces. The whole point: every split
// must give the same answer, because the network chooses the splits and we do
// not get a vote.
std::string bodyInPieces(const std::string& raw, size_t piece, bool* okOut = nullptr) {
    Collect sink;
    ResponseReader rd(sink);
    bool ok = true;
    for (size_t i = 0; i < raw.size() && ok; i += piece) {
        const size_t n = (i + piece <= raw.size()) ? piece : raw.size() - i;
        ok = rd.feed(raw.data() + i, n);
    }
    if (ok) ok = rd.finish();
    if (okOut) *okOut = ok;
    return sink.body;
}

const char* kChunked =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Transfer-Encoding: chunked\r\n"
    "\r\n"
    "19\r\n{\"items\":[{\"summary\":\"Lec\r\n"
    "f\r\nture\",\"id\":\"x\"}\r\n"
    "3\r\n]}\n\r\n"
    "0\r\n"
    "\r\n";

const char* kChunkedBody = "{\"items\":[{\"summary\":\"Lecture\",\"id\":\"x\"}]}\n";

} // namespace

void test_http_parse() {
    t::suite("http / a plain response");
    {
        Run r("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
        CHECK(r.fed);
        CHECK(r.finished);
        CHECK_EQ(r.rd.status(), 200);
        CHECK(r.rd.complete());
        CHECK_STR(r.sink.body.c_str(), "hello");
        CHECK_EQ(r.rd.bodyBytes(), 5u);
        CHECK(!r.rd.chunked());
    }
    {
        // Header names are case-insensitive, and a bare LF is tolerated: some
        // proxies emit it and refusing costs more than accepting.
        Run r("HTTP/1.0 404 Not Found\ncOnTeNt-LeNgTh: 3\n\nnah");
        CHECK_EQ(r.rd.status(), 404);
        CHECK_STR(r.sink.body.c_str(), "nah");
    }
    {
        // No Content-Length and no chunking: the body runs until the peer hangs
        // up, and finish() is the only thing that can end it.
        Collect sink;
        ResponseReader rd(sink);
        const char* raw = "HTTP/1.1 200 OK\r\n\r\nstreamed bytes";
        CHECK(rd.feed(raw, std::strlen(raw)));
        CHECK(!rd.complete());
        CHECK(rd.finish());
        CHECK(rd.complete());
        CHECK_STR(sink.body.c_str(), "streamed bytes");
    }
    {
        // 204 has no body however the headers are dressed up. Waiting for one
        // would hang the request until the socket timed out.
        Run r("HTTP/1.1 204 No Content\r\nContent-Length: 9\r\n\r\n");
        CHECK_EQ(r.rd.status(), 204);
        CHECK(r.rd.complete());
        CHECK_EQ(r.sink.body.size(), 0u);
    }

    t::suite("http / chunked, which is how Google answers");
    {
        Run r(kChunked);
        CHECK(r.fed);
        CHECK(r.finished);
        CHECK(r.rd.chunked());
        CHECK(!r.rd.hasContentLength());
        CHECK_STR(r.sink.body.c_str(), kChunkedBody);
    }
    {
        // Chunk extensions and trailers are legal and mean nothing to us.
        Run r("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
              "5;name=value\r\nhello\r\n"
              "0\r\n"
              "X-Checksum: 1234\r\n"
              "\r\n");
        CHECK(r.rd.complete());
        CHECK_STR(r.sink.body.c_str(), "hello");
    }
    {
        // Chunked wins over Content-Length; RFC 9112 says ignore the length.
        Run r("HTTP/1.1 200 OK\r\nContent-Length: 999\r\nTransfer-Encoding: chunked\r\n\r\n"
              "2\r\nok\r\n0\r\n\r\n");
        CHECK(r.rd.complete());
        CHECK_STR(r.sink.body.c_str(), "ok");
    }

    t::suite("http / every split of the bytes gives the same answer");
    {
        // The network chooses where the reads land and we do not get a vote. A
        // status line cut in half, a header cut mid-name, a chunk-size line cut
        // between the digits and the CRLF -- all of it must come out identical.
        const std::string raw(kChunked);
        for (size_t piece = 1; piece <= raw.size(); ++piece) {
            bool ok = false;
            const std::string got = bodyInPieces(raw, piece, &ok);
            if (!ok || got != kChunkedBody) {
                std::printf("  split %zu differs: %s\n", piece, got.c_str());
                CHECK(false);
                break;
            }
        }
        CHECK(true);

        const std::string plain =
            "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhello world";
        for (size_t piece = 1; piece <= plain.size(); ++piece) {
            bool ok = false;
            const std::string got = bodyInPieces(plain, piece, &ok);
            if (!ok || got != "hello world") { CHECK(false); break; }
        }
        CHECK(true);
    }

    t::suite("http / headers we act on");
    {
        Run r("HTTP/1.1 429 Too Many\r\nRetry-After: 120\r\nContent-Length: 0\r\n\r\n");
        CHECK_EQ(r.rd.status(), 429);
        CHECK_EQ(r.rd.retryAfterSec(), 120u);
    }
    {
        // The HTTP-date form of Retry-After is legal and needs a date parser
        // plus a trustworthy clock. Ignoring it leaves the budget's own backoff
        // in charge, which is the safe direction.
        Run r("HTTP/1.1 503 Busy\r\nRetry-After: Fri, 31 Dec 1999 23:59:59 GMT\r\n"
              "Content-Length: 0\r\n\r\n");
        CHECK_EQ(r.rd.retryAfterSec(), 0u);
    }
    {
        Run r("HTTP/1.1 302 Found\r\nLocation: https://elsewhere.example/x?y=1\r\n"
              "Content-Length: 0\r\n\r\n");
        CHECK_STR(r.rd.location(), "https://elsewhere.example/x?y=1");
    }
    {
        // A header longer than the buffer is SKIPPED, not truncated. A
        // "Content-Length: 12" clipped to "Content-Length: 1" is a body that
        // ends in the wrong place with nothing to say so.
        std::string raw = "HTTP/1.1 200 OK\r\nX-Long: ";
        raw.append(400, 'z');
        raw += "\r\nContent-Length: 2\r\n\r\nhi";
        Run r(raw);
        CHECK(r.fed);
        CHECK(r.rd.truncatedHeaderSeen());
        CHECK_EQ(r.rd.status(), 200);
        CHECK_STR(r.sink.body.c_str(), "hi");   // structure stayed in step
    }

    t::suite("http / malformed input is refused, not guessed at");
    {
        Collect sink;
        ResponseReader rd(sink);
        const char* raw = "NOT-HTTP 200 OK\r\n\r\n";
        CHECK(!rd.feed(raw, std::strlen(raw)));
        CHECK(rd.error() == Error::Protocol);
    }
    {
        Collect sink;
        ResponseReader rd(sink);
        const char* raw = "HTTP/1.1 2x0 OK\r\n\r\n";
        CHECK(!rd.feed(raw, std::strlen(raw)));
    }
    {
        Collect sink;
        ResponseReader rd(sink);
        const char* raw = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n";
        CHECK(!rd.feed(raw, std::strlen(raw)));
        CHECK(rd.error() == Error::Protocol);
    }
    {
        // A body cut short with a Content-Length promised must NOT look like a
        // complete answer -- that is how half a calendar becomes "no events".
        Collect sink;
        ResponseReader rd(sink);
        const char* raw = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort";
        CHECK(rd.feed(raw, std::strlen(raw)));
        CHECK(!rd.finish());
        CHECK(rd.error() == Error::Protocol);
        CHECK(!rd.complete());
    }
    {
        // Chunked, cut off before the terminating zero chunk: same rule.
        Collect sink;
        ResponseReader rd(sink);
        const char* raw = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                          "5\r\nhello\r\n";
        CHECK(rd.feed(raw, std::strlen(raw)));
        CHECK(!rd.finish());
    }
    {
        // A sink that says stop is obeyed, and says so.
        Collect sink;
        sink.refuseAfter = 3;
        ResponseReader rd(sink);
        const char* raw = "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhello world";
        CHECK(!rd.feed(raw, std::strlen(raw)));
        CHECK(rd.error() == Error::Aborted);
    }

    t::suite("http / urls");
    {
        Url u;
        CHECK(parseUrl("https://www.googleapis.com/calendar/v3/x?a=1&b=2", u));
        CHECK(u.tls);
        CHECK_STR(u.host, "www.googleapis.com");
        CHECK_EQ(u.port, 443);
        CHECK_STR(u.path, "/calendar/v3/x?a=1&b=2");
    }
    {
        Url u;
        CHECK(parseUrl("http://api.open-meteo.com/v1/forecast?lat=50", u));
        CHECK(!u.tls);
        CHECK_EQ(u.port, 80);
        CHECK_STR(u.path, "/v1/forecast?lat=50");
    }
    {
        Url u;
        CHECK(parseUrl("http://192.168.125.223:8080/set?img=x", u));
        CHECK_EQ(u.port, 8080);
        CHECK_STR(u.host, "192.168.125.223");
        CHECK_STR(u.path, "/set?img=x");
    }
    {
        // No path at all still needs a request target.
        Url u;
        CHECK(parseUrl("https://example.com", u));
        CHECK_STR(u.path, "/");
    }
    {
        Url u;
        CHECK(parseUrl("https://example.com?q=1", u));
        CHECK_STR(u.path, "/?q=1");
    }
    {
        Url u;
        CHECK(!parseUrl("ftp://example.com/x", u));
        CHECK(!parseUrl("example.com/x", u));
        CHECK(!parseUrl("https:///x", u));
        CHECK(!parseUrl("https://example.com:0/x", u));
        CHECK(!parseUrl("https://example.com:99999/x", u));
        CHECK(!parseUrl(nullptr, u));
    }
    {
        // ⚠️ THE ONE THAT COST A FLASH CYCLE. Every URL the builders can emit
        // must fit the parser. Sizing them independently meant a request that
        // was constructed perfectly died one layer later, and the board printed
        // "cannot parse that url" and sat in `idle` for ever.
        //
        // A real-shaped calendar id: 64 hex characters at
        // @group.calendar.google.com, which percent-encodes to 28 more.
        google::Config cfg;
        cfg.calendarId = "6af222a00d7210e345a7fe5fa59d3b6fda5ec405"
                         "aaaaaaaaaaaaaaaaaaaaaaaa@group.calendar.google.com";
        google::Query q;
        q.from = core::makeTs(20000 * 1440 + 9 * 60);
        q.to = core::makeTs(20000 * 1440 + 21 * 60);
        q.utcOffsetMin = 120;

        char url[1024];
        const int n = google::buildEventsUrl(cfg, q, url, sizeof url);
        CHECK(n > 0);
        CHECK(n > 320);                 // it really is longer than the old field

        Url u;
        CHECK(parseUrl(url, u));
        CHECK(u.tls);
        CHECK_STR(u.host, "www.googleapis.com");
        CHECK(std::strlen(u.path) < kMaxPathLen);
        // The path must survive whole: a clipped query is a different request.
        CHECK(std::strstr(u.path, "nextPageToken") != nullptr);
        CHECK(std::strstr(u.path, "timeMin=") != nullptr);
    }
    {
        // Too long to hold is refused outright rather than silently clipped
        // into a request for a different resource.
        std::string longHost = "https://";
        longHost.append(kMaxHostLen + 16, 'h');
        longHost += ".example.com/";
        Url u;
        CHECK(!parseUrl(longHost.c_str(), u));

        // Sized FROM the constant, not from a number that matched it once.
        // This assertion is why the previous line existed at all, and it broke
        // the moment kMaxPathLen changed -- which is the test doing its job.
        std::string longPath = "https://example.com/";
        longPath.append(kMaxPathLen + 16, 'p');
        CHECK(!parseUrl(longPath.c_str(), u));
    }
}
