// Stub. See README.md -- syntax only, no behaviour.
#pragma once
#include <Arduino.h>
#include <WiFiClient.h>
#include <esp_wifi.h>
#include <functional>

typedef enum {
    WL_NO_SHIELD = 255,
    WL_IDLE_STATUS = 0,
    WL_NO_SSID_AVAIL = 1,
    WL_SCAN_COMPLETED = 2,
    WL_CONNECTED = 3,
    WL_CONNECT_FAILED = 4,
    WL_CONNECTION_LOST = 5,
    WL_DISCONNECTED = 6
} wl_status_t;

typedef enum { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 } wifi_mode_t;

typedef enum { WIFI_FAST_SCAN = 0, WIFI_ALL_CHANNEL_SCAN = 1 } wifi_scan_method_t;
// Quarter-dBm units, as the real header defines them.
typedef enum { WIFI_POWER_MINUS_1dBm = -4, WIFI_POWER_19_5dBm = 78 } wifi_power_t;
typedef enum { WIFI_CONNECT_AP_BY_SIGNAL = 0, WIFI_CONNECT_AP_BY_SECURITY = 1 }
    wifi_sort_method_t;

typedef enum {
    ARDUINO_EVENT_WIFI_STA_CONNECTED = 4,
    ARDUINO_EVENT_WIFI_STA_DISCONNECTED = 5,
    ARDUINO_EVENT_WIFI_STA_GOT_IP = 7,
    ARDUINO_EVENT_MAX = 99
} arduino_event_id_t;

typedef struct {
    struct { uint8_t ssid[32]; uint8_t ssid_len; uint8_t bssid[6]; uint8_t reason; }
        wifi_sta_disconnected;
} arduino_event_info_t;

typedef arduino_event_id_t WiFiEvent_t;
typedef arduino_event_info_t WiFiEventInfo_t;
typedef std::function<void(arduino_event_id_t, arduino_event_info_t)> WiFiEventFuncCb;

class IPAddress {
public:
    String toString() const { return String("0.0.0.0"); }
};

class WiFiStub {
public:
    void persistent(bool) {}
    bool mode(wifi_mode_t) { return true; }
    void setAutoReconnect(bool) {}
    bool setSleep(bool) { return true; }
    wl_status_t status() { return WL_DISCONNECTED; }
    bool begin(const char*) { return true; }
    bool begin(const char*, const char*) { return true; }
    bool begin(const char*, const char*, int32_t, const uint8_t* = nullptr,
               bool = true) { return true; }
    uint8_t* BSSID(uint8_t) { static uint8_t b[6] = {0}; return b; }
    bool disconnect(bool = false, bool = false) { return true; }
    String macAddress() { return String("00:00:00:00:00:00"); }
    IPAddress localIP() { return IPAddress(); }
    int32_t RSSI() { return 0; }
    int32_t channel() { return 0; }
    String BSSIDstr() { return String(""); }
    void setScanMethod(wifi_scan_method_t) {}
    bool setTxPower(wifi_power_t) { return true; }
    wifi_power_t getTxPower() { return WIFI_POWER_19_5dBm; }
    void setSortMethod(wifi_sort_method_t) {}
    int onEvent(WiFiEventFuncCb, arduino_event_id_t = ARDUINO_EVENT_MAX) { return 0; }
    int16_t scanNetworks(bool = false, bool = false, bool = false, uint32_t = 300) { return 0; }
    void scanDelete() {}
    bool softAP(const char*, const char*, int = 1, int = 0, int = 4) { return true; }
    IPAddress softAPIP() { return IPAddress(); }
    int softAPgetStationNum() { return 0; }
    String SSID(uint8_t) { return String(""); }
    int32_t RSSI(uint8_t) { return 0; }
    int32_t channel(uint8_t) { return 0; }
    int encryptionType(uint8_t) { return 0; }
    String BSSIDstr(uint8_t) { return String(""); }
};
extern WiFiStub WiFi;
