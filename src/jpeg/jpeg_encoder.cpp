#include "jpeg_encoder.h"

#include <cmath>
#include <cstring>

namespace jpeg {
namespace {

// ---------------------------------------------------------------------------
// Tables from ITU-T T.81 Annex K. These are the tables essentially every
// encoder ships; a decoder that cannot read them cannot read JPEG at all,
// which is exactly the property we want when the decoder is stock firmware we
// cannot inspect.
// ---------------------------------------------------------------------------

// Natural (row-major) order. Converted to zig-zag only when written to DQT.
const uint8_t kQuantLum[64] = {
    16, 11, 10, 16,  24,  40,  51,  61,
    12, 12, 14, 19,  26,  58,  60,  55,
    14, 13, 16, 24,  40,  57,  69,  56,
    14, 17, 22, 29,  51,  87,  80,  62,
    18, 22, 37, 56,  68, 109, 103,  77,
    24, 35, 55, 64,  81, 104, 113,  92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103,  99,
};

const uint8_t kQuantChr[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
};

// kZigZag[k] = natural index of the k-th coefficient in zig-zag order.
const uint8_t kZigZag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

const uint8_t kDcLumBits[17] = {0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
const uint8_t kDcLumVals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

const uint8_t kDcChrBits[17] = {0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
const uint8_t kDcChrVals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

const uint8_t kAcLumBits[17] = {0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
const uint8_t kAcLumVals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
    0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
    0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

const uint8_t kAcChrBits[17] = {0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
const uint8_t kAcChrVals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,
    0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
    0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74,
    0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

// ---------------------------------------------------------------------------
// Derived tables. Built once; ~3 KB of .bss on the ESP32, which is cheaper
// than rebuilding them on every frame and far cheaper than a code search per
// coefficient.
// ---------------------------------------------------------------------------

struct HuffTable {
    uint16_t code[256];
    uint8_t  size[256];   // 0 = symbol not in this table
};

HuffTable g_dcLum, g_dcChr, g_acLum, g_acChr;
float     g_dct[8][8];   // g_dct[u][x], with the 1-D normalisation folded in
bool      g_ready = false;

void buildHuff(const uint8_t* bits, const uint8_t* vals, HuffTable& out) {
    std::memset(&out, 0, sizeof out);
    uint16_t code = 0;
    int k = 0;
    for (int len = 1; len <= 16; ++len) {
        for (int i = 0; i < bits[len]; ++i) {
            const uint8_t sym = vals[k++];
            out.code[sym] = code++;
            out.size[sym] = static_cast<uint8_t>(len);
        }
        code = static_cast<uint16_t>(code << 1);
    }
}

void ensureTables() {
    if (g_ready) return;
    buildHuff(kDcLumBits, kDcLumVals, g_dcLum);
    buildHuff(kDcChrBits, kDcChrVals, g_dcChr);
    buildHuff(kAcLumBits, kAcLumVals, g_acLum);
    buildHuff(kAcChrBits, kAcChrVals, g_acChr);

    const float pi = 3.14159265358979323846f;
    for (int u = 0; u < 8; ++u) {
        const float cu = (u == 0) ? 0.70710678118654752f : 1.0f;
        for (int x = 0; x < 8; ++x)
            g_dct[u][x] = 0.5f * cu *
                          std::cos((2.0f * static_cast<float>(x) + 1.0f) *
                                   static_cast<float>(u) * pi / 16.0f);
    }
    g_ready = true;
}

// IJG quality scaling, so `quality` means what it means in every other tool.
void scaleQuant(const uint8_t* base, int quality, uint16_t* out) {
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    const int scale = (quality < 50) ? (5000 / quality) : (200 - quality * 2);
    for (int i = 0; i < 64; ++i) {
        int v = (base[i] * scale + 50) / 100;
        if (v < 1) v = 1;
        if (v > 255) v = 255;   // 8-bit DQT precision
        out[i] = static_cast<uint16_t>(v);
    }
}

// ---------------------------------------------------------------------------
// Bit writer. Buffers into a small block so the sink sees whole chunks rather
// than a call per byte -- which matters when the sink is a socket.
// ---------------------------------------------------------------------------

class BitWriter {
public:
    explicit BitWriter(Sink& s) : sink_(s) {}

    bool ok() const { return ok_; }

    void putByte(uint8_t b) {
        if (n_ == sizeof buf_) flushBuf();
        buf_[n_++] = b;
    }

    void putWord(uint16_t w) {
        putByte(static_cast<uint8_t>(w >> 8));
        putByte(static_cast<uint8_t>(w & 0xFF));
    }

    void putBytes(const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) putByte(p[i]);
    }

    // Entropy-coded bits, MSB first, with the mandatory 0x00 stuffing after
    // any 0xFF so the decoder never mistakes data for a marker.
    void putBits(uint16_t value, int length) {
        for (int i = length - 1; i >= 0; --i) {
            acc_ = static_cast<uint8_t>((acc_ << 1) | ((value >> i) & 1u));
            if (++accBits_ == 8) {
                putByte(acc_);
                if (acc_ == 0xFF) putByte(0x00);
                acc_ = 0;
                accBits_ = 0;
            }
        }
    }

    // Pads the final partial byte with 1-bits, as the standard requires.
    void flushBits() {
        while (accBits_ != 0) putBits(1, 1);
    }

    bool finish() {
        flushBuf();
        return ok_;
    }

private:
    void flushBuf() {
        if (n_ != 0) {
            if (!sink_.write(buf_, n_)) ok_ = false;
            n_ = 0;
        }
    }

    Sink& sink_;
    uint8_t buf_[256];
    size_t n_ = 0;
    uint8_t acc_ = 0;
    int accBits_ = 0;
    bool ok_ = true;
};

// ---------------------------------------------------------------------------
// Colour. RGB565 -> RGB888 by bit replication (so 0x1F maps to 255, not 248),
// then JFIF full-range YCbCr.
// ---------------------------------------------------------------------------

struct Rgb { float r, g, b; };

inline Rgb unpack565(uint16_t p) {
    const uint8_t r5 = static_cast<uint8_t>((p >> 11) & 0x1F);
    const uint8_t g6 = static_cast<uint8_t>((p >> 5) & 0x3F);
    const uint8_t b5 = static_cast<uint8_t>(p & 0x1F);
    return Rgb{
        static_cast<float>((r5 << 3) | (r5 >> 2)),
        static_cast<float>((g6 << 2) | (g6 >> 4)),
        static_cast<float>((b5 << 3) | (b5 >> 2)),
    };
}

inline float lumaOf(const Rgb& c)  { return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b; }
inline float cbOf(const Rgb& c)    { return -0.168736f * c.r - 0.331264f * c.g + 0.5f * c.b + 128.0f; }
inline float crOf(const Rgb& c)    { return 0.5f * c.r - 0.418688f * c.g - 0.081312f * c.b + 128.0f; }

// Hands the whole-frame overload to the banded encoder without copying: every
// requested strip is just a pointer into the caller's buffer.
class WholeImageSource final : public BandSource {
public:
    WholeImageSource(const uint16_t* px, int width) : px_(px), width_(width) {}
    const uint16_t* band(int y, int) override {
        return px_ + static_cast<size_t>(y) * static_cast<size_t>(width_);
    }

private:
    const uint16_t* px_;
    int width_;
};

// ---------------------------------------------------------------------------
// Forward DCT. Separable, float, from a precomputed cosine basis.
//
// Not the fastest formulation -- a scaled integer DCT would be ~4x quicker --
// but it is the one whose correctness is obvious by inspection, and at one
// frame per 60 s on a 240 MHz core with an FPU there is nothing to buy with
// the speed. Revisit only if profiling ever says so.
// ---------------------------------------------------------------------------

void fdct8x8(const float* in, float* out) {
    float tmp[64];
    for (int y = 0; y < 8; ++y) {          // rows
        for (int u = 0; u < 8; ++u) {
            float s = 0.0f;
            for (int x = 0; x < 8; ++x) s += in[y * 8 + x] * g_dct[u][x];
            tmp[y * 8 + u] = s;
        }
    }
    for (int u = 0; u < 8; ++u) {          // columns
        for (int v = 0; v < 8; ++v) {
            float s = 0.0f;
            for (int y = 0; y < 8; ++y) s += tmp[y * 8 + u] * g_dct[v][y];
            out[v * 8 + u] = s;
        }
    }
}

inline int magnitudeBits(int v) {
    if (v < 0) v = -v;
    int n = 0;
    while (v != 0) { ++n; v >>= 1; }
    return n;
}

// Quantises, zig-zag scans and entropy-codes one block. Returns the DC value
// so the caller can carry it into the next block's difference.
int encodeBlock(BitWriter& bw, const float* samples, const uint16_t* quant,
                const HuffTable& dc, const HuffTable& ac, int prevDc) {
    float freq[64];
    fdct8x8(samples, freq);

    int coef[64];
    for (int i = 0; i < 64; ++i) {
        const float q = freq[i] / static_cast<float>(quant[i]);
        coef[i] = static_cast<int>(q < 0.0f ? q - 0.5f : q + 0.5f);
    }

    // --- DC: coded as a difference from the previous block of the same
    //     component, which is where most of the compression on a flat UI
    //     background comes from.
    const int diff = coef[0] - prevDc;
    const int dcCat = magnitudeBits(diff);
    bw.putBits(dc.code[dcCat], dc.size[dcCat]);
    if (dcCat != 0) {
        const int v = (diff < 0) ? diff - 1 : diff;
        bw.putBits(static_cast<uint16_t>(v & ((1 << dcCat) - 1)), dcCat);
    }

    // --- AC: run-length of zeros, then magnitude category.
    int run = 0;
    for (int k = 1; k < 64; ++k) {
        const int v = coef[kZigZag[k]];
        if (v == 0) { ++run; continue; }
        while (run > 15) {                       // ZRL: sixteen zeros
            bw.putBits(ac.code[0xF0], ac.size[0xF0]);
            run -= 16;
        }
        const int cat = magnitudeBits(v);
        const uint8_t sym = static_cast<uint8_t>((run << 4) | cat);
        bw.putBits(ac.code[sym], ac.size[sym]);
        const int enc = (v < 0) ? v - 1 : v;
        bw.putBits(static_cast<uint16_t>(enc & ((1 << cat) - 1)), cat);
        run = 0;
    }
    if (run > 0) bw.putBits(ac.code[0x00], ac.size[0x00]);  // EOB

    return coef[0];
}

void writeMarkerSegment(BitWriter& bw, uint16_t marker, const uint8_t* payload, size_t n) {
    bw.putWord(marker);
    bw.putWord(static_cast<uint16_t>(n + 2));
    bw.putBytes(payload, n);
}

void writeQuantTable(BitWriter& bw, const uint16_t* natural, uint8_t id) {
    uint8_t seg[65];
    seg[0] = id;                                   // 8-bit precision, table id
    for (int k = 0; k < 64; ++k)
        seg[1 + k] = static_cast<uint8_t>(natural[kZigZag[k]]);   // DQT is zig-zag
    writeMarkerSegment(bw, 0xFFDB, seg, sizeof seg);
}

void writeHuffTable(BitWriter& bw, uint8_t classAndId,
                    const uint8_t* bits, const uint8_t* vals) {
    size_t nvals = 0;
    for (int i = 1; i <= 16; ++i) nvals += bits[i];

    uint8_t seg[1 + 16 + 162];
    seg[0] = classAndId;
    std::memcpy(seg + 1, bits + 1, 16);
    std::memcpy(seg + 17, vals, nvals);
    writeMarkerSegment(bw, 0xFFC4, seg, 17 + nvals);
}

} // namespace

// ---------------------------------------------------------------------------

bool BufferSink::write(const uint8_t* data, size_t len) {
    if (len_ + len > cap_) { overflow_ = true; return false; }
    std::memcpy(buf_ + len_, data, len);
    len_ += len;
    return true;
}

bool encodeRgb565(const uint16_t* px, int width, int height,
                  const Options& opt, Sink& sink) {
    if (px == nullptr || width <= 0 || height <= 0) return false;
    WholeImageSource src(px, width);
    return encodeRgb565(src, width, height, opt, sink);
}

bool encodeRgb565(BandSource& src, int width, int height,
                  const Options& opt, Sink& sink) {
    if (width <= 0 || height <= 0) return false;
    ensureTables();

    const bool sub = (opt.sampling == Sampling::S420);
    const int mcuW = sub ? 16 : 8;
    const int mcuH = sub ? 16 : 8;
    const int mcusX = (width + mcuW - 1) / mcuW;
    const int mcusY = (height + mcuH - 1) / mcuH;

    uint16_t qLum[64], qChr[64];
    scaleQuant(kQuantLum, opt.quality, qLum);
    scaleQuant(kQuantChr, opt.quality, qChr);

    BitWriter bw(sink);

    bw.putWord(0xFFD8);                                    // SOI

    const uint8_t jfif[14] = {
        'J', 'F', 'I', 'F', 0,   // identifier
        1, 1,                    // version 1.1
        0,                       // no density units
        0, 1, 0, 1,              // X/Y density 1:1
        0, 0,                    // no thumbnail
    };
    writeMarkerSegment(bw, 0xFFE0, jfif, sizeof jfif);     // APP0

    writeQuantTable(bw, qLum, 0);
    writeQuantTable(bw, qChr, 1);

    const uint8_t sof[15] = {
        8,                                              // 8-bit samples
        static_cast<uint8_t>(height >> 8), static_cast<uint8_t>(height & 0xFF),
        static_cast<uint8_t>(width >> 8),  static_cast<uint8_t>(width & 0xFF),
        3,                                              // three components
        1, static_cast<uint8_t>(sub ? 0x22 : 0x11), 0,  // Y
        2, 0x11, 1,                                     // Cb
        3, 0x11, 1,                                     // Cr
    };
    writeMarkerSegment(bw, 0xFFC0, sof, sizeof sof);       // SOF0, baseline

    writeHuffTable(bw, 0x00, kDcLumBits, kDcLumVals);
    writeHuffTable(bw, 0x10, kAcLumBits, kAcLumVals);
    writeHuffTable(bw, 0x01, kDcChrBits, kDcChrVals);
    writeHuffTable(bw, 0x11, kAcChrBits, kAcChrVals);

    const uint8_t sos[10] = {
        3,              // components in this scan
        1, 0x00,        // Y  -> DC table 0, AC table 0
        2, 0x11,        // Cb -> DC table 1, AC table 1
        3, 0x11,        // Cr -> DC table 1, AC table 1
        0, 63, 0,       // Ss, Se, Ah/Al -- baseline sequential
    };
    writeMarkerSegment(bw, 0xFFDA, sos, sizeof sos);       // SOS

    int dcY = 0, dcCb = 0, dcCr = 0;
    float blk[64];

    for (int my = 0; my < mcusY; ++my) {
        const int oy = my * mcuH;
        // Ask only for rows that exist. The last band of an image whose height
        // is not a multiple of the MCU size is short, and the clamping below
        // replicates its final row -- which is exactly what the whole-frame
        // path did by clamping y to height-1.
        const int avail = (height - oy) < mcuH ? (height - oy) : mcuH;
        const uint16_t* rows = src.band(oy, avail);
        if (rows == nullptr) return false;

        // Reads by ABSOLUTE image coordinates out of the current strip, with
        // the same edge replication the whole-frame path used: padding with
        // black would put a hard edge into the final block row and cost bits.
        auto at = [&](int ax, int ay) -> Rgb {
            if (ax < 0) ax = 0; else if (ax >= width) ax = width - 1;
            int ly = ay - oy;
            if (ly < 0) ly = 0; else if (ly >= avail) ly = avail - 1;
            return unpack565(rows[static_cast<size_t>(ly) * static_cast<size_t>(width)
                                  + static_cast<size_t>(ax)]);
        };

        for (int mx = 0; mx < mcusX; ++mx) {
            const int ox = mx * mcuW;

            // --- luminance: one block at 4:4:4, four at 4:2:0
            const int yBlocks = sub ? 4 : 1;
            for (int b = 0; b < yBlocks; ++b) {
                const int bx = ox + (sub ? (b & 1) * 8 : 0);
                const int by = oy + (sub ? (b >> 1) * 8 : 0);
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x)
                        blk[y * 8 + x] = lumaOf(at(bx + x, by + y)) - 128.0f;
                dcY = encodeBlock(bw, blk, qLum, g_dcLum, g_acLum, dcY);
            }

            // --- chrominance. At 4:2:0 each sample is the mean of a 2x2 group,
            //     averaged in RGB before conversion: averaging Cb/Cr after
            //     conversion biases saturated edges, which is precisely where
            //     the coloured text lives.
            for (int comp = 0; comp < 2; ++comp) {
                for (int y = 0; y < 8; ++y) {
                    for (int x = 0; x < 8; ++x) {
                        Rgb c;
                        if (sub) {
                            const int sx = ox + x * 2, sy = oy + y * 2;
                            const Rgb a = at(sx,     sy);
                            const Rgb b2 = at(sx + 1, sy);
                            const Rgb c2 = at(sx,     sy + 1);
                            const Rgb d = at(sx + 1, sy + 1);
                            c = Rgb{(a.r + b2.r + c2.r + d.r) * 0.25f,
                                    (a.g + b2.g + c2.g + d.g) * 0.25f,
                                    (a.b + b2.b + c2.b + d.b) * 0.25f};
                        } else {
                            c = at(ox + x, oy + y);
                        }
                        blk[y * 8 + x] = (comp == 0 ? cbOf(c) : crOf(c)) - 128.0f;
                    }
                }
                if (comp == 0) dcCb = encodeBlock(bw, blk, qChr, g_dcChr, g_acChr, dcCb);
                else           dcCr = encodeBlock(bw, blk, qChr, g_dcChr, g_acChr, dcCr);
            }

            if (!bw.ok()) return false;
        }
    }

    bw.flushBits();
    bw.putWord(0xFFD9);                                    // EOI
    return bw.finish();
}

int encodeRgb565WithBudget(const uint16_t* px, int width, int height,
                           const Options& opt, size_t maxBytes,
                           int minQuality, BufferSink& sink) {
    if (px == nullptr) return -1;
    WholeImageSource src(px, width);
    return encodeRgb565WithBudget(src, width, height, opt, maxBytes, minQuality, sink);
}

int encodeRgb565WithBudget(BandSource& src, int width, int height,
                           const Options& opt, size_t maxBytes,
                           int minQuality, BufferSink& sink) {
    // Ten points a step: small enough that we never give away more quality
    // than we must, large enough that the worst case is a handful of encodes.
    for (int q = opt.quality; q >= minQuality; q -= 10) {
        Options o = opt;
        o.quality = q;
        sink.reset();
        if (encodeRgb565(src, width, height, o, sink) && sink.size() <= maxBytes)
            return q;
    }
    if (minQuality < opt.quality) {
        Options o = opt;
        o.quality = minQuality;
        sink.reset();
        if (encodeRgb565(src, width, height, o, sink) && sink.size() <= maxBytes)
            return minQuality;
    }
    sink.reset();
    return -1;
}

} // namespace jpeg
