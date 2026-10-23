#include "places.h"

#include <cstdlib>
#include <cstring>

#include "textutil.h"

namespace core {
namespace {

bool isAlnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// Is `hay[at .. at+n)` a whole-word occurrence of a needle? Both strings are
// already normalised, so a word boundary is a space or an end.
bool wholeWordAt(const char* hay, size_t hayLen, size_t at, size_t n) {
    if (at > 0 && hay[at - 1] != ' ') return false;
    const size_t end = at + n;
    if (end < hayLen && hay[end] != ' ') return false;
    return true;
}

bool containsWord(const char* hay, const char* needle) {
    const size_t hayLen = std::strlen(hay);
    const size_t n = std::strlen(needle);
    if (n == 0 || n > hayLen) return false;
    for (size_t i = 0; i + n <= hayLen; ++i) {
        if (std::memcmp(hay + i, needle, n) == 0 && wholeWordAt(hay, hayLen, i, n))
            return true;
    }
    return false;
}

} // namespace

size_t normalizePlace(const char* in, size_t inLen, char* out, size_t outSize) {
    if (out == nullptr || outSize == 0) return 0;
    out[0] = '\0';
    if (in == nullptr) return 0;

    // Fold first, so "Hauptgebäude" and an already-ASCII "Hauptgebaeude" arrive
    // at the same place before the digraphs are collapsed.
    char folded[256];
    const size_t f = asciiFold(in, inLen, folded, sizeof folded);

    size_t w = 0;
    bool pendingSpace = false;
    for (size_t i = 0; i < f && w + 1 < outSize; ) {
        const char c = lower(folded[i]);

        if (!isAlnum(c)) {
            // Runs of punctuation and whitespace collapse to a single space,
            // and a leading one is dropped.
            if (w > 0) pendingSpace = true;
            ++i;
            continue;
        }

        if (pendingSpace) {
            out[w++] = ' ';
            pendingSpace = false;
            if (w + 1 >= outSize) break;
        }

        // Collapse the German digraphs an umlaut folds to, so a name typed with
        // the umlaut and one typed without it end up identical.
        if ((c == 'a' || c == 'o' || c == 'u') && i + 1 < f && lower(folded[i + 1]) == 'e') {
            out[w++] = c;
            i += 2;
            continue;
        }
        if (c == 's' && i + 1 < f && lower(folded[i + 1]) == 's') {
            out[w++] = 's';
            i += 2;
            continue;
        }

        out[w++] = c;
        ++i;
    }

    out[w] = '\0';
    return w;
}

bool PlaceBook::add(const char* label, double lat, double lon) {
    if (count_ >= kMaxPlaces) return false;
    if (label == nullptr || label[0] == '\0') return false;
    if (lat == 0.0 && lon == 0.0) return false;
    // A transposed pair is the classic configuration mistake here -- Google
    // Maps shows lat,lon and openrouteservice wants lon,lat. In Aachen the
    // longitude is single digit and the latitude is fifty-something, so a swap
    // is detectable rather than merely suspicious.
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return false;

    Place& p = places_[count_];
    p = Place{};
    if (!copyStr(p.label, sizeof p.label, label, std::strlen(label))) {
        // A truncated label would still match its own truncation, so this is
        // survivable -- but it is not what the user wrote.
    }
    p.lat = lat;
    p.lon = lon;

    normalizePlace(p.label, std::strlen(p.label), norm_[count_], sizeof norm_[0]);
    if (norm_[count_][0] == '\0') return false;   // a label of pure punctuation

    ++count_;
    return true;
}

bool PlaceBook::addFromConfigLine(const char* line) {
    if (line == nullptr) return false;
    const char* bar1 = std::strchr(line, '|');
    if (bar1 == nullptr) return false;
    const char* bar2 = std::strchr(bar1 + 1, '|');
    if (bar2 == nullptr) return false;

    char label[kPlaceLabelLen];
    const size_t labelLen = static_cast<size_t>(bar1 - line);
    copyStr(label, sizeof label, line, labelLen);

    // Trim: config files collect trailing spaces.
    size_t end = std::strlen(label);
    while (end > 0 && (label[end - 1] == ' ' || label[end - 1] == '\t')) label[--end] = '\0';
    const char* start = label;
    while (*start == ' ' || *start == '\t') ++start;
    if (*start == '\0') return false;             // an entry the user has not named yet

    char latBuf[32];
    copyStr(latBuf, sizeof latBuf, bar1 + 1, static_cast<size_t>(bar2 - bar1 - 1));

    return add(start, std::atof(latBuf), std::atof(bar2 + 1));
}

const Place* PlaceBook::find(const char* location) const {
    if (location == nullptr || location[0] == '\0') return nullptr;

    char needle[kPlaceLabelLen * 2];
    if (normalizePlace(location, std::strlen(location), needle, sizeof needle) == 0)
        return nullptr;

    const Place* best = nullptr;
    size_t bestLen = 0;

    for (int i = 0; i < count_; ++i) {
        const char* n = norm_[i];
        const size_t len = std::strlen(n);
        if (len == 0) continue;

        const bool hit = (std::strcmp(needle, n) == 0) || containsWord(needle, n);
        if (!hit) continue;

        // The longest match wins, so "Aachen Hauptbahnhof" is not beaten by a
        // shorter "Aachen" that happens to sit earlier in the table.
        if (best == nullptr || len > bestLen) {
            best = &places_[i];
            bestLen = len;
        }
    }
    return best;
}

} // namespace core
