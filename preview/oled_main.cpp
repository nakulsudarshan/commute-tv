// Renders the OLED health panel to PGM files, so the layout can be reviewed
// without a soldering iron. `make oled` writes preview/out/oled_*.pgm.
//
// The states here are the ones worth looking at: everything working, nothing
// scheduled, and the panel doing its actual job -- telling you what is broken.
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/core/textutil.h"
#include "../src/oled/health.h"

using namespace oled;

namespace {

void writePgm(const MonoCanvas& c, const char* path, int scale) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) { std::printf("  could not write %s\n", path); return; }
    std::fprintf(f, "P5\n%d %d\n255\n", kWidth * scale, kHeight * scale);
    for (int16_t y = 0; y < kHeight; ++y)
        for (int s = 0; s < scale; ++s)
            for (int16_t x = 0; x < kWidth; ++x)
                for (int t = 0; t < scale; ++t) {
                    const unsigned char v = c.get(x, y) ? 0xFF : 0x08;
                    std::fwrite(&v, 1, 1, f);
                }
    std::fclose(f);
}

void ascii(const MonoCanvas& c) {
    for (int16_t y = 0; y < kHeight; ++y) {
        std::printf("  |");
        for (int16_t x = 0; x < kWidth; ++x) std::printf(c.get(x, y) ? "#" : " ");
        std::printf("|\n");
    }
}

void set(char* dst, size_t cap, const char* s) {
    core::copyStr(dst, cap, s, std::strlen(s));
}

} // namespace

int main(int argc, char** argv) {
    const bool showAscii = (argc > 1 && std::strcmp(argv[1], "--ascii") == 0);
    std::vector<uint8_t> buf(kBufferBytes);
    MonoCanvas c(buf.data());

    std::printf("Rendering OLED health panels:\n");

    {
        HealthModel m;
        set(m.clock, sizeof m.clock, "14:32");
        m.clockSynced = true;
        m.wifiUp = true;
        set(m.wifiName, sizeof m.wifiName, "Campus");
        m.rssiDbm = -73;
        m.haveEvent = true;
        set(m.eventTime, sizeof m.eventTime, "15:30");
        set(m.eventPlace, sizeof m.eventPlace, "SuperC");
        m.stateText = "armed";
        m.minutesToLeave = 12;
        m.alerting = true;
        m.tvReachable = true;
        m.tvFreeKnown = true;
        m.tvFreeKb = 943;
        m.transitAgeMin = 2;
        m.requestsThisHour = 7;
        m.requestsPerHourCap = 60;
        renderHealth(c, m);
        writePgm(c, "preview/out/oled_1_armed.pgm", 4);
        std::printf("  oled_1_armed        holding the screen, everything working\n");
        if (showAscii) ascii(c);
    }
    {
        HealthModel m;
        set(m.clock, sizeof m.clock, "09:14");
        m.clockSynced = true;
        m.wifiUp = true;
        set(m.wifiName, sizeof m.wifiName, "Campus");
        m.rssiDbm = -73;
        m.stateText = "idle";
        m.tvReachable = true;
        m.tvFreeKnown = true;
        m.tvFreeKb = 943;
        m.transitAgeMin = -1;
        m.requestsPerHourCap = 60;
        renderHealth(c, m);
        writePgm(c, "preview/out/oled_2_idle.pgm", 4);
        std::printf("  oled_2_idle         nothing scheduled, nothing fetched yet\n");
        if (showAscii) ascii(c);
    }
    {
        HealthModel m;
        m.clockSynced = false;
        m.wifiUp = false;
        m.haveEvent = true;
        set(m.eventTime, sizeof m.eventTime, "15:30");
        set(m.eventPlace, sizeof m.eventPlace, "RWTH Hauptgebaude Templergraben");
        m.stateText = "armed";
        m.minutesToLeave = -3;
        m.tvReachable = false;
        m.transitAgeMin = 47;
        m.transitStale = true;
        m.backoffSec = 240;
        set(m.errorText, sizeof m.errorText, "tv full");
        renderHealth(c, m);
        writePgm(c, "preview/out/oled_3_broken.pgm", 4);
        std::printf("  oled_3_broken       the panel doing its actual job\n");
        if (showAscii) ascii(c);
    }
    {
        renderBoot(c, "starting");
        writePgm(c, "preview/out/oled_4_boot.pgm", 4);
        std::printf("  oled_4_boot         the first second\n");
    }

    std::printf("\n  128x64, 1bpp, %zu bytes a frame. Open with:  "
                "xdg-open preview/out/oled_1_armed.pgm\n", kBufferBytes);
    std::printf("  Add --ascii to print them in the terminal instead.\n");
    return 0;
}
