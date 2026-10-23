// ssd1306.h - talking to the 128x64 OLED.
//
// Wiring, as built: VCC->3V3 (NOT 5 V -- the module's pull-ups would drive 5 V
// into 3.3 V GPIOs), GND->GND, SDA->GPIO21, SCL->GPIO22.
//
// The I2C write is the one thing here that cannot be tested without hardware,
// so it is the only thing behind an interface. Everything else -- the
// initialisation sequence, the addressing window, the chunking -- is byte
// generation, and byte generation can be asserted exactly. That matters more
// than it sounds: a display that stays dark tells you nothing about WHICH of
// the fifteen setup commands was wrong.
#pragma once
#include <cstddef>
#include <cstdint>

#include "mono_canvas.h"

namespace oled {

struct I2cBus {
    virtual ~I2cBus() = default;
    // One transaction: START, address+W, the bytes, STOP.
    virtual bool write(uint8_t addr, const uint8_t* data, size_t len) = 0;
};

struct Config {
    // 0x3C for almost every module sold; 0x3D when the address jumper is moved.
    uint8_t address = 0x3C;

    uint8_t contrast = 0xCF;

    // Data bytes per transaction, excluding the control byte. Arduino's Wire
    // buffer is 32 bytes by default and ESP32's is larger, so 16 is chosen to
    // be correct everywhere rather than fastest somewhere. A whole frame is
    // 1,024 bytes: 64 transactions at this size, a few milliseconds on a bus
    // that is idle the rest of the minute.
    size_t chunkBytes = 16;
};

// The control byte that prefixes every transaction: commands or pixel data.
constexpr uint8_t kCmdPrefix = 0x00;
constexpr uint8_t kDataPrefix = 0x40;

// The power-on sequence for a 128x64 panel, in order. Exposed so the test can
// assert the bytes rather than the behaviour.
size_t initSequence(uint8_t contrast, uint8_t* out, size_t outCap);

class Display {
public:
    Display(const Config& cfg, I2cBus& bus) : cfg_(cfg), bus_(&bus) {}

    bool begin();
    bool flush(const MonoCanvas& c);

    bool setContrast(uint8_t value);
    bool setPower(bool on);
    bool setInverted(bool inverted);

    bool ready() const { return ready_; }

    // Transactions that the bus refused since boot. On a health panel that is
    // itself the health signal: a display that has stopped acknowledging is
    // usually a wiring fault, and it cannot tell you so itself.
    uint32_t failedWrites() const { return failedWrites_; }

private:
    bool command(const uint8_t* cmds, size_t n);
    bool command1(uint8_t c) { return command(&c, 1); }

    Config cfg_;
    I2cBus* bus_;
    bool ready_ = false;
    uint32_t failedWrites_ = 0;
};

} // namespace oled
