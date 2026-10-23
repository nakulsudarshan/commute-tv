#include "smalltv_request.h"

#include <cstring>

#include "../core/urlencode.h"

namespace smalltv {

const char* const kUploadFieldCandidates[] = {
    "file", "upload", "data", "image", "filename", "doUpload",
};
const size_t kUploadFieldCandidateCount =
    sizeof(kUploadFieldCandidates) / sizeof(kUploadFieldCandidates[0]);

namespace {

// Appends with bounds checking and no truncation: a path that does not fit is
// an error, never a shorter path that means something else.
struct Appender {
    char* out;
    size_t cap;
    size_t n = 0;
    bool ok = true;

    Appender(char* o, size_t c) : out(o), cap(c) {
        if (cap == 0) ok = false;
        else out[0] = 0;
    }

    void str(const char* s) {
        if (!ok || s == nullptr) return;
        for (const char* p = s; *p; ++p) {
            if (n + 1 >= cap) { ok = false; return; }
            out[n++] = *p;
        }
        out[n] = 0;
    }

    void num(int v) {
        char tmp[12];
        int t = 0;
        bool neg = v < 0;
        unsigned int u = neg ? static_cast<unsigned int>(-(v + 1)) + 1u
                             : static_cast<unsigned int>(v);
        if (u == 0) tmp[t++] = '0';
        while (u > 0 && t < 11) { tmp[t++] = static_cast<char>('0' + (u % 10u)); u /= 10u; }
        if (neg) { if (n + 1 >= cap) { ok = false; return; } out[n++] = '-'; }
        while (t > 0) {
            if (n + 1 >= cap) { ok = false; return; }
            out[n++] = tmp[--t];
        }
        out[n] = 0;
    }

    // Percent-encodes a LittleFS path into the remaining space, leaving the
    // separators alone -- see core::urlEncodePath for why that matters here.
    // Percent-encode everything, INCLUDING slashes -- what JavaScript's
    // encodeURIComponent does, and what the device's delete button sends.
    void encodedFull(const char* s) {
        if (!ok || s == nullptr) return;
        const int w = core::urlEncode(s, out + n, cap - n);
        if (w < 0) { ok = false; return; }
        n += static_cast<size_t>(w);
    }

    void encodedPath(const char* s) {
        if (!ok || s == nullptr) return;
        const int w = core::urlEncodePath(s, out + n, cap - n);
        if (w < 0) { ok = false; return; }
        n += static_cast<size_t>(w);
    }

    int finish() const { return ok ? static_cast<int>(n) : -1; }
};

// Joins a directory and a file name into "/image/commute.jpg" without ever
// producing a doubled or missing slash.
void joinPath(const char* dir, const char* name, char* out, size_t cap) {
    Appender a(out, cap);
    if (dir == nullptr || dir[0] == '\0') a.str("/");
    else a.str(dir);
    const size_t len = std::strlen(out);
    if (len == 0 || out[len - 1] != '/') a.str("/");
    a.str(name);
    if (!a.ok) out[0] = 0;
}

char toBase64Digit(unsigned v) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    return kAlphabet[v & 63u];
}

} // namespace

// ---------------------------------------------------------------------------
// URLs
// ---------------------------------------------------------------------------

int buildUploadPath(const char* dir, char* out, size_t outSize) {
    Appender a(out, outSize);
    a.str("/doUpload?dir=");
    a.str(dir && dir[0] ? dir : "/image/");
    return a.finish();
}

int buildSetThemePath(int theme, char* out, size_t outSize) {
    if (theme < 0) return -1;
    Appender a(out, outSize);
    a.str("/set?theme=");
    a.num(theme);
    return a.finish();
}

int buildSetImagePath(const char* dir, const char* name, char* out, size_t outSize) {
    if (name == nullptr || name[0] == '\0') return -1;
    char full[kMaxPath];
    joinPath(dir, name, full, sizeof full);
    if (full[0] == '\0') return -1;

    Appender a(out, outSize);
    a.str("/set?img=");
    a.encodedPath(full);
    return a.finish();
}

int buildFileListPath(const char* dir, char* out, size_t outSize) {
    Appender a(out, outSize);
    a.str("/filelist?dir=");
    a.str(dir && dir[0] ? dir : "/image/");
    return a.finish();
}

