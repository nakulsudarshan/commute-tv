// places.h - matching what you typed in a calendar to somewhere on a map.
//
// WHY THIS EXISTS
// ---------------
// A calendar event carries a location as free text, and the normal path is to
// hand that text to AVV's location search. That works, but it is a guess: the
// search can return the wrong Ponttor, and it costs a request per appointment.
//
// For the handful of places you actually go, an exact coordinate is better than
// a good guess. This is that table -- but the interesting part is the matching,
// not the storage.
//
// THE LABELS ARE YOURS, NOT A MAP'S
// ---------------------------------
// A label here is compared against WHAT YOU TYPE, and nothing else. It does not
// have to match Google Maps, or OpenStreetMap, or what AVV calls the nearest
// stop. "McFIT", "SuperC", "Collective Incubator" are exactly right if that is
// what goes in the location field.
//
// Which means the match has to be forgiving, because people are not consistent
// with themselves:
//
//     "SuperC"                          -> SuperC
//     "superc"                          -> SuperC
//     "Super C"                         -> SuperC
//     "SuperC, Templergraben 57"        -> SuperC
//     "RWTH Hauptgebaeude"              -> RWTH Hauptgebaude
//     "RWTH Hauptgebäude"               -> RWTH Hauptgebaude
//
// That last pair is not a nicety. Calendar entries get typed on a phone with a
// German keyboard and on a laptop without one, so the same place arrives as
// "Hauptgebäude" and "Hauptgebaude" on different days. After ASCII folding
// those become "Hauptgebaeude" and "Hauptgebaude", which are not equal --
// so the normaliser collapses the German digraphs too.
//
// Nothing here is required for the system to work. A miss just means the
// location string goes to AVV's search, which is what would have happened
// anyway.
#pragma once
#include <cstddef>
#include <cstdint>

namespace core {

constexpr int kMaxPlaces = 8;
constexpr size_t kPlaceLabelLen = 40;

struct Place {
    char   label[kPlaceLabelLen] = {0};
    double lat = 0.0;
    double lon = 0.0;

    bool valid() const {
        // Aachen is near 50.78 N, 6.08 E. A zero coordinate is the shape of an
        // unfilled config entry, and 0,0 is in the Atlantic.
        return label[0] != '\0' && (lat != 0.0 || lon != 0.0);
    }
};

// Reduce a place name to something two people typing the same place will agree
// on: ASCII-folded, lower case, German digraphs collapsed (ae/oe/ue/ss ->
// a/o/u/s), every run of non-alphanumerics turned into one space, trimmed.
//
// Returns the length written. Always NUL-terminates when outSize > 0.
size_t normalizePlace(const char* in, size_t inLen, char* out, size_t outSize);

class PlaceBook {
public:
    // Returns false if the table is full or the entry is unusable. A rejected
    // entry is not a failure worth stopping for -- the location still resolves
    // through AVV -- but the caller should say so.
    bool add(const char* label, double lat, double lon);

    // Parse one "LABEL|LAT|LON" line as it appears in the config. An empty
    // label is skipped silently: that is an entry the user has not named yet.
    bool addFromConfigLine(const char* line);

    // The place whose label matches this location text, or nullptr.
    //
    // Matching, in order: exact normalised equality, then whole-word
    // containment -- "SuperC, Templergraben 57" contains "superc" as a word, so
    // it matches, while "supercomputer" does not. Where several labels match,
    // the LONGEST wins: "Aachen Hauptbahnhof" beats "Aachen".
    const Place* find(const char* location) const;

    int count() const { return count_; }
    const Place& at(int i) const { return places_[i]; }
    void clear() { count_ = 0; }

private:
    Place places_[kMaxPlaces];
    // The normalised form is computed once at add() time. Doing it per lookup
    // would mean normalising eight labels on every calendar poll for no reason.
    char  norm_[kMaxPlaces][kPlaceLabelLen * 2] = {};
    int   count_ = 0;
};

} // namespace core
