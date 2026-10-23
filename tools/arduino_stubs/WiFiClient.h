// Stub. See README.md -- syntax only, no behaviour.
// The real <WiFi.h> pulls this in, so this stub does too: a stub that is
// NARROWER than the core it imitates makes the firmware look wrong when it is
// not, which is worse than no stub at all.
#pragma once
#include <Arduino.h>

class WiFiClient {
public:
    virtual ~WiFiClient() = default;
    int connect(const char*, uint16_t) { return 1; }
    void setTimeout(uint32_t) {}
    int available() { return 0; }
    int read() { return -1; }
    int read(uint8_t*, size_t) { return 0; }
    size_t write(const uint8_t*, size_t n) { return n; }
    bool connected() { return false; }
    void stop() {}
    __attribute__((format(printf, 2, 3)))
    size_t printf(const char*, ...) { return 0; }
};
