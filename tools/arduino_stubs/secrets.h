// Stub. See README.md -- syntax only, no behaviour. NOT credentials, and not
// what the firmware builds against: PlatformIO only sees firmware/include.
#pragma once
#define WIFI_NETWORK_LIST                              \
    { "stub-enterprise", "", "", "stub-user", "", -95 }, \
    { "stub-psk", "stub-pass", "", "", "", -85 },
#define SMALLTV_HOST       "0.0.0.0"
