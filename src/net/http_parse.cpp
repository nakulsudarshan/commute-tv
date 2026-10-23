#include "http_parse.h"

#include <cstring>

namespace net {
namespace {

bool iEqual(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return *a == '\0' && *b == '\0';
}

bool iStartsWith(const char* s, const char* prefix) {
    for (; *prefix; ++s, ++prefix) {
        char cs = *s, cp = *prefix;
        if (cs >= 'A' && cs <= 'Z') cs = static_cast<char>(cs - 'A' + 'a');
        if (cp >= 'A' && cp <= 'Z') cp = static_cast<char>(cp - 'A' + 'a');
        if (cs != cp) return false;
    }
    return true;
}

const char* skipSpace(const char* s) {
    while (*s == ' ' || *s == '\t') ++s;
    return s;
}

// Saturating, so a hostile Content-Length cannot wrap into something small.
bool parseSizeT(const char* s, size_t& out) {
    if (!*s) return false;
    size_t v = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return false;
        const size_t d = static_cast<size_t>(*s - '0');
        if (v > (SIZE_MAX - d) / 10) { out = SIZE_MAX; return true; }
        v = v * 10 + d;
    }
    out = v;
    return true;
}

// Chunk sizes are hex, and may carry a `;extension` we ignore.
bool parseChunkSize(const char* s, size_t& out) {
    s = skipSpace(s);
    if (!*s) return false;
    size_t v = 0;
    bool any = false;
    for (; *s; ++s) {
        size_t d;
        if (*s >= '0' && *s <= '9')      d = static_cast<size_t>(*s - '0');
        else if (*s >= 'a' && *s <= 'f') d = static_cast<size_t>(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F') d = static_cast<size_t>(*s - 'A' + 10);
        else break;
        if (v > (SIZE_MAX - d) / 16) return false;   // absurd; refuse it
        v = v * 16 + d;
        any = true;
    }
    if (!any) return false;
    s = skipSpace(s);
    // Anything after the digits must be an extension, and nothing else.
    if (*s != '\0' && *s != ';') return false;
    out = v;
    return true;
}

} // namespace

bool ResponseReader::fail(Error e) {
    if (error_ == Error::None) error_ = e;
    state_ = State::Bad;
    return false;
}

bool ResponseReader::emit(const char* p, size_t n) {
    if (n == 0) return true;
    bodyBytes_ += n;
    if (!sink_->write(p, n)) return fail(Error::Aborted);
    return true;
}

bool ResponseReader::pushLineByte(char c) {
    if (lineLen_ + 1 >= kMaxLineLen) {
        // Keep scanning to the newline so the STRUCTURE stays in step; just stop
        // recording. A half-recorded header is worse than a skipped one --
        // "Content-Length: 12" truncated to "Content-Length: 1" is a body that
        // ends in the wrong place and nothing to say so.
        lineOverflow_ = true;
        return true;
    }
    line_[lineLen_++] = c;
    return true;
}

bool ResponseReader::feed(const char* data, size_t n) {
    if (state_ == State::Bad) return false;
    if (state_ == State::Done) return true;

    size_t i = 0;
    while (i < n) {
        switch (state_) {
            case State::Status:
            case State::Header:
            case State::ChunkSize:
            case State::Trailer: {
                const char c = data[i++];
                if (c == '\n') {
                    line_[lineLen_] = '\0';
                    // Tolerate a bare LF as well as CRLF: some proxies do it and
                    // refusing costs more than accepting.
                    if (lineLen_ > 0 && line_[lineLen_ - 1] == '\r') {
                        line_[lineLen_ - 1] = '\0';
                    }
                    const bool ok = onLine();
                    lineLen_ = 0;
                    lineOverflow_ = false;
                    if (!ok) return false;
                } else {
                    if (!pushLineByte(c)) return false;
                }
                break;
            }
            case State::Body: {
                size_t take = n - i;
                if (haveLength_ && take > remaining_) take = remaining_;
                if (!emit(data + i, take)) return false;
                i += take;
                if (haveLength_) {
                    remaining_ -= take;
                    if (remaining_ == 0) { state_ = State::Done; return true; }
                }
                break;
            }
            case State::ChunkData: {
                size_t take = n - i;
                if (take > remaining_) take = remaining_;
                if (!emit(data + i, take)) return false;
                i += take;
                remaining_ -= take;
                if (remaining_ == 0) state_ = State::ChunkCrlf;
                break;
            }
            case State::ChunkCrlf: {
                // The CRLF that follows every chunk's data. Skip exactly those
                // bytes; anything else here means the framing is wrong and the
                // body cannot be trusted.
                const char c = data[i++];
                if (c == '\n') state_ = State::ChunkSize;
                else if (c != '\r') return fail(Error::Protocol);
                break;
            }
            case State::Done: return true;
            case State::Bad:  return false;
        }
    }
    return true;
}

bool ResponseReader::onLine() {
    switch (state_) {
        case State::Status:  return onStatusLine();
        case State::Header:  return onHeaderLine();
        case State::ChunkSize: {
            if (lineOverflow_) return fail(Error::Protocol);
            if (line_[0] == '\0') return true;      // tolerate a stray blank line
            size_t sz = 0;
            if (!parseChunkSize(line_, sz)) return fail(Error::Protocol);
            if (sz == 0) {
                state_ = State::Trailer;            // trailers, then a blank line
                return true;
            }
            remaining_ = sz;
            state_ = State::ChunkData;
            return true;
        }
        case State::Trailer:
            if (line_[0] == '\0') { state_ = State::Done; }
            return true;                            // ignore trailer headers
        default:
            return fail(Error::Protocol);
    }
}

bool ResponseReader::onStatusLine() {
    if (lineOverflow_) return fail(Error::Protocol);
    if (!iStartsWith(line_, "http/")) return fail(Error::Protocol);

    const char* p = line_;
    while (*p && *p != ' ') ++p;                    // past HTTP/1.1
    p = skipSpace(p);
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9' ||
        p[2] < '0' || p[2] > '9') {
        return fail(Error::Protocol);
    }
    status_ = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
    state_ = State::Header;
    return true;
}

