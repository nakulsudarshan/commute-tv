#include "address.h"

#include <cstring>

namespace core {
namespace {

// Folded already, so no umlauts to worry about here.
const char* const kCountries[] = {
    "germany", "deutschland", "de",
    "netherlands", "nederland", "niederlande", "nl",
    "belgium", "belgie", "belgien", "be",
};

char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iEqualN(const char* a, size_t alen, const char* b) {
    const size_t blen = std::strlen(b);
    if (alen != blen) return false;
    for (size_t i = 0; i < alen; ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isSpace(char c) { return c == ' ' || c == '\t'; }

void trim(const char*& s, size_t& n) {
    while (n && isSpace(*s)) { ++s; --n; }
    while (n && isSpace(s[n - 1])) --n;
}

} // namespace

bool isDroppableCountry(const char* s, size_t len) {
    trim(s, len);
    if (len == 0) return true;
    for (const char* c : kCountries) {
        if (iEqualN(s, len, c)) return true;
    }
    return false;
}

// A German postcode is five digits. Returns how many leading characters to skip
// so that "52072 Aachen" becomes "Aachen" -- and 0 for "30 Rockefeller Plaza",
// which is a house number and must survive.
size_t postcodePrefixLen(const char* s, size_t len) {
    size_t i = 0;
    while (i < len && isDigit(s[i])) ++i;
    if (i != 5) return 0;                 // exactly five, or it is not a postcode
    size_t j = i;
    while (j < len && isSpace(s[j])) ++j;
    if (j == i) return 0;                 // "52072Aachen" is not a postcode either
    if (j >= len) return 0;               // nothing after it; keep what we have
    return j;
}

// Is this whitespace-delimited token a house number? "28", "2a", "12-14", "3/5".
// Not five digits -- that is a postcode, and mistaking one for the other is how
// "52062 Aachen" would come to look like a street.
static bool isHouseNumber(const char* t, size_t n) {
    if (n == 0 || n > 8) return false;
    size_t digits = 0;
    while (digits < n && isDigit(t[digits])) ++digits;
    if (digits == 0 || digits > 4) return false;
    for (size_t i = digits; i < n; ++i) {
        const char c = t[i];
        if (!isDigit(c) && !isLetter(c) && c != '-' && c != '/') return false;
    }
    return true;
}

bool looksLikeStreet(const char* s, size_t len) {
    trim(s, len);
    if (len == 0) return false;

    bool sawWord = false;
    size_t i = 0;
    while (i < len) {
        while (i < len && isSpace(s[i])) ++i;
        size_t j = i;
        while (j < len && !isSpace(s[j])) ++j;
        if (j == i) break;

        // A house number only counts once there is a street name in front of
        // it. That is what stops a lone "28", or a postcode that escaped the
        // trimmer, from reading as an address.
        if (sawWord && isHouseNumber(s + i, j - i)) return true;

        for (size_t k = i; k < j; ++k) {
            if (isLetter(s[k])) { sawWord = true; break; }
        }
        i = j;
    }
    return false;
}

namespace {

struct Part { const char* p; size_t n; };

// Splits on commas and drops countries and postcodes. Returns how many parts
// survived, never more than `cap`.
int collectParts(const char* in, size_t inLen, Part* out, int cap) {
    int kept = 0;
    size_t i = 0;
    while (i < inLen && kept < cap) {
        size_t j = i;
        while (j < inLen && in[j] != ',') ++j;

        const char* p = in + i;
        size_t n = j - i;
        trim(p, n);

        if (n && !isDroppableCountry(p, n)) {
            const size_t skip = postcodePrefixLen(p, n);
            p += skip;
            n -= skip;
            trim(p, n);
            if (n) {
                out[kept].p = p;
                out[kept].n = n;
                ++kept;
            }
        }
        i = (j < inLen) ? j + 1 : inLen;
    }
    return kept;
}

// ", "-joined, truncated rather than overflowed, always NUL-terminated.
size_t join(const Part* parts, int count, char* out, size_t outCap) {
    size_t used = 0;
    for (int k = 0; k < count; ++k) {
        if (used) {
            if (used + 2 >= outCap) break;
            out[used++] = ',';
            out[used++] = ' ';
        }
        const size_t room = outCap - 1 - used;
        const size_t take = parts[k].n < room ? parts[k].n : room;
        std::memcpy(out + used, parts[k].p, take);
        used += take;
    }
    out[used] = '\0';
    return used;
}

} // namespace

size_t simplifyAddress(const char* in, size_t inLen, char* out, size_t outCap) {
    if (!out || outCap == 0) return 0;
    out[0] = '\0';
    if (!in || inLen == 0) return 0;

    Part parts[kMaxAddressParts];
    const int n = collectParts(in, inLen, parts, kMaxAddressParts);
    return join(parts, n, out, outCap);
}

size_t transitQuery(const char* in, size_t inLen, char* out, size_t outCap) {
    if (!out || outCap == 0) return 0;
    out[0] = '\0';
    if (!in || inLen == 0) return 0;

    Part parts[kMaxAddressParts];
    const int n = collectParts(in, inLen, parts, kMaxAddressParts);
    if (n <= 0) return 0;

    // Start at the street, if there is one. No street found means nothing was
    // recognisably an address, and then the whole string is the best guess
    // available -- which is exactly what simplifyAddress() would have produced.
    int first = 0;
    for (int i = 0; i < n; ++i) {
        if (looksLikeStreet(parts[i].p, parts[i].n)) { first = i; break; }
    }
    return join(parts + first, n - first, out, outCap);
}

size_t addressLabel(const char* in, size_t inLen, char* out, size_t outCap) {
    if (!out || outCap == 0) return 0;
    out[0] = '\0';
    if (!in || inLen == 0) return 0;

    Part parts[1];
    if (collectParts(in, inLen, parts, 1) != 1) return 0;
    const size_t take = parts[0].n < outCap - 1 ? parts[0].n : outCap - 1;
    std::memcpy(out, parts[0].p, take);
    out[take] = '\0';
    return take;
}

} // namespace core
