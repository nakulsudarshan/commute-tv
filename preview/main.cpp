// Host-only layout preview. Renders every scenario the screen has to handle
// and writes them as BMPs, so the design can be reviewed and argued about
// without flashing anything.
//
//   make preview && xdg-open preview/out/01_normal.bmp
#include <cstdio>
#include <cstring>
#include "../src/core/textutil.h"
#include "../src/render/screen.h"
#include "../src/render/band_renderer.h"
#include "../src/jpeg/jpeg_encoder.h"

using namespace render;

// ⚠️ strcpy into a fixed field is how this file crashed the moment a scenario
// got a realistic name: a 48-character title into eventTitle[40], caught by
// _FORTIFY_SOURCE. The project has paid for this once before, in a test that
// put a 34-character place into eventPlace[24]. Bounded, or not at all.
template <size_t N>
static void set(char (&dst)[N], const char* src) {
    core::copyStr(dst, N, src, std::strlen(src));
}

static Color buffer[kScreenW * kScreenH];

// The ESP32 will hold one fixed upload buffer. Sizing it here, against real
// frames, is the point: if a scenario does not fit, we want to find out on the
// laptop and not at 08:15 on a Tuesday.
static constexpr size_t kUploadBudget = 32 * 1024;
static uint8_t jpegBuf[kUploadBudget];

// The band the DEVICE will use. The full canvas below exists only so the
// preview can also write a lossless BMP to compare against; the ESP32 never
// allocates one, because it cannot -- malloc(115200) fails on the real board.
static constexpr int16_t kBandRows = 16;
static Color bandBuf[kScreenW * kBandRows];

static size_t totalJpeg = 0;
static size_t worstJpeg = 0;

// Writes the BMP (lossless reference) and the JPEG the device would actually
// upload, so the two can be compared pixel for pixel afterwards.
static void emit(const Canvas& c, jpeg::BandSource& src, const char* name) {
    char path[256];
    std::snprintf(path, sizeof path, "preview/out/%s.bmp", name);
    if (!writeBmp(c, path)) { std::printf("  FAILED to write %s\n", path); return; }

    jpeg::Options opt;                       // quality 90, 4:4:4
    jpeg::BufferSink sink(jpegBuf, sizeof jpegBuf);
    // Encoded through the BANDED path, so these files are byte-for-byte what
    // the ESP32 will upload -- not an approximation of it.
    const int q = jpeg::encodeRgb565WithBudget(src, c.width(), c.height(),
                                               opt, kUploadBudget, 55, sink);
    if (q < 0) { std::printf("  %-24s BMP ok, JPEG DID NOT FIT\n", name); return; }

    std::snprintf(path, sizeof path, "preview/out/%s.jpg", name);
    std::FILE* f = std::fopen(path, "wb");
    if (!f) { std::printf("  FAILED to write %s\n", path); return; }
    std::fwrite(sink.data(), 1, sink.size(), f);
    std::fclose(f);

    totalJpeg += sink.size();
    if (sink.size() > worstJpeg) worstJpeg = sink.size();
    std::printf("  %-24s %6zu B jpeg  q%-3d\n", name, sink.size(), q);
}

static void save(const ScreenModel& m, const char* name) {
    Canvas c(buffer, kScreenW, kScreenH);
    renderCommute(c, m);
    BandRenderer src(bandBuf, kScreenW, kScreenH, kBandRows, renderCommuteBand, &m);
    emit(c, src, name);
}

// The scene that caught nothing: every preview had a POSITIVE countdown, so
// "min" was drawn where the real screen drew "min ago" and the clipping never
// showed up here. Added deliberately.
static ScreenModel overdueBase();

