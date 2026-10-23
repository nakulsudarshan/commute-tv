#include "test.h"
#include "../src/core/places.h"

#include <cstdio>
#include <cstring>

using namespace core;

namespace {

const char* norm(const char* s) {
    static char buf[128];
    normalizePlace(s, std::strlen(s), buf, sizeof buf);
    return buf;
}

// The five places Nakul actually gave, labelled the way he labels them.
PlaceBook realBook() {
    PlaceBook b;
    b.add("McFIT", 50.770770, 6.118744);
    b.add("Aachen Hauptbahnhof", 50.768105, 6.091202);
    b.add("Collective Incubator", 50.785820, 6.109085);
    b.add("SuperC", 50.778091, 6.078766);
    b.add("RWTH Hauptgebaude", 50.777736, 6.077915);
    return b;
}

const char* found(const PlaceBook& b, const char* what) {
    const Place* p = b.find(what);
    return p ? p->label : "(none)";
}

} // namespace

void test_places() {
    t::suite("places / normalising a name");
    CHECK_STR(norm("SuperC"), "superc");
    CHECK_STR(norm("Super C"), "super c");
    CHECK_STR(norm("  McFIT  "), "mcfit");
    CHECK_STR(norm("Aachen Hauptbahnhof"), "aachen hauptbahnhof");
    // Punctuation collapses to a single space, wherever it is.
    CHECK_STR(norm("SuperC, Templergraben 57"), "superc templergraben 57");
    CHECK_STR(norm("A---B"), "a b");
    CHECK_STR(norm("!!!"), "");
    CHECK_STR(norm(""), "");

    // The pair that made this necessary: the same building typed on a phone
    // with a German keyboard and on a laptop without one.
    CHECK_STR(norm("RWTH Hauptgeb\xc3\xa4ude"), norm("RWTH Hauptgebaude"));
    CHECK_STR(norm("RWTH Hauptgebaeude"), norm("RWTH Hauptgebaude"));
    CHECK_STR(norm("Scheibenstra\xc3\x9f""e"), norm("Scheibenstrasse"));
    CHECK_STR(norm("K\xc3\xb6lnstra\xc3\x9f""e"), norm("Koelnstrasse"));
    CHECK_STR(norm("H\xc3\xb6rn"), norm("Hoern"));

    t::suite("places / finding the place you meant");
    {
        const PlaceBook b = realBook();
        CHECK_EQ(b.count(), 5);

        // Exactly as written.
        CHECK_STR(found(b, "McFIT"), "McFIT");
        CHECK_STR(found(b, "SuperC"), "SuperC");
        CHECK_STR(found(b, "Collective Incubator"), "Collective Incubator");

        // Case and spacing, because nobody types the same way twice.
        CHECK_STR(found(b, "mcfit"), "McFIT");
        CHECK_STR(found(b, "  SUPERC "), "SuperC");
        CHECK_STR(found(b, "collective incubator"), "Collective Incubator");

        // With the address Google Calendar appends when you pick from its
        // autocomplete.
        CHECK_STR(found(b, "SuperC, Templergraben 57, 52062 Aachen"), "SuperC");
        CHECK_STR(found(b, "McFIT Fitnessstudio, Aachen"), "McFIT");
        CHECK_STR(found(b, "Aachen Hauptbahnhof, Bahnhofplatz 2a"), "Aachen Hauptbahnhof");

        // Typed with the umlaut, stored without.
        CHECK_STR(found(b, "RWTH Hauptgeb\xc3\xa4ude"), "RWTH Hauptgebaude");
        CHECK_STR(found(b, "RWTH Hauptgebaeude"), "RWTH Hauptgebaude");

        // Not a match. A miss is not a failure -- the string still goes to
        // AVV's search -- but it must not be a WRONG match.
        CHECK_STR(found(b, "Ponttor"), "(none)");
        CHECK_STR(found(b, "Zahnarzt"), "(none)");
        CHECK_STR(found(b, ""), "(none)");
        CHECK_STR(found(b, nullptr), "(none)");
    }
    {
        // Whole words only. "SuperC" must not match inside a longer word, or
        // the table would quietly send you to the wrong side of town.
        PlaceBook b;
        b.add("SuperC", 50.778091, 6.078766);
        CHECK_STR(found(b, "Supercomputer Centre"), "(none)");
        CHECK_STR(found(b, "asuperc"), "(none)");
        CHECK_STR(found(b, "SuperC"), "SuperC");
        CHECK_STR(found(b, "at SuperC today"), "SuperC");
    }
    {
        // The longest match wins, so a general label cannot shadow a specific
        // one just by being added first.
        PlaceBook b;
        b.add("Aachen", 50.7753, 6.0839);
        b.add("Aachen Hauptbahnhof", 50.768105, 6.091202);
        CHECK_STR(found(b, "Aachen Hauptbahnhof"), "Aachen Hauptbahnhof");
        CHECK_STR(found(b, "Aachen"), "Aachen");
    }

    t::suite("places / config lines");
    {
        PlaceBook b;
        CHECK(b.addFromConfigLine("McFIT|50.770770|6.118744"));
        CHECK_EQ(b.count(), 1);
        CHECK_STR(b.at(0).label, "McFIT");
        CHECK_NEAR(b.at(0).lat, 50.770770, 1e-6);
        CHECK_NEAR(b.at(0).lon, 6.118744, 1e-6);

        // Spaces around the label are the config file's, not the user's.
        CHECK(b.addFromConfigLine("  SuperC  |50.778091|6.078766"));
        CHECK_STR(b.at(1).label, "SuperC");

        // An unnamed entry is skipped, not an error: it is what the template
        // ships with before you have filled it in.
        CHECK(!b.addFromConfigLine("|50.770770|6.118744"));
        CHECK_EQ(b.count(), 2);

        // Malformed lines are refused rather than half-parsed.
        CHECK(!b.addFromConfigLine("McFIT|50.77"));
        CHECK(!b.addFromConfigLine("McFIT"));
        CHECK(!b.addFromConfigLine(""));
        CHECK(!b.addFromConfigLine(nullptr));
        CHECK_EQ(b.count(), 2);
    }
    {
        // A zero coordinate is the shape of an unfilled entry, and 0,0 is in
        // the Atlantic -- accepting it would route every appointment there.
        PlaceBook b;
        CHECK(!b.add("Nowhere", 0.0, 0.0));
        CHECK(!b.addFromConfigLine("Nowhere|0|0"));
        // And a coordinate that is not on Earth.
        CHECK(!b.add("Broken", 91.0, 6.0));
        CHECK(!b.add("Broken", 50.0, 181.0));
        CHECK_EQ(b.count(), 0);
    }
    {
        // The table is bounded, and filling it is not a crash.
        PlaceBook b;
        for (int i = 0; i < kMaxPlaces + 3; ++i) {
            char label[16];
            std::snprintf(label, sizeof label, "P%d", i);
            b.add(label, 50.0 + i * 0.001, 6.0);
        }
        CHECK_EQ(b.count(), kMaxPlaces);
        CHECK_STR(found(b, "P0"), "P0");
        CHECK_STR(found(b, "P9"), "(none)");   // never made it in
    }
    {
        PlaceBook b = realBook();
        b.clear();
        CHECK_EQ(b.count(), 0);
        CHECK_STR(found(b, "SuperC"), "(none)");
    }
}
