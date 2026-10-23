#include "test.h"
#include "../src/core/address.h"

#include <cstring>

using namespace core;

namespace {

const char* simp(const char* in, char* buf, size_t cap) {
    simplifyAddress(in, std::strlen(in), buf, cap);
    return buf;
}
const char* label(const char* in, char* buf, size_t cap) {
    addressLabel(in, std::strlen(in), buf, cap);
    return buf;
}
const char* ask(const char* in, char* buf, size_t cap) {
    transitQuery(in, std::strlen(in), buf, cap);
    return buf;
}
bool street(const char* s) { return looksLikeStreet(s, std::strlen(s)); }

} // namespace

void test_address() {
    char b[96];

    t::suite("address / what Google Calendar actually puts in `location`");
    {
        // The real one, from Nakul's calendar. Already ASCII-folded by the
        // calendar client, which is why there is no eszett here.
        CHECK_STR(simp("Mathieusstrasse 30, 52072 Aachen, Germany", b, sizeof b),
                  "Mathieusstrasse 30, Aachen");
        CHECK_STR(label("Mathieusstrasse 30, 52072 Aachen, Germany", b, sizeof b),
                  "Mathieusstrasse 30");
    }
    {
        // A place that has a name as well as an address.
        CHECK_STR(simp("McFIT, Juelicher Str. 1, 52070 Aachen, Germany", b, sizeof b),
                  "McFIT, Juelicher Str. 1, Aachen");
        CHECK_STR(label("McFIT, Juelicher Str. 1, 52070 Aachen, Germany", b, sizeof b),
                  "McFIT");
    }
    {
        // Someone typing a name rather than picking a map pin.
        CHECK_STR(simp("SuperC", b, sizeof b), "SuperC");
        CHECK_STR(label("SuperC", b, sizeof b), "SuperC");
    }
    {
        // Just a city.
        CHECK_STR(simp("52062 Aachen, Germany", b, sizeof b), "Aachen");
        CHECK_STR(label("52062 Aachen, Germany", b, sizeof b), "Aachen");
    }

    t::suite("address / a street line, and what is merely a name");
    {
        CHECK(street("Marienbongard 28"));
        CHECK(street("Mathieusstrasse 30"));
        CHECK(street("Juelicher Str. 1"));
        CHECK(street("Am Kupferofen 12"));
        CHECK(street("Bahnhofplatz 2a"));
        CHECK(street("Somewhere 12-14"));
        CHECK(street("Marienbongard 28 a"));    // the letter written apart
    }
    {
        // Names, not places you can stand at a number of.
        CHECK(!street("Netto-Marken Discount"));
        CHECK(!street("McFIT"));
        CHECK(!street("Aachen"));
        CHECK(!street("SuperC"));
        CHECK(!street(""));
        CHECK(!street("   "));
        // A house number needs a street in front of it.
        CHECK(!street("28"));
        CHECK(!street("28 a"));
        // A postcode is five digits and is not a house number, whether or not
        // the trimmer got to it first.
        CHECK(!street("52062 Aachen"));
        // Nor is a year, or anything else that long.
        CHECK(!street("Room 100000"));
    }

    t::suite("address / what to ASK AVV about a Google Maps pin");
    {
        // ⚠️ THE ONE THAT BROKE IT. AVV has never heard of the shop; the street
        // line under it is an ordinary address it resolves every time.
        CHECK_STR(ask("Netto-Marken Discount, Marienbongard 28, 52062 Aachen, Germany",
                      b, sizeof b),
                  "Marienbongard 28, Aachen");
        // And the shop is still what he is shown, because that is what he
        // recognises and "Marienbongard 28" is not.
        CHECK_STR(label("Netto-Marken Discount, Marienbongard 28, 52062 Aachen, Germany",
                        b, sizeof b),
                  "Netto-Marken Discount");
    }
    {
        CHECK_STR(ask("McFIT, Juelicher Str. 1, 52070 Aachen, Germany", b, sizeof b),
                  "Juelicher Str. 1, Aachen");
    }
    {
        // Already an address: nothing to drop, and nothing dropped.
        CHECK_STR(ask("Mathieusstrasse 30, 52072 Aachen, Germany", b, sizeof b),
                  "Mathieusstrasse 30, Aachen");
    }
    {
        // No street anywhere in it -- a stop name, or a POI. The whole string is
        // the best question available, which is what simplifyAddress() gives.
        CHECK_STR(ask("Aachen Bushof", b, sizeof b), "Aachen Bushof");
        CHECK_STR(ask("Elisenbrunnen, Friedrich-Wilhelm-Platz, Aachen", b, sizeof b),
                  "Elisenbrunnen, Friedrich-Wilhelm-Platz, Aachen");
        CHECK_STR(ask("52062 Aachen, Germany", b, sizeof b), "Aachen");
    }
    {
        // Same guards as its sibling: nothing usable in, nothing out.
        CHECK_EQ(transitQuery("", 0, b, sizeof b), 0u);
        CHECK_STR(b, "");
        CHECK_EQ(transitQuery(nullptr, 0, b, sizeof b), 0u);
        CHECK_EQ(transitQuery("Germany", 7, b, sizeof b), 0u);
        CHECK_EQ(transitQuery("anything", 8, b, 0), 0u);

        char one[1];
        CHECK_EQ(transitQuery("Marienbongard 28, Aachen", 24, one, sizeof one), 0u);
        CHECK_STR(one, "");

        // A short buffer truncates rather than overruns, and terminates.
        char small[14];
        std::memset(small, 0x7F, sizeof small);
        transitQuery("Netto-Marken Discount, Marienbongard 28, 52062 Aachen", 52,
                     small, sizeof small);
        CHECK(std::strlen(small) < sizeof small);
        CHECK_STR(small, "Marienbongard");
    }

    t::suite("address / the bits that make a transit search worse");
    {
        CHECK(isDroppableCountry("Germany", 7));
        CHECK(isDroppableCountry("  deutschland  ", 15));
        CHECK(isDroppableCountry("DE", 2));
        CHECK(isDroppableCountry("", 0));
        CHECK(!isDroppableCountry("Germania", 8));
        // A stop really called this must not vanish.
        CHECK(!isDroppableCountry("Deutschlandring", 15));
    }
    {
        // Five digits and a space is a postcode.
        CHECK_EQ(postcodePrefixLen("52072 Aachen", 12), 6u);
        // A house number is not, however much it looks like the start of one.
        CHECK_EQ(postcodePrefixLen("30 Rockefeller Plaza", 20), 0u);
        CHECK_EQ(postcodePrefixLen("1234 Somewhere", 14), 0u);
        CHECK_EQ(postcodePrefixLen("123456 Somewhere", 16), 0u);
        // No separator: not a postcode, and "52072Aachen" is nothing we should
        // silently turn into "achen".
        CHECK_EQ(postcodePrefixLen("52072Aachen", 11), 0u);
        // Nothing after it -- keep the digits rather than emit an empty part.
        CHECK_EQ(postcodePrefixLen("52072", 5), 0u);
        CHECK_EQ(postcodePrefixLen("52072 ", 6), 0u);
    }

    t::suite("address / it is a trimmer, not a parser");
    {
        // Four parts collapse to three: venue, street, city -- the country goes,
        // and anything past that is not helping a stop search.
        CHECK_STR(simp("A, B, C, D, 52072 Aachen, Germany", b, sizeof b), "A, B, C");
    }
    {
        CHECK_STR(simp("  spaced out  ,   52072   Aachen  , Germany ", b, sizeof b),
                  "spaced out, Aachen");
    }
    {
        // Empty parts from doubled commas are dropped, not emitted as blanks.
        CHECK_STR(simp("Street 1,, 52072 Aachen", b, sizeof b), "Street 1, Aachen");
    }
    {
        CHECK_EQ(simplifyAddress("", 0, b, sizeof b), 0u);
        CHECK_STR(b, "");
        CHECK_EQ(simplifyAddress(nullptr, 0, b, sizeof b), 0u);
        CHECK_EQ(simplifyAddress("Germany", 7, b, sizeof b), 0u);
        CHECK_EQ(addressLabel("Germany", 7, b, sizeof b), 0u);
    }

    t::suite("address / bounds");
    {
        // A short buffer truncates rather than overruns, and always terminates.
        char small[12];
        std::memset(small, 0x7F, sizeof small);
        simplifyAddress("Mathieusstrasse 30, 52072 Aachen, Germany", 41, small,
                        sizeof small);
        CHECK(std::strlen(small) < sizeof small);
        CHECK_STR(small, "Mathieusstr");

        char tiny[4];
        addressLabel("Mathieusstrasse 30, Aachen", 26, tiny, sizeof tiny);
        CHECK_STR(tiny, "Mat");

        // A one-byte buffer can hold nothing but the terminator.
        char one[1];
        CHECK_EQ(simplifyAddress("anything", 8, one, sizeof one), 0u);
        CHECK_STR(one, "");
        CHECK_EQ(simplifyAddress("anything", 8, b, 0), 0u);
    }
    {
        // A part that will not fit is dropped whole rather than half-written --
        // half a street name is a search that finds the wrong street.
        char mid[24];
        simplifyAddress("Street One, Street Two, Aachen", 30, mid, sizeof mid);
        CHECK(std::strlen(mid) < sizeof mid);
        CHECK_STR(mid, "Street One, Street Two");
    }
}
