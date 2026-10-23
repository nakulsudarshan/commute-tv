#include "textutil.h"
#include <cstring>

namespace core {
namespace {

// A code point and what it becomes. Kept to the ranges that actually turn up in
// German and neighbouring-language transit data, plus the punctuation HAFAS
// emits in free text. Sorted by code point so the lookup can stop early.
struct Fold { unsigned cp; const char* to; };

const Fold kFolds[] = {
    {0x00A0, " "},   {0x00AB, "\""}, {0x00AD, ""},    {0x00B4, "'"},
    {0x00BB, "\""},
    {0x00C0, "A"},  {0x00C1, "A"},  {0x00C2, "A"},  {0x00C3, "A"},
    {0x00C4, "Ae"}, {0x00C5, "A"},  {0x00C6, "Ae"}, {0x00C7, "C"},
    {0x00C8, "E"},  {0x00C9, "E"},  {0x00CA, "E"},  {0x00CB, "E"},
    {0x00CC, "I"},  {0x00CD, "I"},  {0x00CE, "I"},  {0x00CF, "I"},
    {0x00D0, "D"},  {0x00D1, "N"},  {0x00D2, "O"},  {0x00D3, "O"},
    {0x00D4, "O"},  {0x00D5, "O"},  {0x00D6, "Oe"}, {0x00D7, "x"},
    {0x00D8, "O"},  {0x00D9, "U"},  {0x00DA, "U"},  {0x00DB, "U"},
    {0x00DC, "Ue"}, {0x00DD, "Y"},  {0x00DE, "Th"}, {0x00DF, "ss"},
    {0x00E0, "a"},  {0x00E1, "a"},  {0x00E2, "a"},  {0x00E3, "a"},
    {0x00E4, "ae"}, {0x00E5, "a"},  {0x00E6, "ae"}, {0x00E7, "c"},
    {0x00E8, "e"},  {0x00E9, "e"},  {0x00EA, "e"},  {0x00EB, "e"},
    {0x00EC, "i"},  {0x00ED, "i"},  {0x00EE, "i"},  {0x00EF, "i"},
    {0x00F0, "d"},  {0x00F1, "n"},  {0x00F2, "o"},  {0x00F3, "o"},
    {0x00F4, "o"},  {0x00F5, "o"},  {0x00F6, "oe"}, {0x00F7, "/"},
    {0x00F8, "o"},  {0x00F9, "u"},  {0x00FA, "u"},  {0x00FB, "u"},
    {0x00FC, "ue"}, {0x00FD, "y"},  {0x00FE, "th"}, {0x00FF, "y"},
    {0x0152, "Oe"}, {0x0153, "oe"}, {0x0160, "S"},  {0x0161, "s"},
    {0x017D, "Z"},  {0x017E, "z"},
    {0x1E9E, "SS"},                                   // capital sharp s
    {0x2010, "-"},  {0x2011, "-"},  {0x2012, "-"},  {0x2013, "-"},
    {0x2014, "-"},  {0x2015, "-"},
    {0x2018, "'"},  {0x2019, "'"},  {0x201A, "'"},  {0x201B, "'"},
    {0x201C, "\""}, {0x201D, "\""}, {0x201E, "\""}, {0x201F, "\""},
    {0x2022, "-"},  {0x2026, "..."},
    {0x202F, " "},  {0x205F, " "},
    {0x20AC, "EUR"},
    {0x2192, "->"},
};

const char* lookup(unsigned cp) {
    for (const Fold& f : kFolds) {
        if (f.cp == cp) return f.to;
        if (f.cp > cp) break;
    }
    return nullptr;
}

// Decode one UTF-8 sequence. Advances `i`. Returns the code point, or 0xFFFD
// for anything malformed -- a truncated or overlong sequence must not make the
// decoder run off the end or resynchronise in the middle of a character.
unsigned decodeUtf8(const char* s, size_t len, size_t& i) {
    const unsigned char c0 = static_cast<unsigned char>(s[i]);
    size_t extra;
    unsigned cp;
    if (c0 < 0x80)        { ++i; return c0; }
    else if ((c0 & 0xE0) == 0xC0) { extra = 1; cp = c0 & 0x1Fu; }
    else if ((c0 & 0xF0) == 0xE0) { extra = 2; cp = c0 & 0x0Fu; }
    else if ((c0 & 0xF8) == 0xF0) { extra = 3; cp = c0 & 0x07u; }
    else { ++i; return 0xFFFD; }

    if (i + extra >= len) {          // not enough bytes for a complete sequence
        i = len;
        return 0xFFFD;
    }
    for (size_t k = 1; k <= extra; ++k) {
        const unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) { i += k; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    i += extra + 1;
    return cp;
}

} // namespace

size_t asciiFold(const char* in, size_t inLen, char* out, size_t outSize,
                 bool* truncated) {
    if (truncated) *truncated = false;
    if (out == nullptr || outSize == 0) { if (truncated) *truncated = inLen > 0; return 0; }
    if (in == nullptr) { out[0] = '\0'; return 0; }

    size_t w = 0;
    const size_t cap = outSize - 1;

    size_t i = 0;
    for (; i < inLen && in[i] != '\0'; ) {
        if (w >= cap) { if (truncated) *truncated = true; break; }
        const unsigned cp = decodeUtf8(in, inLen, i);

        const char* rep;
        char one[2] = {0, 0};
        if (cp >= 0x20 && cp < 0x7F) {
            one[0] = static_cast<char>(cp);
            rep = one;
        } else if (cp == '\t' || cp == '\n' || cp == '\r') {
            one[0] = ' ';
            rep = one;
        } else {
            rep = lookup(cp);
            if (rep == nullptr) rep = "?";
        }

        const size_t n = std::strlen(rep);
        if (w + n > cap) {              // never split a replacement
            if (truncated) *truncated = true;
            break;
        }
        std::memcpy(out + w, rep, n);
        w += n;
    }

    out[w] = '\0';
    return w;
}

bool foldInto(char* dst, size_t dstSize, const char* src, size_t srcLen) {
    bool trunc = false;
    asciiFold(src, srcLen, dst, dstSize, &trunc);
    return !trunc;
}

bool copyStr(char* dst, size_t dstSize, const char* src, size_t srcLen) {
    if (dst == nullptr || dstSize == 0) return false;
    if (src == nullptr) { dst[0] = '\0'; return true; }
    size_t n = srcLen;
    if (n > dstSize - 1) n = dstSize - 1;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
    return n == srcLen;
}

} // namespace core
