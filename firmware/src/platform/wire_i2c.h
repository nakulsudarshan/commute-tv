// wire_i2c.h - oled::I2cBus over the Arduino Wire library.
//
// The whole platform-specific surface of the display driver, in thirty lines.
// Everything else about the OLED -- the init sequence, the addressing window,
// the chunking, the layout -- is tested on the laptop.
#pragma once
#include <Wire.h>

#include "oled/ssd1306.h"

namespace platform {

class WireI2c final : public oled::I2cBus {
public:
    // SDA 21, SCL 22 as wired. 400 kHz is the SSD1306's fast mode; a whole
    // 1 KB frame is then about 25 ms, once a second at most.
    bool begin(int sda = 21, int scl = 22, uint32_t hz = 400000) {
        started_ = Wire.begin(sda, scl, hz);
        return started_;
    }

    bool write(uint8_t addr, const uint8_t* data, size_t len) override {
        if (!started_) return false;
        Wire.beginTransmission(addr);
        // Wire::write returns how much it buffered; a short write means the
        // transaction would be truncated, and a truncated command stream is
        // how a display ends up in a state nobody can explain.
        if (Wire.write(data, len) != len) {
            Wire.endTransmission();
            return false;
        }
        return Wire.endTransmission() == 0;
    }

    // Is anything acknowledging at that address? Worth checking before blaming
    // the driver: a loose SDA wire and a wrong address look identical from
    // software, and this tells them apart from a broken solder joint.
    bool present(uint8_t addr) {
        if (!started_) return false;
        Wire.beginTransmission(addr);
        return Wire.endTransmission() == 0;
    }

private:
    bool started_ = false;
};

} // namespace platform
