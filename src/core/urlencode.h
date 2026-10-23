// urlencode.h - RFC 3986 percent-encoding.
//
// Lives in core rather than in hafas/ because the SmallTV client needs exactly
// the same thing for /set?img=... . Two copies of an escaping routine is how
// one of them ends up subtly wrong.
#pragma once
#include <cstddef>

namespace core {

// Percent-encodes `in` into `out`, escaping everything outside the RFC 3986
// unreserved set. Returns the length written excluding the terminator, or -1
// if it did not fit -- never a truncated result, because a half-escaped URL is
// worse than no URL.
int urlEncode(const char* in, char* out, size_t outSize);

// As urlEncode, but leaves '/' alone.
//
// This exists for one specific reason. The SmallTV's /set?img= and
// /delete?file= parameters take a LittleFS path, and tools/probe_smalltv.py
// sends them through Python's urllib.parse.quote, whose default safe set
// includes '/'. So the probe puts "/set?img=/image/probe.jpg" on the wire.
// Whether the stock firmware decodes %2F back to a slash is unknown and will
// stay unknown, so the firmware sends exactly what the probe sends -- the
// probe's result is only transferable if the request is the same.
int urlEncodePath(const char* in, char* out, size_t outSize);

} // namespace core
