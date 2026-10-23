// rsa_signer.h - RS256 for the Google service-account assertion.
//
// google::Signer, implemented with the mbedTLS that is already in the ESP32
// image because TLS needs it. Thirty lines of real work; the rest of this file
// is the reasoning behind two decisions that are not obvious.
//
// 1. THE KEY IS PARSED PER SIGNATURE AND FREED IMMEDIATELY.
//
//    Keeping the parsed key alive is the obvious optimisation and the wrong one
//    here. mbedtls_pk_parse_key holds roughly 1-2 KB of RSA context, and the
//    very next thing that happens after signing is a TLS handshake wanting tens
//    of KB against a largest contiguous block of 110,580 bytes. Signing happens
//    once an hour when the token expires; ~30 ms of parsing to keep that KB out
//    of the way during the handshake is a trade worth making every time.
//
// 2. IT USES esp_fill_random RATHER THAN AN ENTROPY + CTR_DRBG PAIR.
//
//    RSASSA-PKCS1-v1_5 is deterministic and needs no randomness to sign, but
//    mbedtls_pk_sign wants a callable anyway. Standing up an entropy context and
//    a CTR-DRBG to satisfy an argument that will barely be used costs another
//    ~2 KB and a seeding delay. The ESP32's hardware RNG is a true RNG whenever
//    the radio is on -- which it always is here, because the whole point of
//    signing this is to make a network request.
//
// ⚠️ The key itself lives in secrets_generated.h, in unencrypted flash. Anyone
// holding the board can read it back. That is why the calendar is shared
// read-only with a service account that owns nothing else.
#pragma once
#include <cstddef>
#include <cstdint>

#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/version.h>

#if __has_include(<esp_random.h>)
  #include <esp_random.h>
#else
  #include <esp_system.h>
#endif

#include "google/jwt.h"

namespace platform {

class RsaSigner final : public google::Signer {
public:
    // `pemLen` MUST include the terminating NUL: mbedtls_pk_parse_key counts it
    // for PEM input and silently fails to parse without it. secrets_generated.h
    // emits `sizeof` rather than `strlen` for exactly this reason.
    RsaSigner(const char* pem, size_t pemLen) : pem_(pem), pemLen_(pemLen) {}

    size_t signRs256(const uint8_t* data, size_t len, uint8_t* out,
                     size_t outCap) override {
        if (!pem_ || pemLen_ == 0 || !data || !out) return fail("no key");

        uint8_t hash[32];
        const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        if (!md || mbedtls_md(md, data, len, hash) != 0) return fail("sha256");

        mbedtls_pk_context pk;
        mbedtls_pk_init(&pk);

        int rc;
#if MBEDTLS_VERSION_MAJOR >= 3
        rc = mbedtls_pk_parse_key(&pk, reinterpret_cast<const unsigned char*>(pem_),
                                  pemLen_, nullptr, 0, rng, nullptr);
#else
        rc = mbedtls_pk_parse_key(&pk, reinterpret_cast<const unsigned char*>(pem_),
                                  pemLen_, nullptr, 0);
#endif
        if (rc != 0) {
            mbedtls_pk_free(&pk);
            return fail("parse key", rc);
        }

        size_t sigLen = 0;
        // The signature of an RSA-2048 key is 256 bytes. mbedtls_pk_sign in 2.x
        // writes into `out` without being told its capacity, so refuse anything
        // it could overrun rather than trusting the caller.
        const size_t need = mbedtls_pk_get_len(&pk);
        if (need == 0 || need > outCap) {
            mbedtls_pk_free(&pk);
            return fail("signature would not fit");
        }
#if MBEDTLS_VERSION_MAJOR >= 3
        rc = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, hash, sizeof hash, out, outCap,
                             &sigLen, rng, nullptr);
#else
        rc = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, hash, sizeof hash, out, &sigLen,
                             rng, nullptr);
#endif
        // Freed before returning, on every path. See note 1 at the top.
        mbedtls_pk_free(&pk);

        if (rc != 0) return fail("sign", rc);
        ++signatures_;
        return sigLen;
    }

    const char* lastError() const { return lastError_; }
    int lastCode() const { return lastCode_; }
    uint32_t signatures() const { return signatures_; }

private:
    // Hardware RNG. A true RNG while the radio is on, which it always is by the
    // time anything wants a signature.
    static int rng(void*, unsigned char* out, size_t len) {
        esp_fill_random(out, len);
        return 0;
    }

    size_t fail(const char* what, int code = 0) {
        lastError_ = what;
        lastCode_ = code;
        return 0;
    }

    const char* pem_;
    size_t pemLen_;
    const char* lastError_ = "";
    int lastCode_ = 0;
    uint32_t signatures_ = 0;
};

} // namespace platform
