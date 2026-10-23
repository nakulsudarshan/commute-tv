// Band rendering must be indistinguishable from whole-frame rendering.
//
// This is the test the decision rests on. The ESP32 cannot allocate a 240x240
// framebuffer -- measured, malloc(115200) fails -- so the frame is produced in
// strips. That is only acceptable if the resulting JPEG is EXACTLY the file the
// whole-frame path would have produced. Not similar. Identical.
//
// Everything else here is secondary: if these byte comparisons pass, banding is
// a memory optimisation with no visual consequence at all, and the previews
// reviewed on the laptop remain the bytes the device sends.
#include <cstring>
#include <vector>

#include "test.h"
#include "../src/jpeg/jpeg_encoder.h"
#include "../src/render/band_renderer.h"
#include "../src/render/screen.h"

using namespace render;

namespace {

std::vector<uint8_t> encodeWhole(const ScreenModel& m, const jpeg::Options& o) {
    std::vector<Color> frame(static_cast<size_t>(kScreenW) * kScreenH);
    Canvas c(frame.data(), kScreenW, kScreenH);
    renderCommute(c, m);

    std::vector<uint8_t> buf(200 * 1024);
    jpeg::BufferSink sink(buf.data(), buf.size());
    if (!jpeg::encodeRgb565(frame.data(), kScreenW, kScreenH, o, sink)) return {};
    return std::vector<uint8_t>(sink.data(), sink.data() + sink.size());
}

std::vector<uint8_t> encodeBanded(const ScreenModel& m, const jpeg::Options& o,
                                  int16_t bandRows) {
    std::vector<Color> band(static_cast<size_t>(kScreenW) * static_cast<size_t>(bandRows));
    BandRenderer src(band.data(), kScreenW, kScreenH, bandRows, renderCommuteBand, &m);

    std::vector<uint8_t> buf(200 * 1024);
    jpeg::BufferSink sink(buf.data(), buf.size());
    if (!jpeg::encodeRgb565(src, kScreenW, kScreenH, o, sink)) return {};
    return std::vector<uint8_t>(sink.data(), sink.data() + sink.size());
}

ScreenModel busy() {
    ScreenModel m;
    m.minutesToLeave = 12;
    std::strcpy(m.nowText, "14:38");
    std::strcpy(m.destination, "Klinikum Aachen Haupteingang");
    std::strcpy(m.eventTime, "15:30");
    m.hasTransit = true;
    std::strcpy(m.line, "147");
    std::strcpy(m.boardStop, "Aachen Westbahnhof Nord");
    std::strcpy(m.depTime, "14:50");
    std::strcpy(m.arrTime, "15:12");
    m.delayMin = 4;
    m.hasCycle = true;
    m.cycleMin = 22;
    m.ascentM = 85;
    m.comfortScore = 31;
    m.comfortLabel = "Rough";
    m.comfortReason = "rain";
    m.stale = true;
    std::strcpy(m.statusText, "STALE - last ok 13:52");
    return m;
}

ScreenModel plain() {
    ScreenModel m;
    m.minutesToLeave = 0;
    std::strcpy(m.nowText, "14:45");
    std::strcpy(m.destination, "Uniklinik");
    std::strcpy(m.eventTime, "15:30");
    m.hasTransit = true;
    std::strcpy(m.line, "3B");
    std::strcpy(m.boardStop, "Ponttor");
    std::strcpy(m.depTime, "14:50");
    std::strcpy(m.arrTime, "15:12");
    m.hasCycle = true;
    m.cycleMin = 22;
    m.ascentM = 85;
    m.comfortScore = 88;
    m.comfortLabel = "Ideal";
    m.comfortReason = "";
    std::strcpy(m.statusText, "updated 14:38");
    return m;
}

} // namespace

