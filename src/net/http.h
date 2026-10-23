// http.h - the HTTP client interface the firmware talks to the internet through.
//
// Streaming by design. The SmallTV transport in src/smalltv/ buffers a whole
// response into a 384-byte struct, which is right for a LAN device answering in
// tens of bytes. It is wrong for AVV (36 KB) and for Google, where buffering
// would put the response back in the one contiguous DRAM region that TLS and
// the JPEG buffer are already competing for.
//
// So a response arrives as a series of chunks. Feed them straight into a
// json::Reader and the document is never held whole. Nothing here allocates.
//
// The ESP32 implements this over WiFiClientSecure; the host tests implement it
// with canned responses, which is what makes the clients above it testable
// without a network.
#pragma once
#include <cstddef>
#include <cstdint>

namespace net {

// Receives the response body as it arrives. Returning false aborts the
// transfer -- used when the parser has already failed and the rest of the body
// is of no interest.
struct BodySink {
    virtual ~BodySink() = default;
    virtual bool write(const char* data, size_t n) = 0;
};

struct Header {
    const char* name;
    const char* value;
};

enum class Error : uint8_t {
    None,
    Unreachable,   // no route, DNS failure, connection refused
    Timeout,
    Tls,           // handshake or certificate failure
    Protocol,      // malformed response
    Aborted,       // the sink asked to stop
};

const char* errorName(Error e);

struct Result {
    Error error = Error::Unreachable;
    int status = 0;
    size_t bytes = 0;          // body bytes handed to the sink

    // Seconds from a Retry-After header, or 0 when absent. Passed straight to
    // core::RequestBudget::recordFailure: a server that says how long to wait
    // is more authoritative than our own backoff.
    uint32_t retryAfterSec = 0;

    bool ok() const { return error == Error::None && status >= 200 && status < 300; }
};

struct Client {
    virtual ~Client() = default;

    virtual Result get(const char* url, const Header* headers, size_t headerCount,
                       BodySink& body) = 0;

    // POST with an explicit content type. `body` is not copied.
    virtual Result post(const char* url, const char* contentType,
                        const char* body, size_t bodyLen,
                        const Header* headers, size_t headerCount,
                        BodySink& out) = 0;
};

} // namespace net
