// address.h - turning a Google Maps location into something AVV can find.
//
// Nakul's calendar entries come from the Google Calendar app, which fills the
// `location` field from Google Maps. So most of the time it is not a name he
// chose, it is a formatted postal address:
//
//     "Mathieusstrasse 30, 52072 Aachen, Germany"
//     "McFIT, Juelicher Str. 1, 52070 Aachen, Germany"
//
// (Already ASCII-folded by the calendar client -- see core/textutil.h. German
// transit search accepts the ss/ae/oe/ue transliterations because that is what
// people type, and PlaceBook normalises the same way for the same reason.)
//
// HAFAS `location.name` is a free-text search over stops, addresses and POIs. It
// does well with "street, city". It does less well with a trailing country, and
// a postcode is noise. So three small functions, all pure and all tested:
//
//   simplifyAddress()  the whole thing, tidied  -> "McFIT, Juelicher Str. 1, Aachen"
//   transitQuery()     what to ASK AVV          -> "Juelicher Str. 1, Aachen"
//   addressLabel()     what to SHOW             -> "McFIT"
//
// ⚠️ WHY transitQuery() IS NOT simplifyAddress(). When you pick a place from
// Google Maps rather than typing an address, Google puts the BUSINESS NAME in
// front of it:
//
//     "Netto-Marken Discount, Marienbongard 28, 52062 Aachen"
//
// AVV has never heard of that shop. Handed the whole string, its free-text
// search either finds nothing or -- worse -- fuzzily matches the shop name to
// some unrelated stop and sends the journey there. The street line beneath it
// is a perfectly ordinary address that AVV resolves every time. So the query
// starts at the first part that looks like a street, and the parts before it,
// which are a name and not a place, are left out.
//
// The name is not thrown away: addressLabel() still shows it, because
// "Netto-Marken Discount" is what tells Nakul where he is going and
// "Marienbongard 28" is not.
//
// KNOWN LIMIT, stated rather than hidden: the FIRST street-looking part wins.
// A venue whose own name ends in a number -- "Cafe 42, Pontstrasse 42, Aachen"
// -- reads as a street and nothing gets dropped. Taking the LAST one instead
// would fix that case and break "Templergraben 55, Raum 003, Aachen", which is
// what a human types for a lecture room. First is the rule that never throws
// away a real street line, so first is the rule.
//
// None of these is a parser. There is no attempt to understand an address; the
// point is only to drop the parts that make a search worse, and to keep the
// order the search expects.
#pragma once
#include <cstddef>

namespace core {

// Up to this many comma-separated parts survive. Three is enough for
// "venue, street, city" and short enough that a long Google string cannot push
// the useful part out of the buffer.
constexpr int kMaxAddressParts = 3;

// Writes a NUL-terminated result and returns its length. Returns 0 (and writes
// an empty string) if there was nothing usable.
size_t simplifyAddress(const char* in, size_t inLen, char* out, size_t outCap);

// The same, minus any leading parts that come before a street line. Identical
// to simplifyAddress() when there is no street line to find -- a bare stop name
// ("Aachen Bushof") is left exactly as it is.
size_t transitQuery(const char* in, size_t inLen, char* out, size_t outCap);

// The first meaningful part -- the venue name, or the street. What goes on the
// 240x240 screen, where there is room for a line and not an address.
size_t addressLabel(const char* in, size_t inLen, char* out, size_t outCap);

// Exposed for the tests: is this a country name we drop, or a postcode?
bool isDroppableCountry(const char* s, size_t len);
size_t postcodePrefixLen(const char* s, size_t len);

// Does this one part read like a German street line -- a word, then a house
// number? "Marienbongard 28" and "Juelicher Str. 1" do; "Netto-Marken Discount"
// and "Aachen" do not.
bool looksLikeStreet(const char* s, size_t len);

} // namespace core
