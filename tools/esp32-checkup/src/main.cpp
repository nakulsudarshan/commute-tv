// ESP32 checkup: MAC address, memory, and a 2.4 GHz survey.
//
// Flash this first. It confirms the board is alive, prints the MAC you need to
// register, and shows whether your phone hotspot is actually visible and on
// the right band -- which is the usual reason "it won't connect" later.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>   // heap_caps_get_largest_free_block; do not rely
                             // on this arriving transitively via Arduino.h
#include <esp_system.h>

static void rule(const char* title) {
    Serial.println();
    Serial.println("========================================");
    Serial.print("  "); Serial.println(title);
    Serial.println("========================================");
}

static void reportMemory(const char* title) {
    rule(title);
    Serial.printf("  Free heap         : %u bytes\n", (unsigned)ESP.getFreeHeap());
    Serial.printf("  Largest free block: %u bytes\n",
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    Serial.printf("  Minimum ever free : %u bytes\n", (unsigned)ESP.getMinFreeHeap());
    Serial.println("  The design needs ~115 KB framebuffer + 45 KB TLS + 35 KB JPEG.");
    Serial.println("  Note the framebuffer must be CONTIGUOUS -- compare it against");
    Serial.println("  the largest free block, not against total free heap.");
}

static const char* encName(wifi_auth_mode_t e) {
    switch (e) {
        case WIFI_AUTH_OPEN:            return "OPEN  <- unencrypted";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise  <- needs login, unsupported";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
        default:                        return "unknown";
    }
}

void setup() {
    Serial.begin(115200);
    delay(1500);

    rule("BOARD");
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    Serial.printf("  Chip model    : ESP32 (rev %d)\n", chip.revision);
    Serial.printf("  CPU cores     : %d\n", chip.cores);
    Serial.printf("  Flash size    : %u MB\n", (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
    Serial.printf("  PSRAM         : %s\n", ESP.getPsramSize() ? "yes" : "no (fine, not needed)");

    reportMemory("MEMORY, before WiFi starts");

    rule("MAC ADDRESS  (register the STATION one)");
    WiFi.mode(WIFI_STA);
    delay(100);
    Serial.print("  >>> STATION MAC : ");
    Serial.println(WiFi.macAddress());
    Serial.print("      softAP MAC  : ");
    Serial.println(WiFi.softAPmacAddress());
    Serial.println();
    Serial.println("  Give dorm IT the STATION MAC. It is burned into eFuse and");
    Serial.println("  never rotates, so it survives reflashing and power cycles.");

    rule("2.4 GHz SURVEY");
    Serial.println("  Scanning...");
    const int n = WiFi.scanNetworks();
    if (n <= 0) {
        Serial.println("  No networks found. If your hotspot is on, it is probably");
        Serial.println("  5 GHz only -- the SmallTV cannot see 5 GHz at all.");
    } else {
        Serial.printf("\n  %-32s %5s %4s  %s\n", "SSID", "RSSI", "CH", "SECURITY");
        Serial.println("  -------------------------------------------------------------------");
        for (int i = 0; i < n; ++i) {
            String ssid = WiFi.SSID(i);
            if (ssid.length() == 0) ssid = "(hidden)";
            const int rssi = WiFi.RSSI(i);
            const char* quality = rssi > -60 ? "strong" : (rssi > -75 ? "ok" : "WEAK");
            Serial.printf("  %-32s %5d %4d  %-16s %s\n",
                          ssid.c_str(), rssi, WiFi.channel(i),
                          // encryptionType, not encryptionMode -- the latter
                          // does not exist in the Arduino ESP32 core.
                          encName(WiFi.encryptionType(i)), quality);
        }
        Serial.println("\n  Only 2.4 GHz networks appear here -- that is the point.");
        Serial.println("  If your phone hotspot is missing, switch it to 2.4 GHz:");
        Serial.println("    iPhone : Personal Hotspot -> Maximum Compatibility ON");
        Serial.println("    Android: Hotspot -> AP Band -> 2.4 GHz");
        Serial.println("  Aim for RSSI better than -70. Weaker than that and the");
        Serial.println("  device will connect but drop out intermittently.");
    }

    // The number that actually decides the architecture. WiFi has been started
    // and a scan run by this point, so this is much closer to steady state than
    // a reading taken at boot.
    reportMemory("MEMORY, after WiFi is up");

    rule("ALLOCATION TEST  (the real question)");
    Serial.println("  Free heap is not the constraint -- CONTIGUOUS space is.");
    Serial.println("  The ESP32's DRAM is split into several regions, so a large");
    Serial.println("  malloc can fail while plenty of heap remains.\n");

    // Try the framebuffer the design assumes, then walk down to find the real
    // ceiling. Every block is freed immediately; this measures, it does not keep.
    const size_t sizes[] = {115200, 110000, 100000, 90000, 80000, 64000, 32000};
    const char* labels[] = {"240x240 RGB565 framebuffer", "", "", "", "", "", ""};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        void* p = malloc(sizes[i]);
        Serial.printf("  malloc(%6u) : %-7s %s\n", (unsigned)sizes[i],
                      p ? "OK" : "FAILED", labels[i]);
        if (p) free(p);
    }

    Serial.println("\n  If the first line says FAILED, a single full-screen buffer");
    Serial.println("  cannot be heap-allocated on this board and the renderer must");
    Serial.println("  either use a static buffer or draw in horizontal bands.");

    rule("DONE");
    Serial.println("  Copy this whole output and send it over.");
}

void loop() {
    delay(10000);
}