void test_band_render() {
    t::suite("bands / canvas reports the full screen");
    {
        // The property that makes every layout routine work unchanged: a band
        // Canvas still says it is 240x240, so text centring and right-alignment
        // land in the same place regardless of which strip is being drawn.
        std::vector<Color> buf(static_cast<size_t>(kScreenW) * 16);
        Canvas band(buf.data(), kScreenW, kScreenH, 96, 16);
        CHECK_EQ(band.width(), kScreenW);
        CHECK_EQ(band.height(), kScreenH);     // the SCREEN, not the buffer
        CHECK_EQ(band.bandY(), 96);
        CHECK_EQ(band.bandRows(), 16);
        CHECK(!band.isFullFrame());
        CHECK_EQ(band.sizeBytes(), static_cast<size_t>(kScreenW) * 16 * 2);

        Canvas full(buf.data(), 8, 8);
        CHECK(full.isFullFrame());
        CHECK_EQ(full.bandY(), 0);
        CHECK_EQ(full.bandRows(), 8);
    }

    t::suite("bands / stitched pixels equal a whole frame");
    {
        // Before trusting the JPEG comparison, confirm the pixels themselves
        // match: render once into a frame, then again band by band, and compare.
        const ScreenModel m = busy();

        std::vector<Color> frame(static_cast<size_t>(kScreenW) * kScreenH);
        Canvas c(frame.data(), kScreenW, kScreenH);
        renderCommute(c, m);

        const int16_t rows = 16;
        std::vector<Color> band(static_cast<size_t>(kScreenW) * static_cast<size_t>(rows));
        int mismatches = 0;
        for (int16_t y = 0; y < kScreenH; y = static_cast<int16_t>(y + rows)) {
            Canvas b(band.data(), kScreenW, kScreenH, y, rows);
            renderCommute(b, m);
            for (int16_t r = 0; r < rows; ++r) {
                for (int16_t x = 0; x < kScreenW; ++x) {
                    const Color got = band[static_cast<size_t>(r) * kScreenW + x];
                    const Color want = frame[static_cast<size_t>(y + r) * kScreenW + x];
                    if (got != want) ++mismatches;
                }
            }
        }
        CHECK_EQ(mismatches, 0);
    }

    t::suite("bands / JPEG bytes are identical, 4:4:4");
    {
        const ScreenModel models[] = {plain(), busy()};
        const int16_t heights[] = {8, 16, 24, 48, 80};
        for (const auto& m : models) {
            jpeg::Options o;                       // quality 90, 4:4:4
            const std::vector<uint8_t> whole = encodeWhole(m, o);
            CHECK(!whole.empty());
            for (const int16_t h : heights) {
                const std::vector<uint8_t> banded = encodeBanded(m, o, h);
                CHECK_EQ(banded.size(), whole.size());
                CHECK(!banded.empty() && banded == whole);
            }
        }
    }

    t::suite("bands / JPEG bytes are identical, 4:2:0");
    {
        // 4:2:0 uses 16-row MCUs, so the band must be at least 16 -- and the
        // chroma averaging reads 2x2 groups, which is where an off-by-one in
        // the band arithmetic would show up first.
        const ScreenModel m = busy();
        jpeg::Options o;
        o.sampling = jpeg::Sampling::S420;
        const std::vector<uint8_t> whole = encodeWhole(m, o);
        CHECK(!whole.empty());
        for (const int16_t h : {int16_t(16), int16_t(32), int16_t(48)}) {
            const std::vector<uint8_t> banded = encodeBanded(m, o, h);
            CHECK(!banded.empty() && banded == whole);
        }
    }

    t::suite("bands / a band too small for the MCU is refused");
    {
        // 4:2:0 needs 16 rows. Handing it 8 must fail cleanly rather than
        // encode a frame whose lower half is stale buffer contents.
        const ScreenModel m = plain();
        std::vector<Color> band(static_cast<size_t>(kScreenW) * 8);
        BandRenderer src(band.data(), kScreenW, kScreenH, 8, renderCommuteBand, &m);

        std::vector<uint8_t> buf(200 * 1024);
        jpeg::BufferSink sink(buf.data(), buf.size());
        jpeg::Options o;
        o.sampling = jpeg::Sampling::S420;
        CHECK(!jpeg::encodeRgb565(src, kScreenW, kScreenH, o, sink));

        // 4:4:4 needs only 8, so the same buffer is fine there.
        jpeg::Options ok444;
        BandRenderer src2(band.data(), kScreenW, kScreenH, 8, renderCommuteBand, &m);
        sink.reset();
        CHECK(jpeg::encodeRgb565(src2, kScreenW, kScreenH, ok444, sink));
    }

    t::suite("bands / out-of-range requests are rejected");
    {
        const ScreenModel m = plain();
        std::vector<Color> band(static_cast<size_t>(kScreenW) * 16);
        BandRenderer src(band.data(), kScreenW, kScreenH, 16, renderCommuteBand, &m);

        CHECK(src.band(0, 16) != nullptr);
        CHECK(src.band(224, 16) != nullptr);        // the last full band
        CHECK(src.band(240, 16) == nullptr);        // past the bottom
        CHECK(src.band(232, 16) == nullptr);        // would run off the end
        CHECK(src.band(-8, 16) == nullptr);
        CHECK(src.band(0, 0) == nullptr);
        CHECK(src.band(0, 17) == nullptr);          // bigger than the buffer

        BandRenderer nullBuf(nullptr, kScreenW, kScreenH, 16, renderCommuteBand, &m);
        CHECK(nullBuf.band(0, 16) == nullptr);
    }

    t::suite("bands / the idle screen too");
    {
        const IdleModel idle{"14:38", "next: Uniklinik 15:30"};

        std::vector<Color> frame(static_cast<size_t>(kScreenW) * kScreenH);
        Canvas c(frame.data(), kScreenW, kScreenH);
        renderIdle(c, idle.nowText, idle.note);
        std::vector<uint8_t> wbuf(200 * 1024);
        jpeg::BufferSink wsink(wbuf.data(), wbuf.size());
        jpeg::Options o;
        CHECK(jpeg::encodeRgb565(frame.data(), kScreenW, kScreenH, o, wsink));
        const std::vector<uint8_t> whole(wsink.data(), wsink.data() + wsink.size());

        std::vector<Color> band(static_cast<size_t>(kScreenW) * 16);
        BandRenderer src(band.data(), kScreenW, kScreenH, 16, renderIdleBand, &idle);
        std::vector<uint8_t> bbuf(200 * 1024);
        jpeg::BufferSink bsink(bbuf.data(), bbuf.size());
        CHECK(jpeg::encodeRgb565(src, kScreenW, kScreenH, o, bsink));
        const std::vector<uint8_t> banded(bsink.data(), bsink.data() + bsink.size());

        CHECK(banded == whole);
        CHECK_EQ(src.bandsRendered(), 30);          // 240 / 8-row MCUs
    }

    t::suite("bands / the memory claim");
    {
        // The numbers that justify all of the above, asserted so they cannot
        // drift silently: the board's largest contiguous block is 110,580 B.
        const size_t frame = BandRenderer::bufferBytes(kScreenW, kScreenH);
        const size_t band16 = BandRenderer::bufferBytes(kScreenW, 16);
        CHECK_EQ(frame, 115200u);
        CHECK_EQ(band16, 7680u);
        CHECK(frame > 110580u);        // does not fit -- measured on hardware
        CHECK(band16 < 110580u / 10);  // fits with room to spare
    }

    t::suite("bands / budget encoding works banded too");
    {
        const ScreenModel m = busy();
        std::vector<Color> band(static_cast<size_t>(kScreenW) * 16);
        BandRenderer src(band.data(), kScreenW, kScreenH, 16, renderCommuteBand, &m);

        std::vector<uint8_t> buf(200 * 1024);
        jpeg::BufferSink sink(buf.data(), buf.size());
        jpeg::Options o;
        const int q = jpeg::encodeRgb565WithBudget(src, kScreenW, kScreenH, o,
                                                   32 * 1024, 55, sink);
        CHECK(q > 0);
        CHECK(sink.size() <= 32u * 1024u);
        // Same result as the whole-frame budget path.
        std::vector<Color> frame(static_cast<size_t>(kScreenW) * kScreenH);
        Canvas c(frame.data(), kScreenW, kScreenH);
        renderCommute(c, m);
        std::vector<uint8_t> buf2(200 * 1024);
        jpeg::BufferSink sink2(buf2.data(), buf2.size());
        const int q2 = jpeg::encodeRgb565WithBudget(frame.data(), kScreenW, kScreenH,
                                                    o, 32 * 1024, 55, sink2);
        CHECK_EQ(q, q2);
        CHECK_EQ(sink.size(), sink2.size());
        CHECK_EQ(std::memcmp(sink.data(), sink2.data(), sink.size()), 0);
    }
}
