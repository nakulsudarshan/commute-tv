#include "urlencode.h"

namespace core {
namespace {
bool unreserved(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

int encode(const char* in, char* out, size_t outSize, bool slashIsSafe) {
    if (!in || !out || outSize == 0) return -1;
    static const char* hex = "0123456789ABCDEF";
    size_t n = 0;
    for (const char* p = in; *p; ++p) {
        const unsigned char ch = static_cast<unsigned char>(*p);
        if (unreserved(static_cast<char>(ch)) || (slashIsSafe && ch == '/')) {
            if (n + 1 >= outSize) return -1;
            out[n++] = static_cast<char>(ch);
        } else {
            if (n + 3 >= outSize) return -1;
            out[n++] = '%';
            out[n++] = hex[ch >> 4];
            out[n++] = hex[ch & 0x0F];
        }
    }
    out[n] = 0;
    return static_cast<int>(n);
}
} // namespace

int urlEncode(const char* in, char* out, size_t outSize) {
    return encode(in, out, outSize, false);
}

int urlEncodePath(const char* in, char* out, size_t outSize) {
    return encode(in, out, outSize, true);
}

} // namespace core
