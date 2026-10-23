// Structural tests for the JPEG encoder.
//
// These check the things a decoder needs to be true and that a wrong encoder
// gets wrong: markers present and in order, dimensions correct, no unstuffed
// 0xFF inside the entropy-coded data. They deliberately do NOT check that the
// picture looks right -- no self-written test can, because it would be the
// same misunderstanding grading its own work. That job belongs to
// tools/verify_jpeg.py, which pushes the bytes through Pillow's decoder and
// compares pixels.
#include <cstring>
#include <vector>

#include "test.h"
#include "../src/jpeg/jpeg_encoder.h"

namespace {

std::vector<uint8_t> encode(const uint16_t* px, int w, int h, const jpeg::Options& o) {
    std::vector<uint8_t> buf(200 * 1024);
    jpeg::BufferSink sink(buf.data(), buf.size());
    if (!jpeg::encodeRgb565(px, w, h, o, sink)) return {};
    return std::vector<uint8_t>(sink.data(), sink.data() + sink.size());
}

// A small image with hard edges and saturated colour -- the content that
// stresses an encoder, rather than a flat field that would hide mistakes.
std::vector<uint16_t> testImage(int w, int h) {
    std::vector<uint16_t> px(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool on = ((x / 3) + (y / 3)) % 2 == 0;
            px[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] =
                on ? static_cast<uint16_t>(0xF81F) : static_cast<uint16_t>(0x0410);
        }
    return px;
}

// Finds a two-byte marker in the header region (before SOS).
long findMarker(const std::vector<uint8_t>& d, uint8_t marker) {
    for (size_t i = 0; i + 1 < d.size(); ++i) {
        if (d[i] == 0xFF && d[i + 1] == marker) return static_cast<long>(i);
        if (d[i] == 0xFF && d[i + 1] == 0xDA) break;   // scan data starts here
    }
    return -1;
}

int countMarkers(const std::vector<uint8_t>& d, uint8_t marker) {
    int n = 0;
    size_t i = 2;
    while (i + 3 < d.size()) {
        if (d[i] != 0xFF) break;
        const uint8_t m = d[i + 1];
        if (m == 0xDA || m == 0xD9) break;
        const size_t segLen = (static_cast<size_t>(d[i + 2]) << 8) | d[i + 3];
        if (m == marker) ++n;
        i += 2 + segLen;
    }
    return n;
}

size_t scanStart(const std::vector<uint8_t>& d) {
    size_t i = 2;
    while (i + 3 < d.size()) {
        if (d[i] != 0xFF) break;
        const size_t segLen = (static_cast<size_t>(d[i + 2]) << 8) | d[i + 3];
        if (d[i + 1] == 0xDA) return i + 2 + segLen;
        i += 2 + segLen;
    }
    return d.size();
}

} // namespace

