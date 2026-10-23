// json_sink.h - plug a streaming HTTP body straight into the JSON reader.
//
// This is the join that keeps a response from ever being held whole: bytes go
// from the socket into the parser and the only thing that survives is the
// handful of fields the handler kept.
#pragma once
#include <cstddef>

#include "../json/json_stream.h"
#include "http.h"

namespace net {

class JsonSink final : public BodySink {
public:
    explicit JsonSink(json::Reader& rd) : rd_(&rd) {}

    // Returning false on a parse error aborts the transfer. There is no point
    // pulling another 30 KB through TLS to feed a reader that has already
    // given up, and the transport reports Error::Aborted so the caller can
    // tell that apart from a network failure.
    bool write(const char* data, size_t n) override {
        return rd_->feed(data, n);
    }

    // Call after the request completes. False means the body was malformed or
    // cut short -- which is NOT the same as an empty result and must not be
    // reported as one.
    bool finish() { return rd_->finish(); }

private:
    json::Reader* rd_;
};

} // namespace net
