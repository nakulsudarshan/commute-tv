#include "test.h"
#include "../src/core/textutil.h"

#include <cstring>

namespace {
const char* fold(const char* in, size_t outSize = 64) {
    static char buf[128];
    core::asciiFold(in, std::strlen(in), buf, outSize > sizeof buf ? sizeof buf : outSize);
    return buf;
}
} // namespace

void test_textutil() {
    t::suite("textutil / German transit names");
    // The names this exists for, straight out of the recorded AVV responses.
    CHECK_STR(fold("Aachen, H\xc3\xb6rn Br\xc3\xbc""cke"), "Aachen, Hoern Bruecke");
    CHECK_STR(fold("Aachen, Scheibenstra\xc3\x9f""e"), "Aachen, Scheibenstrasse");
    CHECK_STR(fold("H\xc3\xbcls Gewerbegebiet"), "Huels Gewerbegebiet");
    CHECK_STR(fold("Aachener Stra\xc3\x9f""enbahn"), "Aachener Strassenbahn");
    CHECK_STR(fold("Fu\xc3\x9f""weg"), "Fussweg");

    t::suite("textutil / folding rules");
    CHECK_STR(fold("plain ASCII 123"), "plain ASCII 123");
    CHECK_STR(fold(""), "");
    // Umlauts expand to digraphs; other accents simply lose the mark.
    CHECK_STR(fold("\xc3\x84\xc3\x96\xc3\x9c\xc3\xa4\xc3\xb6\xc3\xbc"), "AeOeUeaeoeue");
    CHECK_STR(fold("caf\xc3\xa9 na\xc3\xafve"), "cafe naive");
    CHECK_STR(fold("Ge\xc3\x9f""ler"), "Gessler");
    // Typographic punctuation becomes its ASCII equivalent rather than '?'.
    CHECK_STR(fold("A \xe2\x80\x93 B"), "A - B");
    CHECK_STR(fold("\xe2\x80\x9c""quoted\xe2\x80\x9d"), "\"quoted\"");
    CHECK_STR(fold("a\xc2\xa0""b"), "a b");
    // Anything with no sensible ASCII form becomes '?', which is visible.
    // Silently dropping it would look like a rendering bug instead.
    CHECK_STR(fold("\xe4\xb8\xad\xe6\x96\x87"), "??");
    // Control characters would corrupt the layout; they become spaces.
    CHECK_STR(fold("a\tb\nc"), "a b c");

    t::suite("textutil / bounds");
    {
        // Truncation never splits a multi-byte replacement: "ue" is two bytes
        // and either both land or neither does.
        char buf[8];
        bool trunc = false;
        const char* src = "H\xc3\xbc""lsxxxx";          // folds to "Huelsxxxx"
        const size_t n = core::asciiFold(src, std::strlen(src), buf, sizeof buf, &trunc);
        CHECK_STR(buf, "Huelsxx");
        CHECK_EQ(n, 7);
        CHECK(trunc);
    }
    {
        char buf[4];
        bool trunc = false;
        core::asciiFold("H\xc3\xbc""l", 4, buf, sizeof buf, &trunc);
        CHECK_STR(buf, "Hue");     // "l" would not fit
        CHECK(trunc);
    }
    {
        char buf[16];
        bool trunc = true;
        core::asciiFold("short", 5, buf, sizeof buf, &trunc);
        CHECK_STR(buf, "short");
        CHECK(!trunc);
    }
    {   // Malformed UTF-8 must not run off the end or resynchronise mid-character.
        char buf[16];
        CHECK_EQ(core::asciiFold("a\xc3", 2, buf, sizeof buf), 2);
        CHECK_STR(buf, "a?");
        CHECK(core::asciiFold("\xff\xfe", 2, buf, sizeof buf) > 0);
        CHECK_STR(buf, "??");
        CHECK_EQ(core::asciiFold("a\xe4\xb8", 3, buf, sizeof buf), 2);
    }
    {   // A zero-size or null destination is a no-op, not a crash.
        char buf[4] = {'x', 'y', 'z', 0};
        CHECK_EQ(core::asciiFold("abc", 3, buf, 0), 0u);
        CHECK_STR(buf, "xyz");
        CHECK_EQ(core::asciiFold(nullptr, 3, buf, sizeof buf), 0u);
        CHECK_STR(buf, "");
    }

    t::suite("textutil / foldInto and copyStr");
    {
        char field[12];
        CHECK(core::foldInto(field, sizeof field, "Ponttor", 7));
        CHECK_STR(field, "Ponttor");
        CHECK(!core::foldInto(field, sizeof field, "Aachen, Hauptbahnhof", 20));
        CHECK_STR(field, "Aachen, Hau");
    }
    {
        char field[6];
        CHECK(core::copyStr(field, sizeof field, "abc", 3));
        CHECK_STR(field, "abc");
        CHECK(!core::copyStr(field, sizeof field, "abcdefgh", 8));
        CHECK_STR(field, "abcde");
        CHECK(core::copyStr(field, sizeof field, nullptr, 0));
        CHECK_STR(field, "");
    }
}