bool ResponseReader::onHeaderLine() {
    if (line_[0] == '\0') return beginBody();       // the blank line

    if (lineOverflow_) {
        // We could not see all of it, so we do not know what it said. Recording
        // that is the point: a caller that cares can refuse the response rather
        // than trust a body framed by a header we only half read.
        truncatedHeader_ = true;
        return true;
    }

    char* colon = std::strchr(line_, ':');
    if (!colon) return true;                        // not a header; ignore it
    *colon = '\0';
    const char* name = line_;
    const char* value = skipSpace(colon + 1);

    if (iEqual(name, "content-length")) {
        size_t v = 0;
        if (parseSizeT(value, v)) { haveLength_ = true; length_ = v; }
    } else if (iEqual(name, "transfer-encoding")) {
        // "chunked" is always the last encoding if present, and it is the only
        // one this firmware will ever see.
        if (std::strstr(value, "chunked") || std::strstr(value, "Chunked")) {
            chunked_ = true;
        }
    } else if (iEqual(name, "retry-after")) {
        size_t v = 0;
        // Only the delta-seconds form. An HTTP-date is legal and would need a
        // date parser plus a trustworthy clock, and core::RequestBudget clamps
        // whatever it gets anyway.
        if (parseSizeT(value, v)) {
            retryAfter_ = v > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(v);
        }
    } else if (iEqual(name, "location")) {
        const size_t n = std::strlen(value);
        const size_t cap = kMaxLocationLen - 1;
        const size_t take = n < cap ? n : cap;
        std::memcpy(location_, value, take);
        location_[take] = '\0';
    }
    return true;
}

bool ResponseReader::beginBody() {
    // 204 and 304 never have a body, whatever the headers claim. So does any
    // 1xx. Treating a Content-Length on one of those as real would leave the
    // reader waiting for bytes that are never coming.
    if (status_ == 204 || status_ == 304 || (status_ >= 100 && status_ < 200)) {
        state_ = State::Done;
        return true;
    }
    if (chunked_) {
        // Chunked wins over Content-Length; RFC 9112 says to ignore the length.
        haveLength_ = false;
        state_ = State::ChunkSize;
        return true;
    }
    if (haveLength_) {
        remaining_ = length_;
        if (remaining_ == 0) { state_ = State::Done; return true; }
        state_ = State::Body;
        return true;
    }
    // Neither: the body runs until the connection closes. finish() ends it.
    state_ = State::Body;
    return true;
}

bool ResponseReader::finish() {
    switch (state_) {
        case State::Done:
            return true;
        case State::Body:
            // Only legitimate when there was no Content-Length. If there was
            // one, the peer hung up early and the body we have is a fragment --
            // which must NOT look like a complete answer.
            if (!haveLength_) { state_ = State::Done; return true; }
            return fail(Error::Protocol);
        default:
            return fail(Error::Protocol);
    }
}

// ---------------------------------------------------------------------------

bool parseUrl(const char* url, Url& out) {
    out = Url{};
    if (!url) return false;

    const char* p = url;
    if (iStartsWith(p, "https://")) { out.tls = true;  out.port = 443; p += 8; }
    else if (iStartsWith(p, "http://")) { out.tls = false; out.port = 80; p += 7; }
    else return false;

    const char* hostStart = p;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#') ++p;
    const size_t hostLen = static_cast<size_t>(p - hostStart);
    if (hostLen == 0 || hostLen >= sizeof out.host) return false;
    std::memcpy(out.host, hostStart, hostLen);
    out.host[hostLen] = '\0';

    if (*p == ':') {
        ++p;
        uint32_t port = 0;
        bool any = false;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + static_cast<uint32_t>(*p - '0');
            if (port > 65535) return false;
            any = true;
            ++p;
        }
        if (!any || port == 0) return false;
        out.port = static_cast<uint16_t>(port);
    }

    // Everything from the slash on, query included, is the request target. An
    // empty path is "/" -- a request line without one is malformed.
    if (*p == '\0' || *p == '#') {
        out.path[0] = '/';
        out.path[1] = '\0';
    } else {
        if (*p != '/' && *p != '?') return false;
        size_t i = 0;
        if (*p == '?') out.path[i++] = '/';
        for (; *p && *p != '#'; ++p) {
            if (i + 1 >= sizeof out.path) return false;
            out.path[i++] = *p;
        }
        out.path[i] = '\0';
    }
    out.valid = true;
    return true;
}

} // namespace net
