// Stub. See ../README.md -- syntax only, no behaviour.
#pragma once
#include <cstddef>
#define MBEDTLS_MD_SHA256 6
typedef struct { int dummy; } mbedtls_md_info_t;
const mbedtls_md_info_t* mbedtls_md_info_from_type(int);
int mbedtls_md(const mbedtls_md_info_t*, const unsigned char*, size_t, unsigned char*);