const char* deleteStyleName(DeleteStyle s) {
    switch (s) {
        case DeleteStyle::DoubledSlash:   return "dir//name";
        case DeleteStyle::UiExact:        return "ui exact (dir//name, encoded)";
        case DeleteStyle::SingleSlashEnc: return "dir/name, encoded";
        case DeleteStyle::SingleSlash:    return "dir/name";
        case DeleteStyle::NameOnly:       return "name only";
    }
    return "?";
}

// Build "dir//name" -- the separator doubled, which is what the device stores
// and what its own UI passes to deletef(). joinPath deliberately avoids the
// doubled slash, which is right everywhere except here.
static void joinPathDoubled(const char* dir, const char* name, char* out, size_t cap) {
    Appender d(out, cap);
    const char* base = (dir == nullptr || dir[0] == '\0') ? "/" : dir;
    d.str(base);
    const size_t len = std::strlen(out);
    if (len == 0 || out[len - 1] != '/') d.str("/");
    d.str("/");
    d.str(name);
    if (d.finish() < 0) out[0] = '\0';
}

int buildDeletePath(const char* dir, const char* name, DeleteStyle style,
                    char* out, size_t outSize) {
    if (name == nullptr || name[0] == '\0') return -1;

    char full[kMaxPath];
    switch (style) {
        case DeleteStyle::NameOnly: {
            Appender n(full, sizeof full);
            n.str(name);
            if (n.finish() < 0) return -1;
            break;
        }
        case DeleteStyle::UiExact:
        case DeleteStyle::DoubledSlash:
            joinPathDoubled(dir, name, full, sizeof full);
            break;
        default:
            joinPath(dir, name, full, sizeof full);
            break;
    }
    if (full[0] == '\0') return -1;

    Appender a(out, outSize);
    a.str("/delete?file=");
    if (style == DeleteStyle::UiExact || style == DeleteStyle::SingleSlashEnc)
        a.encodedFull(full);
    else
        a.encodedPath(full);
    return a.finish();
}

int buildDeletePath(const char* dir, const char* name, char* out, size_t outSize) {
    return buildDeletePath(dir, name, DeleteStyle::DoubledSlash, out, outSize);
}

int buildSpacePath(char* out, size_t outSize) {
    Appender a(out, outSize);
    a.str("/space.json");
    return a.finish();
}

int buildAlbumPath(char* out, size_t outSize) {
    Appender a(out, outSize);
    a.str("/album.json");
    return a.finish();
}

int buildSetAlbumPath(int intervalSec, int autoplay, char* out, size_t outSize) {
    if (intervalSec < 1) intervalSec = 1;
    if (intervalSec > 3600) intervalSec = 3600;
    Appender a(out, outSize);
    a.str("/set?i_i=");
    a.num(intervalSec);
    a.str("&autoplay=");
    a.num(autoplay ? 1 : 0);
    return a.finish();
}

// Reads one integer field out of a small flat object. The value may be quoted:
// this firmware is inconsistent about it between endpoints.
static bool intField(const char* body, size_t len, const char* key, int& out) {
    const size_t klen = std::strlen(key);
    for (size_t i = 0; i + klen <= len; ++i) {
        if (std::memcmp(body + i, key, klen) != 0) continue;
        size_t p = i + klen;
        while (p < len && (body[p] == ' ' || body[p] == ':' || body[p] == '"')) ++p;
        bool neg = false;
        if (p < len && body[p] == '-') { neg = true; ++p; }
        if (p >= len || body[p] < '0' || body[p] > '9') return false;
        int v = 0;
        while (p < len && body[p] >= '0' && body[p] <= '9' && v < 100000000) {
            v = v * 10 + (body[p] - '0');
            ++p;
        }
        out = neg ? -v : v;
        return true;
    }
    return false;
}

bool parseAlbum(const char* body, size_t len, int& autoplay, int& intervalSec) {
    if (body == nullptr) return false;
    const bool a = intField(body, len, "\"autoplay\"", autoplay);
    const bool i = intField(body, len, "\"i_i\"", intervalSec);
    return a || i;
}

