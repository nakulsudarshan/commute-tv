// Stub. See README.md -- syntax only, no behaviour.
// Arduino-ESP32 2.x names. The 3.x header (esp_eap_client.h) is deliberately
// absent so the __has_include() in wifi_link.h picks the same branch the
// installed core does.
#pragma once
#include <esp_err.h>

esp_err_t esp_wifi_sta_wpa2_ent_set_identity(const unsigned char*, int);
esp_err_t esp_wifi_sta_wpa2_ent_set_username(const unsigned char*, int);
esp_err_t esp_wifi_sta_wpa2_ent_set_password(const unsigned char*, int);
esp_err_t esp_wifi_sta_wpa2_ent_enable(void);
esp_err_t esp_wifi_sta_wpa2_ent_disable(void);
