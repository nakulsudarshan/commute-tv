// Stub. See ../README.md -- syntax only, no behaviour.
#pragma once
#include <cstddef>
typedef struct { int dummy; } mbedtls_pk_context;
void mbedtls_pk_init(mbedtls_pk_context*);
void mbedtls_pk_free(mbedtls_pk_context*);
size_t mbedtls_pk_get_len(const mbedtls_pk_context*);
int mbedtls_pk_parse_key(mbedtls_pk_context*, const unsigned char*, size_t,
                         const unsigned char*, size_t);
int mbedtls_pk_sign(mbedtls_pk_context*, int, const unsigned char*, size_t,
                    unsigned char*, size_t*,
                    int (*)(void*, unsigned char*, size_t), void*);