static ScreenModel base() {
    ScreenModel m;
    m.minutesToLeave = 12;
    std::strcpy(m.nowText, "14:38");
    std::strcpy(m.destination, "Uniklinik");
    set(m.eventTitle, "Praktikum Regelungstechnik");
    m.transfers = 0;
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

int main() {
    std::printf("Rendering layout previews:\n");

    // 1. The common case: everything fine.
    save(base(), "01_normal");

    // 2. Bus running late. Colour shifts to amber, delay is explicit.
    {
        ScreenModel m = base();
        m.delayMin = 4;
        m.minutesToLeave = 16;
        std::strcpy(m.arrTime, "15:16");
        save(m, "02_delayed");
    }

    // 3. Badly late: red, and the arrival consequence spelled out.
    {
        ScreenModel m = base();
        m.delayMin = 11;
        m.willBeLate = true;
        m.lateByMin = 7;
        std::strcpy(m.arrTime, "15:37");
        save(m, "03_will_be_late");
    }

    // 4. Cancelled service.
    {
        ScreenModel m = base();
        m.cancelled = true;
        save(m, "04_cancelled");
    }

    // 5. Leave now. The whole header goes red.
    {
        ScreenModel m = base();
        m.minutesToLeave = 0;
        std::strcpy(m.nowText, "14:45");
        save(m, "05_leave_now");
    }

    // 6. Past the leave time, inside the grace period.
    {
        ScreenModel m = base();
        m.minutesToLeave = -3;
        std::strcpy(m.nowText, "14:48");
        save(m, "06_overdue");
    }

    // 7. No connection at all: cycling has to carry the screen.
    {
        ScreenModel m = base();
        m.hasTransit = false;
        std::strcpy(m.noTransitReason, "nothing arrives before 15:30");
        m.comfortScore = 61;
        m.comfortLabel = "OK";
        m.comfortReason = "climb";
        save(m, "07_no_transit");
    }

    // 8. Rain caps the cycling score regardless of a pleasant temperature.
    {
        ScreenModel m = base();
        m.comfortScore = 31;
        m.comfortLabel = "Rough";
        m.comfortReason = "rain";
        save(m, "08_bike_rain");
    }

    // 9. Long stop and destination names -- the norm in Aachen, not an edge
    //    case. Both must clip cleanly rather than overrun the panel.
    {
        ScreenModel m = base();
        std::strcpy(m.destination, "Klinikum Aachen Haupteingang");
        set(m.eventTitle, "A meeting with a very long name indeed, truncated");
        m.transfers = 2;
        std::strcpy(m.boardStop, "Aachen Westbahnhof Nord");
        std::strcpy(m.line, "147");
        save(m, "09_long_names");
    }

    // 10. Stale data. The footer turns amber and says how old it is, so a dead
    //     service cannot masquerade as a working one.
    {
        ScreenModel m = base();
        m.stale = true;
        std::strcpy(m.statusText, "STALE - last ok 13:52");
        save(m, "10_stale");
    }

    // 11. No cycling option (too far), so the panel is simply absent.
    {
        ScreenModel m = base();
        m.hasCycle = false;
        save(m, "11_no_bike");
    }

    // 12. Idle screen.
    {
        Canvas c(buffer, kScreenW, kScreenH);
        renderIdle(c, "14:38", "next: Uniklinik 15:30");
        const IdleModel idle{"14:38", "next: Uniklinik 15:30"};
        BandRenderer src(bandBuf, kScreenW, kScreenH, kBandRows, renderIdleBand, &idle);
        emit(c, src, "12_idle");
    }

    std::printf("\n  worst frame %zu B, mean %zu B, budget %zu B\n",
                worstJpeg, totalJpeg / 12, kUploadBudget);
    std::printf("  encoded through %d-row bands: %zu B of pixel buffer, "
                "not %zu B\n", (int)kBandRows,
                BandRenderer::bufferBytes(kScreenW, kBandRows),
                BandRenderer::bufferBytes(kScreenW, kScreenH));
    std::printf("\nDone. Open them with:  xdg-open preview/out/01_normal.bmp\n");
    return 0;
}
