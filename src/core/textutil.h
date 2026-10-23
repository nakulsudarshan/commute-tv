// textutil.h - getting real-world names onto an ASCII-only font.
//
// The baked fonts cover U+0020..U+007E and nothing else (render/font_data.h),
// because a 1bpp glyph table for all of Latin-1 would cost flash for glyphs
// that almost never appear. Aachen's stop names, however, are full of them:
// "Aachen, Hoern Bruecke", "Scheibenstrasse", "Huels Gewerbegebiet". Rendering
// those unfolded silently drops the accented letters -- "Hrn Brcke" -- which
// looks like a corrupted display rather than a missing glyph.
//
// So names are folded to ASCII once, at parse time, before they are copied into
// the fixed-size fields in core::types.h. Folding there rather than at render
// time means no consumer has to remember, and the German digraphs (ae, oe, ue,
// ss) are the same length in UTF-8 as their replacement, so a folded name never
// grows.
#pragma once
#include <cstddef>

namespace core {

// Transliterate UTF-8 to printable ASCII. Latin letters lose their diacritics,
// the German sharp s becomes "ss", typographic punctuation becomes its ASCII
// equivalent, and anything else becomes '?' -- visible, rather than a hole.
//
// Always NUL-terminates (unless outSize is 0). Returns the number of bytes
// written, excluding the terminator; output is truncated to fit, never
// overrun, and truncation never splits a multi-byte replacement. `truncated`,
// when given, reports whether anything was dropped.
size_t asciiFold(const char* in, size_t inLen, char* out, size_t outSize,
                 bool* truncated = nullptr);

// Fold straight into a fixed-size field. Returns false if it had to truncate,
// which callers may ignore for display text and must not for identifiers.
bool foldInto(char* dst, size_t dstSize, const char* src, size_t srcLen);

// Bounded copy that always NUL-terminates. Returns false if it truncated.
bool copyStr(char* dst, size_t dstSize, const char* src, size_t srcLen);

} // namespace core
