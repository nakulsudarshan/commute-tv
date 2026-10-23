// jwt.h - the signed assertion that gets us a Google access token.
//
// A service account authenticates by signing a JWT and exchanging it for an
// access token. There is no refresh token and nothing to expire on a shelf,
// which is exactly why this project uses a service account rather than user
// OAuth: no browser, no consent screen, nothing that stops working while the
// device sits on a desk.
//
// Everything here is pure string and byte work so it can be tested on the host.
// The one part that cannot be -- the RSA signature -- is behind Signer, which
// the ESP32 implements with mbedTLS (already in the SDK) and the tests
// implement with a stub. That split matters: the bugs in this flow are almost
// never in the RSA. They are in base64url padding, in the claim set, and in the
// clock -- all of which the stub lets us test exactly.
#pragma once
#include <cstddef>
#include <cstdint>

namespace google {

// base64url, no padding: RFC 7515 section 2. Note '-' and '_' rather than '+'
// and '/', and no '=' tail. Standard base64 here produces a token Google
// rejects with an unhelpful "Invalid JWT" and no clue which half is wrong.
//
// Returns characters written (excluding the terminator), or 0 if it did not
// fit. Always NUL-terminates when outCap > 0.
size_t base64UrlEncode(const uint8_t* in, size_t inLen, char* out, size_t outCap);

struct Signer {
    virtual ~Signer() = default;

    // RSASSA-PKCS1-v1_5 over SHA-256, the RS256 of the JWT header. Returns the
    // number of signature bytes written, or 0 on failure. An RSA-2048 service
    // account key produces 256 bytes.
    //
    // ESP32 implementation: mbedtls_md(SHA256) then mbedtls_pk_sign with the
    // key parsed by mbedtls_pk_parse_key from the service account JSON.
    virtual size_t signRs256(const uint8_t* data, size_t len,
                             uint8_t* out, size_t outCap) = 0;
};

// A JWT bearer assertion for the OAuth2 token endpoint.
//
// `nowUtcSec` is real UTC epoch seconds. This is the ONLY place in the codebase
// that uses UTC, and it has to: `iat` and `exp` are checked by Google against
// Google's clock, not ours. Everything else in this firmware is local time on
// purpose -- see core/timeutil.h.
//
// `lifetimeSec` is clamped to Google's maximum of one hour. Shorter is fine;
// the token that comes back lasts an hour regardless of how short the assertion
// was, so there is no reason to ask for the maximum.
//
// Returns the assertion length, or -1 if it did not fit or signing failed.
int buildAssertion(const char* issuerEmail, const char* scope, const char* audience,
                   int64_t nowUtcSec, int32_t lifetimeSec,
                   Signer& signer, char* out, size_t outCap);

// The signing input on its own: "<base64url(header)>.<base64url(claims)>".
// buildAssertion uses it; it is exposed because it is the thing worth asserting
// on in tests -- a stub Signer can record exactly what it was asked to sign.
int buildSigningInput(const char* issuerEmail, const char* scope, const char* audience,
                      int64_t nowUtcSec, int32_t lifetimeSec,
                      char* out, size_t outCap);

// The fixed prefix of the token request body, precomputed. `grant_type` is a
// URN full of colons and has to be percent-encoded. There is a test asserting
// this literal is exactly what core::urlEncode produces, so it cannot drift.
extern const char* const kTokenGrantPrefix;

// The form body for the token request: kTokenGrantPrefix followed by the
// assertion. The assertion needs no encoding -- base64url and '.' are already
// unreserved characters.
//
// Returns the body length, or -1 if it did not fit.
int buildTokenRequestBody(const char* assertion, char* out, size_t outCap);

constexpr int32_t kMaxAssertionLifetimeSec = 3600;

} // namespace google
