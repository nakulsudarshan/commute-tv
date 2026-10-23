// http_parse.h - reading an HTTP/1.1 response, without a socket in sight.
//
// The socket half of an HTTP client is dull and cannot be tested on a laptop.
// The parsing half is where the bugs live and CAN be, so it lives here and the
// ESP32 code does nothing but feed it bytes.
//
// What it has to get right, in the order they bite:
//
//   * CHUNKED TRANSFER ENCODING. Google's Calendar API answers chunked, and a
//     client that ignores it hands the JSON reader a body with hex length
//     markers sprinkled through it. That does not fail loudly -- it fails as a
//     parse error two layers away from the cause.
//   * ARBITRARY CHUNK BOUNDARIES. Bytes arrive in whatever sizes the network
//     felt like. A status line split across two reads, a header split mid-name,
//     a chunk-size line split between the digits and the CRLF: every one of
//     those must produce the same result as the whole response at once. Same
//     property the JSON reader has, tested the same way.
//   * BOUNDED MEMORY. One line buffer, no allocation, and a header longer than
//     the buffer is skipped rather than overflowed or half-read.
//
// It streams the body to a net::BodySink as it goes, so a 40 KB calendar reply
// never exists anywhere in one piece.
#pragma once
#include <cstddef>
#include <cstdint>

#include "http.h"

namespace net {

// Long enough for the status line and every header we act on. A longer header
// is skipped -- see `lineTruncated()` -- rather than truncated into a value
// that would be wrong in a way nobody could see.
constexpr size_t kMaxLineLen = 192;
constexpr size_t kMaxLocationLen = 160;

class ResponseReader {
public:
    explicit ResponseReader(BodySink& sink) : sink_(&sink) {}

    ResponseReader(const ResponseReader&) = delete;
    ResponseReader& operator=(const ResponseReader&) = delete;

    // Feed whatever arrived. Returns false once the response is malformed or
    // the sink asked to stop; error() says which.
    bool feed(const char* data, size_t n);

    // The connection closed. For a response with neither Content-Length nor
    // chunked encoding this is the only thing that ends the body, so it is not
    // optional. Returns false if the response was cut short mid-structure.
    bool finish();

    int status() const { return status_; }
    bool headersDone() const { return state_ > State::Header; }
    bool complete() const { return state_ == State::Done; }
    Error error() const { return error_; }

    bool chunked() const { return chunked_; }
    bool hasContentLength() const { return haveLength_; }
    size_t contentLength() const { return length_; }
    size_t bodyBytes() const { return bodyBytes_; }
    uint32_t retryAfterSec() const { return retryAfter_; }
    const char* location() const { return location_; }
    bool truncatedHeaderSeen() const { return truncatedHeader_; }

private:
    enum class State : uint8_t {
        Status, Header, Body, ChunkSize, ChunkData, ChunkCrlf, Trailer, Done, Bad,
    };

    bool pushLineByte(char c);        // false => the line overflowed
    bool onLine();                    // dispatch a completed line
    bool onStatusLine();
    bool onHeaderLine();
    bool beginBody();
    bool emit(const char* p, size_t n);
    bool fail(Error e);

    BodySink* sink_;
    State state_ = State::Status;
    Error error_ = Error::None;

    char line_[kMaxLineLen] = {0};
    size_t lineLen_ = 0;
    bool lineOverflow_ = false;
    bool truncatedHeader_ = false;

    int status_ = 0;
    bool chunked_ = false;
    bool haveLength_ = false;
    size_t length_ = 0;
    size_t remaining_ = 0;       // of the current body or chunk
    size_t bodyBytes_ = 0;
    uint32_t retryAfter_ = 0;
    char location_[kMaxLocationLen] = {0};
};

// --- URLs -------------------------------------------------------------------
//
// Just enough to open a socket with. No userinfo, no fragments: nothing in this
// firmware issues a URL containing either, and parsing what we do not use is
// how a parser grows a bug.
// ⚠️ THIS CAPACITY IS NOT A GUESS, and the number it replaced was.
//
// A Google Calendar events URL carries a 64-character calendar id, two RFC 3339
// timestamps with percent-encoded offsets, and a `fields` mask. MEASURED against
// Nakul's real calendar id: 394 characters total, 368 of them path and query.
// The field was 320, parseUrl refused it -- correctly, since truncating a URL
// silently turns it into a request for a different resource -- and the board
// printed "cannot parse that url" and sat in `idle` for ever.
//
// It is 512 to match google::Client::urlBuf_: anything the URL BUILDERS can
// emit must fit the URL PARSER, or a request that was constructed successfully
// dies one layer later for no reason a log could explain. test_http_parse
// asserts that relationship against a real-shaped calendar URL.
constexpr size_t kMaxPathLen = 512;
constexpr size_t kMaxHostLen = 96;

struct Url {
    bool tls = false;
    char host[kMaxHostLen] = {0};
    uint16_t port = 0;
    char path[kMaxPathLen] = {0};   // path + query, ready for a request line
    bool valid = false;
};

// Returns false if it is not something we can dial: unknown scheme, empty host,
// or a host or path longer than the fields above.
bool parseUrl(const char* url, Url& out);

} // namespace net