bool parseFreeBytes(const char* body, size_t len, uint32_t& freeBytes) {
    if (body == nullptr) return false;
    // A four-field object is not worth a JSON reader. Find "free", step over
    // the quote, colon and any spaces, then read digits.
    for (size_t i = 0; i + 6 <= len; ++i) {
        if (std::memcmp(body + i, "\"free\"", 6) != 0) continue;
        size_t p = i + 6;
        while (p < len && (body[p] == ' ' || body[p] == ':' || body[p] == '"')) ++p;
        if (p >= len || body[p] < '0' || body[p] > '9') return false;
        uint32_t v = 0;
        while (p < len && body[p] >= '0' && body[p] <= '9') {
            const uint32_t d = static_cast<uint32_t>(body[p] - '0');
            if (v > (0xFFFFFFFFu - d) / 10u) return false;   // implausible, not truncated
            v = v * 10u + d;
            ++p;
        }
        freeBytes = v;
        return true;
    }
    return false;
}

bool bodySaysFailure(const char* body, size_t len) {
    if (body == nullptr) return false;
    // Short, case-insensitive scan for "fail". The body is a status word, not a
    // document; anything longer than a few characters is a file listing, which
    // is what a SUCCESSFUL call returns.
    for (size_t i = 0; i + 4 <= len; ++i) {
        const char a = body[i]     | 0x20;
        const char b = body[i + 1] | 0x20;
        const char c2 = body[i + 2] | 0x20;
        const char d = body[i + 3] | 0x20;
        if (a == 'f' && b == 'a' && c2 == 'i' && d == 'l') return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Multipart
// ---------------------------------------------------------------------------

bool buildMultipart(const char* boundary, const char* field, const char* filename,
                    const char* payloadContentType, MultipartFrame& out) {
    if (!boundary || !*boundary || !field || !*field || !filename || !*filename)
        return false;
    if (payloadContentType == nullptr || *payloadContentType == '\0')
        payloadContentType = "image/jpeg";

    {
        Appender a(out.contentType, sizeof out.contentType);
        a.str("multipart/form-data; boundary=");
        a.str(boundary);
        if (a.finish() < 0) return false;
    }
    {
        Appender a(out.header, sizeof out.header);
        a.str("--");
        a.str(boundary);
        a.str("\r\nContent-Disposition: form-data; name=\"");
        a.str(field);
        a.str("\"; filename=\"");
        a.str(filename);
        a.str("\"\r\nContent-Type: ");
        a.str(payloadContentType);
        a.str("\r\n\r\n");
        const int n = a.finish();
        if (n < 0) return false;
        out.headerLen = static_cast<size_t>(n);
    }
    {
        Appender a(out.trailer, sizeof out.trailer);
        a.str("\r\n--");
        a.str(boundary);
        a.str("--\r\n");
        const int n = a.finish();
        if (n < 0) return false;
        out.trailerLen = static_cast<size_t>(n);
    }
    return true;
}

bool makeBoundary(uint32_t nonce, char* out, size_t outSize) {
    Appender a(out, outSize);
    a.str("----commutetv");
    // Eight hex digits: enough entropy that a collision with JPEG data is
    // negligible, and fixed-width so the frame size is predictable.
    static const char* hex = "0123456789abcdef";
    for (int shift = 28; shift >= 0; shift -= 4) {
        const char c = hex[(nonce >> shift) & 0xFu];
        char s[2] = {c, 0};
        a.str(s);
    }
    return a.finish() > 0;
}

bool boundaryIsSafe(const char* boundary, const uint8_t* payload, size_t len) {
    if (boundary == nullptr || *boundary == '\0') return false;
    if (payload == nullptr || len == 0) return true;
    const size_t bl = std::strlen(boundary);
    if (bl > len) return true;
    for (size_t i = 0; i + bl <= len; ++i)
        if (std::memcmp(payload + i, boundary, bl) == 0) return false;
    return true;
}

int buildBasicAuth(const char* user, const char* pass, char* out, size_t outSize) {
    if (user == nullptr || out == nullptr) return -1;
    if (pass == nullptr) pass = "";

    char raw[128];
    Appender r(raw, sizeof raw);
    r.str(user);
    r.str(":");
    r.str(pass);
    if (r.finish() < 0) return -1;
    const size_t rawLen = static_cast<size_t>(r.finish());

    Appender a(out, outSize);
    a.str("Basic ");
    if (!a.ok) return -1;

    for (size_t i = 0; i < rawLen; i += 3) {
        const unsigned b0 = static_cast<unsigned char>(raw[i]);
        const unsigned b1 = (i + 1 < rawLen) ? static_cast<unsigned char>(raw[i + 1]) : 0u;
        const unsigned b2 = (i + 2 < rawLen) ? static_cast<unsigned char>(raw[i + 2]) : 0u;
        const unsigned triple = (b0 << 16) | (b1 << 8) | b2;

        char quad[5];
        quad[0] = toBase64Digit(triple >> 18);
        quad[1] = toBase64Digit(triple >> 12);
        quad[2] = (i + 1 < rawLen) ? toBase64Digit(triple >> 6) : '=';
        quad[3] = (i + 2 < rawLen) ? toBase64Digit(triple) : '=';
        quad[4] = 0;
        a.str(quad);
    }
    return a.finish();
}

// ---------------------------------------------------------------------------
// Response parsing
// ---------------------------------------------------------------------------

bool parseThemeFromAppJson(const char* body, size_t len, int& theme) {
    if (body == nullptr || len == 0) return false;
    static const char kKey[] = "\"theme\"";
    const size_t keyLen = sizeof(kKey) - 1;
    if (len < keyLen) return false;

    for (size_t i = 0; i + keyLen <= len; ++i) {
        if (std::memcmp(body + i, kKey, keyLen) != 0) continue;

        size_t j = i + keyLen;
        while (j < len && (body[j] == ' ' || body[j] == '\t')) ++j;
        if (j >= len || body[j] != ':') return false;
        ++j;
        while (j < len && (body[j] == ' ' || body[j] == '\t' || body[j] == '"')) ++j;

        bool neg = false;
        if (j < len && body[j] == '-') { neg = true; ++j; }
        if (j >= len || body[j] < '0' || body[j] > '9') return false;

        int v = 0;
        while (j < len && body[j] >= '0' && body[j] <= '9') {
            v = v * 10 + (body[j] - '0');
            if (v > 100000) return false;   // not a theme index; refuse it
            ++j;
        }
        theme = neg ? -v : v;
        return true;
    }
    return false;
}

bool fileListMentions(const char* body, size_t len, const char* name) {
    if (body == nullptr || name == nullptr || *name == '\0') return false;
    const size_t nl = std::strlen(name);
    if (nl > len) return false;
    for (size_t i = 0; i + nl <= len; ++i)
        if (std::memcmp(body + i, name, nl) == 0) return true;
    return false;
}


bool parseJsonString(const char* body, size_t len, const char* key, char* out,
                     size_t outCap) {
    if (!body || !key || !out || outCap == 0) return false;
    out[0] = '\0';
    const size_t klen = std::strlen(key);
    if (klen == 0 || len < klen + 4) return false;

    for (size_t i = 0; i + klen + 1 < len; ++i) {
        if (body[i] != '"') continue;
        if (std::strncmp(body + i + 1, key, klen) != 0) continue;
        size_t j = i + 1 + klen;
        if (j >= len || body[j] != '"') continue;
        ++j;
        while (j < len && (body[j] == ' ' || body[j] == '\t')) ++j;
        if (j >= len || body[j] != ':') continue;
        ++j;
        while (j < len && (body[j] == ' ' || body[j] == '\t')) ++j;
        if (j >= len || body[j] != '"') return false;   // present but not a string
        ++j;

        size_t n = 0;
        // No escape handling: a hostname cannot contain one, and inventing
        // unescaping for a field that never needs it is how a reader grows a
        // bug it will never exercise.
        //
        // ⚠️ EVERY failure below clears the buffer before returning. Writing as
        // we go and bailing out later leaves a PREFIX of the real value sitting
        // there, and a caller that ignores the return code then points a clock
        // -- or a certificate -- at "ntp.a". Caught by the test that expected an
        // empty buffer, which is the whole reason it checks that.
        while (j < len && body[j] != '"') {
            if (n + 1 >= outCap) { out[0] = '\0'; return false; }
            out[n++] = body[j++];
        }
        if (j >= len) { out[0] = '\0'; return false; }   // unterminated
        out[n] = '\0';
        if (n == 0) return false;
        return true;
    }
    return false;
}

} // namespace smalltv
