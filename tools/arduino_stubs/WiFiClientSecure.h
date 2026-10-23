// Stub. See README.md -- syntax only, no behaviour.
#pragma once
#include <WiFiClient.h>

class WiFiClientSecure : public WiFiClient {
public:
    void setCACert(const char*) {}
    void setInsecure() {}
    void setHandshakeTimeout(unsigned long) {}
};
