#include "jwt.h"

#include <cstdio>
#include <cstring>

#include "../core/urlencode.h"

namespace google {
namespace {

const char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// Appends to a bounded buffer, tracking overflow rather than truncating
// silently: a truncated claim set produces a signature over the wrong bytes,
// which fails at Google with no local symptom.
struct Out {
    char* buf;
    size_t cap;
    size_t len = 0;
    bool overflow = false;

    void put(const char* s) {
        const size_t n = std::strlen(s);
        if (len + n >= cap) { overflow = true; return; }
        std::memcpy(buf + len, s, n);
        len += n;
    }
    void putc(char c) {
        if (len + 1 >= cap) { overflow = true; return; }
        buf[len++] = c;
    }
    void putI64(int64_t v) {
        char tmp[24];
        std::snprintf(tmp, sizeof tmp, "%lld", static_cast<long long>(v));
        put(tmp);
    }
    // JSON string escaping. Service account emails and scope URLs contain
    // nothing that needs it, but a configuration typo should not be able to
    // produce a malformed claim set.
    void putJsonString(const char* s) {
        putc('"');
        for (const char* p = s; *p; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c == '"' || c == '\\') { putc('\\'); putc(static_cast<char>(c)); }
            else if (c < 0x20) {
                char esc[8];
                std::snprintf(esc, sizeof esc, "\\u%04x", c);
                put(esc);
            } else {
                putc(static_cast<char>(c));
            }
        }
        putc('"');
    }
    bool finish() {
        if (overflow || len >= cap) return false;
        buf[len] = '\0';
        return true;
    }
};

// base64url-encode a run of bytes straight into an Out.
bool putB64(Out& o, const uint8_t* data, size_t n) {
    // Encoded length, no padding.
    const size_t need = (n * 4 + 2) / 3;
    if (o.len + need >= o.cap) { o.overflow = true; return false; }
    o.len += base64UrlEncode(data, n, o.buf + o.len, o.cap - o.len);
    return !o.overflow;
}

bool putB64Text(Out& o, const char* s) {
    return putB64(o, reinterpret_cast<const uint8_t*>(s), std::strlen(s));
}

} // namespace

size_t base64UrlEncode(const uint8_t* in, size_t inLen, char* out, size_t outCap) {
    if (out == nullptr || outCap == 0) return 0;
    const size_t need = (inLen * 4 + 2) / 3;      // no padding
    if (need + 1 > outCap) { out[0] = '\0'; return 0; }
    if (in == nullptr) { out[0] = '\0'; return 0; }

    size_t w = 0;
    size_t i = 0;
    while (i + 3 <= inLen) {
        const uint32_t v = (static_cast<uint32_t>(in[i]) << 16) |
                           (static_cast<uint32_t>(in[i + 1]) << 8) |
                            static_cast<uint32_t>(in[i + 2]);
        out[w++] = kAlphabet[(v >> 18) & 0x3F];
        out[w++] = kAlphabet[(v >> 12) & 0x3F];
        out[w++] = kAlphabet[(v >> 6) & 0x3F];
        out[w++] = kAlphabet[v & 0x3F];
        i += 3;
    }
    const size_t rem = inLen - i;
    if (rem == 1) {
        const uint32_t v = static_cast<uint32_t>(in[i]) << 16;
        out[w++] = kAlphabet[(v >> 18) & 0x3F];
        out[w++] = kAlphabet[(v >> 12) & 0x3F];
    } else if (rem == 2) {
        const uint32_t v = (static_cast<uint32_t>(in[i]) << 16) |
                           (static_cast<uint32_t>(in[i + 1]) << 8);
        out[w++] = kAlphabet[(v >> 18) & 0x3F];
        out[w++] = kAlphabet[(v >> 12) & 0x3F];
        out[w++] = kAlphabet[(v >> 6) & 0x3F];
    }
    out[w] = '\0';
    return w;
}

int buildSigningInput(const char* issuerEmail, const char* scope, const char* audience,
                      int64_t nowUtcSec, int32_t lifetimeSec,
                      char* out, size_t outCap) {
    if (!issuerEmail || !scope || !audience || !out || outCap == 0) return -1;
    if (lifetimeSec <= 0 || lifetimeSec > kMaxAssertionLifetimeSec)
        lifetimeSec = kMaxAssertionLifetimeSec;

    Out o{out, outCap};

    // Header. `kid` is optional and deliberately omitted: it identifies which
    // key signed the token, and with a single key it only adds bytes.
    putB64Text(o, "{\"alg\":\"RS256\",\"typ\":\"JWT\"}");
    o.putc('.');

    // Claim set. Built into a scratch buffer first because it has to be
    // base64url-encoded as a unit.
    char claims[512];
    Out c{claims, sizeof claims};
    c.put("{\"iss\":");    c.putJsonString(issuerEmail);
    c.put(",\"scope\":");  c.putJsonString(scope);
    c.put(",\"aud\":");    c.putJsonString(audience);
    c.put(",\"iat\":");    c.putI64(nowUtcSec);
    c.put(",\"exp\":");    c.putI64(nowUtcSec + lifetimeSec);
    c.put("}");
    // No `sub`. That claim impersonates a user and needs domain-wide
    // delegation, which a consumer Google account cannot grant -- it is why the
    // service account owns the calendar instead. See memory.md.
    if (!c.finish()) return -1;

    putB64Text(o, claims);
    if (!o.finish()) return -1;
    return static_cast<int>(o.len);
}

int buildAssertion(const char* issuerEmail, const char* scope, const char* audience,
                   int64_t nowUtcSec, int32_t lifetimeSec,
                   Signer& signer, char* out, size_t outCap) {
    const int inputLen = buildSigningInput(issuerEmail, scope, audience,
                                           nowUtcSec, lifetimeSec, out, outCap);
    if (inputLen < 0) return -1;

    uint8_t sig[512];
    const size_t sigLen = signer.signRs256(reinterpret_cast<const uint8_t*>(out),
                                           static_cast<size_t>(inputLen),
                                           sig, sizeof sig);
    // A Signer that reports more than it wrote would make putB64 read past the
    // buffer. Trusting an interface's return value that far is not worth it.
    if (sigLen == 0 || sigLen > sizeof sig) return -1;

    Out o{out, outCap};
    o.len = static_cast<size_t>(inputLen);
    o.putc('.');
    if (!putB64(o, sig, sigLen)) return -1;
    if (!o.finish()) return -1;
    return static_cast<int>(o.len);
}

const char* const kTokenGrantPrefix =
    "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&assertion=";

int buildTokenRequestBody(const char* assertion, char* out, size_t outCap) {
    if (!assertion || !out || outCap == 0) return -1;
    Out o{out, outCap};
    o.put(kTokenGrantPrefix);
    o.put(assertion);
    if (!o.finish()) return -1;
    return static_cast<int>(o.len);
}

} // namespace google
