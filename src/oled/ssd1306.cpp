#include "ssd1306.h"

#include <cstring>

namespace oled {
namespace {

// Command bytes, named. The datasheet calls these by number and every driver on
// the internet copies the numbers without saying what they do, which is how a
// dark panel becomes an afternoon.
constexpr uint8_t kDisplayOff        = 0xAE;
constexpr uint8_t kDisplayOn         = 0xAF;
constexpr uint8_t kSetClockDiv       = 0xD5;
constexpr uint8_t kSetMultiplex      = 0xA8;
constexpr uint8_t kSetDisplayOffset  = 0xD3;
constexpr uint8_t kSetStartLine0     = 0x40;
constexpr uint8_t kChargePump        = 0x8D;
constexpr uint8_t kMemoryMode        = 0x20;
constexpr uint8_t kSegRemap127       = 0xA1;
constexpr uint8_t kComScanDec        = 0xC8;
constexpr uint8_t kSetComPins        = 0xDA;
constexpr uint8_t kSetContrast       = 0x81;
constexpr uint8_t kSetPrecharge      = 0xD9;
constexpr uint8_t kSetVcomDetect     = 0xDB;
constexpr uint8_t kResumeFromRam     = 0xA4;
constexpr uint8_t kNormalDisplay     = 0xA6;
constexpr uint8_t kInvertDisplay     = 0xA7;
constexpr uint8_t kDeactivateScroll  = 0x2E;
constexpr uint8_t kSetColumnAddr     = 0x21;
constexpr uint8_t kSetPageAddr       = 0x22;

} // namespace

size_t initSequence(uint8_t contrast, uint8_t* out, size_t outCap) {
    const uint8_t seq[] = {
        kDisplayOff,
        kSetClockDiv,      0x80,   // default ratio, oscillator at its usual speed
        kSetMultiplex,     0x3F,   // 64 rows, so 63
        kSetDisplayOffset, 0x00,
        kSetStartLine0,
        kChargePump,       0x14,   // ON -- these modules have no external Vcc,
                                   // and 0x10 here is why a panel stays black
        kMemoryMode,       0x00,   // horizontal: the whole frame is one run
        kSegRemap127,              // together with kComScanDec, the right way up
        kComScanDec,
        kSetComPins,       0x12,   // alternate pins, 128x64. 0x02 is the 128x32 value
        kSetContrast,      contrast,
        kSetPrecharge,     0xF1,
        kSetVcomDetect,    0x40,
        kResumeFromRam,            // show RAM, not all-pixels-on
        kNormalDisplay,
        kDeactivateScroll,
        kDisplayOn,
    };
    if (out == nullptr || outCap < sizeof seq) return 0;
    std::memcpy(out, seq, sizeof seq);
    return sizeof seq;
}

bool Display::command(const uint8_t* cmds, size_t n) {
    // Control byte then the command bytes, in one transaction.
    uint8_t buf[40];
    if (n + 1 > sizeof buf) return false;
    buf[0] = kCmdPrefix;
    std::memcpy(buf + 1, cmds, n);
    if (!bus_->write(cfg_.address, buf, n + 1)) {
        ++failedWrites_;
        return false;
    }
    return true;
}

bool Display::begin() {
    uint8_t seq[40];
    const size_t n = initSequence(cfg_.contrast, seq, sizeof seq);
    if (n == 0) return false;
    ready_ = command(seq, n);
    return ready_;
}

bool Display::flush(const MonoCanvas& c) {
    // Address the whole panel explicitly every time. Relying on the pointer
    // being where the last flush left it works right up until one transaction
    // is lost, after which every frame is shifted and nothing says why.
    const uint8_t window[] = {
        kSetColumnAddr, 0x00, static_cast<uint8_t>(kWidth - 1),
        kSetPageAddr,   0x00, static_cast<uint8_t>(kHeight / 8 - 1),
    };
    if (!command(window, sizeof window)) return false;

    const uint8_t* px = c.bits();
    size_t chunk = cfg_.chunkBytes;
    if (chunk == 0 || chunk > 64) chunk = 16;

    uint8_t buf[65];
    for (size_t sent = 0; sent < kBufferBytes; ) {
        const size_t n = (kBufferBytes - sent < chunk) ? (kBufferBytes - sent) : chunk;
        buf[0] = kDataPrefix;
        std::memcpy(buf + 1, px + sent, n);
        if (!bus_->write(cfg_.address, buf, n + 1)) {
            ++failedWrites_;
            return false;
        }
        sent += n;
    }
    return true;
}

bool Display::setContrast(uint8_t value) {
    cfg_.contrast = value;
    const uint8_t cmd[] = {kSetContrast, value};
    return command(cmd, sizeof cmd);
}

bool Display::setPower(bool on) {
    return command1(on ? kDisplayOn : kDisplayOff);
}

bool Display::setInverted(bool inverted) {
    return command1(inverted ? kInvertDisplay : kNormalDisplay);
}

} // namespace oled