void test_jpeg() {
    t::suite("jpeg / sinks");
    {
        uint8_t small[4];
        jpeg::BufferSink sink(small, sizeof small);
        const uint8_t data[3] = {1, 2, 3};
        CHECK(sink.write(data, 3));
        CHECK_EQ(sink.size(), 3);
        CHECK(!sink.overflowed());

        // Overflow is reported, and nothing is written past the end.
        CHECK(!sink.write(data, 3));
        CHECK(sink.overflowed());
        CHECK_EQ(sink.size(), 3);

        sink.reset();
        CHECK_EQ(sink.size(), 0);
        CHECK(!sink.overflowed());
    }

    t::suite("jpeg / rejects bad input");
    {
        jpeg::Options o;
        jpeg::CountingSink c;
        CHECK(!jpeg::encodeRgb565(nullptr, 8, 8, o, c));
        const std::vector<uint16_t> px = testImage(8, 8);
        CHECK(!jpeg::encodeRgb565(px.data(), 0, 8, o, c));
        CHECK(!jpeg::encodeRgb565(px.data(), 8, -1, o, c));
    }

    t::suite("jpeg / file structure");
    {
        const std::vector<uint16_t> px = testImage(240, 240);
        jpeg::Options o;
        const std::vector<uint8_t> d = encode(px.data(), 240, 240, o);
        CHECK(!d.empty());

        CHECK_EQ(d[0], 0xFF);
        CHECK_EQ(d[1], 0xD8);                              // SOI
        CHECK_EQ(d[d.size() - 2], 0xFF);
        CHECK_EQ(d[d.size() - 1], 0xD9);                   // EOI

        CHECK(findMarker(d, 0xE0) > 0);                    // APP0/JFIF
        CHECK_EQ(countMarkers(d, 0xDB), 2);                // two quant tables
        CHECK_EQ(countMarkers(d, 0xC4), 4);                // four Huffman tables
        CHECK(findMarker(d, 0xC0) > 0);                    // SOF0 => baseline
        CHECK(findMarker(d, 0xC2) < 0);                    // never progressive
        CHECK(findMarker(d, 0xDA) > 0);                    // SOS

        // SOF0 payload: precision, height, width, component count.
        const long sof = findMarker(d, 0xC0);
        const size_t p = static_cast<size_t>(sof) + 4;
        CHECK_EQ(d[p], 8);
        CHECK_EQ((d[p + 1] << 8) | d[p + 2], 240);         // height
        CHECK_EQ((d[p + 3] << 8) | d[p + 4], 240);         // width
        CHECK_EQ(d[p + 5], 3);
        CHECK_EQ(d[p + 7], 0x11);                          // Y sampling, 4:4:4
    }

    t::suite("jpeg / 4:2:0 differs only where it should");
    {
        const std::vector<uint16_t> px = testImage(240, 240);
        jpeg::Options o;
        o.sampling = jpeg::Sampling::S420;
        const std::vector<uint8_t> d = encode(px.data(), 240, 240, o);
        CHECK(!d.empty());
        const long sof = findMarker(d, 0xC0);
        CHECK(sof > 0);
        CHECK_EQ(d[static_cast<size_t>(sof) + 4 + 7], 0x22);   // Y is 2x2 sampled
        CHECK_EQ(d[static_cast<size_t>(sof) + 4 + 10], 0x11);  // Cb stays 1x1

        jpeg::Options o444;
        const std::vector<uint8_t> d444 = encode(px.data(), 240, 240, o444);
        CHECK(d.size() < d444.size());   // subsampling must actually save bytes
    }

    t::suite("jpeg / byte stuffing");
    {
        // Every 0xFF inside the entropy-coded segment must be followed by 0x00,
        // or a decoder reads it as a marker and the image ends early. This is
        // the single most common way a hand-written encoder produces a file
        // that looks fine for a while and then truncates on some inputs.
        const std::vector<uint16_t> px = testImage(240, 240);
        jpeg::Options o;
        o.quality = 98;                  // maximum coefficient magnitudes
        const std::vector<uint8_t> d = encode(px.data(), 240, 240, o);
        CHECK(!d.empty());

        const size_t start = scanStart(d);
        CHECK(start < d.size());
        int unstuffed = 0;
        for (size_t i = start; i + 1 < d.size() - 2; ++i)
            if (d[i] == 0xFF && d[i + 1] != 0x00) ++unstuffed;
        CHECK_EQ(unstuffed, 0);
    }

    t::suite("jpeg / odd dimensions");
    {
        // 13x7 is not a multiple of an MCU in either axis and not even of a
        // block. Both paths must pad internally and still declare the true
        // size, because the SmallTV scales what it is given.
        const int cases[][2] = {{13, 7}, {1, 1}, {17, 33}, {240, 1}};
        for (const auto& c : cases) {
            for (int s = 0; s < 2; ++s) {
                jpeg::Options o;
                o.sampling = s ? jpeg::Sampling::S420 : jpeg::Sampling::S444;
                const std::vector<uint16_t> px = testImage(c[0], c[1]);
                const std::vector<uint8_t> d = encode(px.data(), c[0], c[1], o);
                CHECK(!d.empty());
                const long sof = findMarker(d, 0xC0);
                CHECK(sof > 0);
                const size_t p = static_cast<size_t>(sof) + 4;
                CHECK_EQ((d[p + 1] << 8) | d[p + 2], c[1]);
                CHECK_EQ((d[p + 3] << 8) | d[p + 4], c[0]);
            }
        }
    }

    t::suite("jpeg / quality");
    {
        const std::vector<uint16_t> px = testImage(240, 240);
        jpeg::Options lo, hi;
        lo.quality = 40;
        hi.quality = 95;
        CHECK(encode(px.data(), 240, 240, lo).size() <
              encode(px.data(), 240, 240, hi).size());

        // Out-of-range quality is clamped, not undefined.
        jpeg::Options crazy;
        crazy.quality = 0;
        CHECK(!encode(px.data(), 240, 240, crazy).empty());
        crazy.quality = 1000;
        CHECK(!encode(px.data(), 240, 240, crazy).empty());
    }

    t::suite("jpeg / counting sink agrees with buffer sink");
    {
        const std::vector<uint16_t> px = testImage(64, 64);
        jpeg::Options o;
        jpeg::CountingSink c;
        CHECK(jpeg::encodeRgb565(px.data(), 64, 64, o, c));
        CHECK_EQ(c.size(), encode(px.data(), 64, 64, o).size());
    }

    t::suite("jpeg / budget");
    {
        const std::vector<uint16_t> px = testImage(240, 240);
        std::vector<uint8_t> buf(200 * 1024);
        jpeg::BufferSink sink(buf.data(), buf.size());
        jpeg::Options o;

        // A generous budget keeps the requested quality.
        int q = jpeg::encodeRgb565WithBudget(px.data(), 240, 240, o, 200 * 1024, 40, sink);
        CHECK_EQ(q, o.quality);

        // A tight budget degrades rather than failing, and respects the
        // limit. The budget is derived from what this image actually costs at
        // each end of the range rather than hard-coded, so the test asserts
        // the mechanism instead of a number that depends on the test pattern.
        jpeg::CountingSink atFloor;
        jpeg::Options floorOpt;
        floorOpt.quality = 20;
        CHECK(jpeg::encodeRgb565(px.data(), 240, 240, floorOpt, atFloor));
        jpeg::CountingSink atCeiling;
        CHECK(jpeg::encodeRgb565(px.data(), 240, 240, o, atCeiling));
        CHECK(atFloor.size() < atCeiling.size());

        const size_t tight = (atFloor.size() + atCeiling.size()) / 2;
        q = jpeg::encodeRgb565WithBudget(px.data(), 240, 240, o, tight, 20, sink);
        CHECK(q > 0);
        CHECK(q < o.quality);
        CHECK(sink.size() <= tight);
        CHECK(!sink.overflowed());

        // An impossible budget reports failure and leaves nothing behind, so a
        // caller that ignores the return value cannot upload a partial file.
        q = jpeg::encodeRgb565WithBudget(px.data(), 240, 240, o, 64, 20, sink);
        CHECK_EQ(q, -1);
        CHECK_EQ(sink.size(), 0);
    }
}
