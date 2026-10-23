// Stub. See README.md -- syntax only, no behaviour.
#pragma once
#include <Arduino.h>

class TwoWireStub {
public:
    bool begin(int, int, uint32_t = 0) { return true; }
    void beginTransmission(uint8_t) {}
    size_t write(const uint8_t*, size_t n) { return n; }
    uint8_t endTransmission() { return 0; }
};
extern TwoWireStub Wire;
