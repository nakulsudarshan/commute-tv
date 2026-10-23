// wifi_link.h - getting on the network, and staying on it.
//
// Three things make this less boring than it should be.
//
// 1. THERE IS A LIST OF NETWORKS, IN NAKUL'S ORDER. Campus-WLAN is
//    WPA2-Enterprise and needs 802.1X with a university account; the Phone
//    hotspot is an ordinary passphrase; the flat will be a third. All
//    three have to work, the order is his to set, and adding one must be a line
//    in secrets.h rather than a change here.
//
//    Priority is honoured, but not blindly: a network is only PREFERRED while
//    it is audible at or above its own minRssi. The dorm AP at -90 dBm should
//    not beat a hotspot at -43 just because it is first in the list, and a
//    strict ordering would do exactly that.
//
// 2. THE ENTERPRISE API WAS RENAMED. Arduino-ESP32 2.x has esp_wpa2.h and
//    esp_wifi_sta_wpa2_ent_*; 3.x has esp_eap_client.h and esp_eap_client_*.
//    Which one is present depends on the core version installed, and the
//    laptop cannot compile ESP32 code to find out -- so both are handled and
//    the preprocessor picks.
//
// 3. A FAILED JOIN LOOKS THE SAME FROM THE OUTSIDE WHATEVER CAUSED IT. Out of
//    range, wrong band, wrong password, RADIUS said no -- all of them are
//    "twenty-five seconds of dots and then nothing". The radio knows which one
//    it was; it reports a numbered reason on the disconnect event. This file
//    keeps that number and says it out loud, because guessing between "move
//    the board" and "fix the credentials" wastes an evening either way.
//
// ⚠️ NEVER call esp_wifi_set_mac(). The MAC is eFuse-burned and is what dorm
// IT registered; changing it breaks the network registration, not just this
// project.
//
// Signal at the desk measured -73 dBm on a phone, which works but is marginal
// -- and a phone may well have been reading the 5 GHz radio, which this chip
// cannot hear at all. scanReport() settles that question with the ESP32's own
// antenna before the first join is attempted.
#pragma once
#include <WiFi.h>
#include <cstring>

// esp_wifi.h first: on some core versions the EAP headers assume it.
#include <esp_wifi.h>

#if __has_include(<esp_eap_client.h>)
  #include <esp_eap_client.h>
  #define COMMUTE_TV_EAP_V3 1
#elif __has_include(<esp_wpa2.h>)
  #include <esp_wpa2.h>
  #define COMMUTE_TV_EAP_V3 0
#else
  #define COMMUTE_TV_EAP_NONE 1
#endif

namespace platform {

// The reason codes are ESP-IDF's, and they are the whole point of this file,
// so they are spelled out as numbers rather than enum names: the names have
// moved between IDF versions, the numbers have not.
//
// The ones that matter for telling apart the two stories:
//   201 the radio never heard the AP at all      -> range, or 5 GHz-only
//   202 / 23 / 15 / 204  the AP heard us and said no -> credentials, RADIUS
inline const char* wifiReasonText(uint8_t reason) {
    switch (reason) {
        case 0:   return "";
        case 1:   return "unspecified";
        case 2:   return "auth expired";
        case 3:   return "auth leave";
        case 4:   return "assoc expired";
        case 5:   return "AP full";
        case 6:   return "not authed";
        case 7:   return "not assoced";
        case 8:   return "AP disassociated us";
        case 9:   return "assoc not authed";
        case 13:  return "invalid IE";
        case 14:  return "MIC failure";
        case 15:  return "4-way handshake timeout";   // PSK: wrong password
        case 16:  return "group key timeout";
        case 17:  return "IE differs in 4-way";
        case 18:  return "group cipher invalid";
        case 19:  return "pairwise cipher invalid";
        case 20:  return "AKMP invalid";
        case 21:  return "unsupported RSN version";
        case 22:  return "invalid RSN capabilities";
        case 23:  return "802.1X auth failed";        // RADIUS said no
        case 24:  return "cipher suite rejected";
        case 200: return "beacon timeout";            // heard it, then lost it
        case 201: return "no AP found";               // never heard it
        case 202: return "auth failed";
        case 203: return "assoc failed";
        case 204: return "handshake timeout";
        case 205: return "connection failed";
        case 206: return "AP TSF reset";
        case 207: return "roaming";
        default:  return "unknown";
    }
}

// ⚠️ THREE BUCKETS, NOT TWO -- and the first version of this file got it wrong.
//
// It classified 204 (handshake timeout) as a credential rejection and printed
// "Credentials, not signal." Then the board produced a 204 at -75 dBm using the
// SAME credentials that had joined successfully minutes earlier at -75 and -83.
// So that message was not just unhelpful, it was false, and it was the kind of
// false that sends someone to check a password that was never wrong.
//
// The physics: an 802.1X handshake is many round trips. On a lossy link it can
// start and fail to finish without anyone having refused anything. A refusal
// and a timeout are genuinely different events and the radio reports them with
// different numbers, so:
//
//   Rejected  -- the AP or RADIUS said NO. Credentials or config. Give up
//                eventually; retrying is how an account gets locked.
//   Ambiguous -- the handshake began and did not finish. Could be either.
//                Cool this network off and try the next; NEVER give up on it.
//   Signal    -- never heard it, or heard it and lost it. Retry for ever;
//                this one fixes itself when the board moves.
enum class ReasonKind : uint8_t { Signal, Ambiguous, Rejected };

inline ReasonKind wifiReasonKind(uint8_t reason) {
    switch (reason) {
        // Someone said no, in so many words.
        case 2:    // AUTH_EXPIRE
        case 20:   // AKMP_INVALID
        case 23:   // 802_1X_AUTH_FAILED
        case 24:   // CIPHER_SUITE_REJECTED
        case 202:  // AUTH_FAIL
            return ReasonKind::Rejected;

        // The handshake started and did not finish. A wrong key does this. So
        // does a weak link. Nothing in the code can tell them apart, so it must
        // not pretend to.
        case 1:    // UNSPECIFIED
        case 15:   // 4WAY_HANDSHAKE_TIMEOUT
        case 16:   // GROUP_KEY_UPDATE_TIMEOUT
        case 17:   // IE_IN_4WAY_DIFFERS
        case 204:  // HANDSHAKE_TIMEOUT
        case 205:  // CONNECTION_FAIL
            return ReasonKind::Ambiguous;

        default:
            return ReasonKind::Signal;
    }
}

inline bool wifiReasonIsRejection(uint8_t reason) {
    return wifiReasonKind(reason) == ReasonKind::Rejected;
}

// MEASURED, and the reason the first version of this was wrong: the board
// associated happily at -85 dBm while this function was calling -81 "too weak to
// rely on". At these levels the link works and the retries get expensive, which
// is a different statement from "it will not work" -- and a health panel that
// cries wolf is one you stop reading.
inline const char* rssiVerdict(int dbm) {
    if (dbm >= -67) return "comfortable";
    if (dbm >= -75) return "workable";
    if (dbm >= -85) return "marginal -- expect retries and slow requests";
    return "very weak -- expect dropped requests";
}

inline const char* wifiAuthModeText(int mode) {
    switch (mode) {
        case 0: return "open";
        case 1: return "WEP";
        case 2: return "WPA-PSK";
        case 3: return "WPA2-PSK";
        case 4: return "WPA/WPA2-PSK";
        case 5: return "WPA2-ENTERPRISE";
        case 6: return "WPA3-PSK";
        case 7: return "WPA2/WPA3-PSK";
        case 8: return "WAPI-PSK";
        default: return "?";
    }
}

// One network. Fill these in secrets.h via WIFI_NETWORK_LIST -- see
// include/secrets.h.example, which has a worked line for each kind.
struct WifiNetwork {
    const char* ssid = "";

    // Personal (WPA2-PSK): set password, leave the EAP fields empty.
    const char* password = "";

    // Enterprise (802.1X PEAP/TTLS): set username + eapPassword. identity is
    // the outer/anonymous one; with most university setups it is simply the
    // same as the username, and leaving it empty does that for you.
    const char* identity = "";
    const char* username = "";
    const char* eapPassword = "";

    // Do not PREFER this network below this signal. It still gets used if it is
    // all there is -- this only decides whether it outranks the next one down
    // the list. -85 is about where a link stops being usable in practice.
    int8_t minRssi = -85;

    bool isEnterprise() const { return username != nullptr && username[0] != '\0'; }
    bool valid() const { return ssid != nullptr && ssid[0] != '\0'; }
};

constexpr size_t kMaxNetworks = 6;

struct WifiOptions {
    uint32_t connectTimeoutMs = 25000;
    // Do not hammer the AP after a failure -- and on an enterprise network, do
    // not hammer the RADIUS server either. Repeated failed 802.1X attempts are
    // the kind of thing that gets an account locked.
    uint32_t retryBackoffMs = 20000;
    // After an AP has actively rejected us, back off much harder: nothing about
    // the situation changes in twenty seconds, and each attempt is another
    // failed login against a university account.
    uint32_t authFailBackoffMs = 300000;   // 5 minutes
    // ...and eventually give up ON THAT NETWORK, not on all of them. A wrong
    // dorm credential must not stop the board falling back to the hotspot.
    // Only rejections count; being out of range never gives up.
    uint8_t maxAuthFailures = 6;
    // An AMBIGUOUS failure -- a handshake that started and did not finish --
    // gets this many tries before the network is put in the corner and the next
    // one down the list is tried instead. It is never given up on: the cooldown
    // expires and it is eligible again, because a lossy link is a thing that
    // gets better.
    uint8_t maxSoftFailures = 2;
    uint32_t softCooldownMs = 600000;   // 10 minutes
    // How often to re-survey while nothing is joined. Moving the board, or the
    // hotspot being switched on, should be noticed without a reboot.
    uint32_t rescanEveryMs = 60000;
};

class WifiLink {
public:
    // The list is Nakul's, in his order, and is NOT copied -- it lives in
    // secrets.h as static storage and outlives this object.
    // ⚠️ Call BEFORE begin() when the SmallTV's private access point is wanted.
    //
    // It changes every WiFi.mode() call below from WIFI_STA to WIFI_AP_STA, and
    // that matters more than it looks. connect() deliberately drops the radio
    // and re-enters station mode to work around the enterprise-config ordering
    // bug -- in plain STA that path calls esp_wifi_deinit(), which would take
    // the access point down with it and disconnect the television on every
    // reconnect. In AP_STA it does not: disabling STA leaves the mode as
    // WIFI_AP, the driver stays initialised, and the SmallTV stays associated.
    void keepAccessPoint(bool on) { apMode_ = on; }

    void begin(const WifiNetwork* nets, size_t count, const WifiOptions& opt = {}) {
        nets_ = nets;
        count_ = count > kMaxNetworks ? kMaxNetworks : count;
        opt_ = opt;
        WiFi.persistent(false);        // do not wear out flash storing credentials
        WiFi.mode(mode());
        WiFi.setSleep(false);          // latency over the last milliamp
        // We drive the retries ourselves, always. The driver's own reconnect has
        // no backoff and no idea there is a list -- against RADIUS that is an
        // attack on Nakul's own university account, and against the list it
        // would pin us to whichever network failed first.
        WiFi.setAutoReconnect(false);
        listen();
        survey();
        connectBest();
    }

    bool connected() const { return WiFi.status() == WL_CONNECTED; }
    int rssi() const { return connected() ? static_cast<int>(WiFi.RSSI()) : 0; }
    const char* ssid() const {
        return (current_ >= 0 && current_ < static_cast<int>(count_)) ? nets_[current_].ssid
                                                                     : "";
    }
    uint32_t reconnects() const { return reconnects_; }
    const char* lastNote() const { return note_; }
    uint8_t lastReason() const { return lastReason_; }
    int currentIndex() const { return current_; }
    bool allGaveUp() const {
        for (size_t i = 0; i < count_; ++i) if (!gaveUp_[i]) return false;
        return count_ > 0;
    }

    void reportLink() const {
        Serial.printf("[wifi] on %s via %s  ch%d  %d dBm -- %s\n",
                      ssid(), WiFi.BSSIDstr().c_str(),
                      static_cast<int>(WiFi.channel()),
                      static_cast<int>(WiFi.RSSI()),
                      rssiVerdict(static_cast<int>(WiFi.RSSI())));
        // getTxPower() reports quarter-dBm. Worth printing rather than assuming:
        // if the radio is ever not at its 19.5 dBm maximum on a link this
        // marginal, that is the first thing to know and the easiest to miss.
        Serial.printf("[wifi] tx power %d dBm\n",
                      static_cast<int>(WiFi.getTxPower()) / 4);
    }

    // What to put on a screen when the link is down: the radio's own account of
    // the last failure if there is one, otherwise whatever we were doing.
    const char* failureText() const {
        if (allGaveUp()) return "all rejected";
        if (wifiReasonKind(lastReason_) == ReasonKind::Ambiguous) return "handshake failed";
        if (lastReason_ != 0) return wifiReasonText(lastReason_);
        return note_;
    }

    // Print any disconnect the event task has recorded since the last call.
    // Deliberately not printed from the event callback itself: that runs in the
    // WiFi task, and a serial write there is a good way to turn a diagnostic
    // into a second bug.
    void pollLog() {
        if (!pending_) return;
        pending_ = false;
        const uint8_t r = lastReason_;
        // The driver can emit the same disconnect several times while an attempt
        // unwinds. Say it once per attempt; a repeated line is not a repeated
        // fact.
        if (r == printedReason_) return;
        printedReason_ = r;
        Serial.printf("[wifi] %s: disconnected, reason %u (%s)\n", ssid(),
                      static_cast<unsigned>(r), wifiReasonText(r));
        switch (wifiReasonKind(r)) {
            case ReasonKind::Rejected:
                Serial.println("[wifi] -- the AP refused us outright. Credentials or config,");
                Serial.println("[wifi]    not signal. Backing off to protect the account.");
                break;
            case ReasonKind::Ambiguous:
                Serial.println("[wifi] -- the handshake began and did not finish. That is a");
                Serial.println("[wifi]    wrong key OR a lossy link, and nothing here can tell");
                Serial.println("[wifi]    them apart. Cooling this one off, trying the next.");
                break;
            case ReasonKind::Signal:
                if (r == 201) {
                    Serial.println("[wifi] -- never heard the AP. Range, or a 5 GHz-only SSID:");
                    Serial.println("[wifi]    this chip is 2.4 GHz only.");
                } else if (r == 200) {
                    Serial.println("[wifi] -- heard it, then lost it. That is signal.");
                }
                break;
        }
    }

    // Survey the air and print what is out there. Safe to call before begin();
    // leaves the radio in station mode and disconnected. Takes a few seconds.
    void survey() {
        WiFi.mode(mode());
        WiFi.disconnect(false);
        delay(100);
        for (size_t i = 0; i < count_; ++i) {
            seen_[i] = false;
            rssi_[i] = -127;
            haveBssid_[i] = false;
        }

        Serial.println("[scan] surveying (2.4 GHz only -- that is all this chip has)");
        const int16_t n = WiFi.scanNetworks(false /*async*/, true /*hidden*/);
        if (n <= 0) {
            Serial.printf("[scan] %d networks. If that is zero indoors, suspect the antenna.\n",
                          static_cast<int>(n));
            WiFi.scanDelete();
            lastSurveyMs_ = millis();
            return;
        }
        const int16_t limit = n > 255 ? 255 : n;
        for (int16_t i = 0; i < limit; ++i) {
            const uint8_t k = static_cast<uint8_t>(i);
            const String s = WiFi.SSID(k);
            const int r = static_cast<int>(WiFi.RSSI(k));
            int match = -1;
            for (size_t j = 0; j < count_; ++j) {
                if (nets_[j].valid() && s == nets_[j].ssid) { match = static_cast<int>(j); break; }
            }
            if (match >= 0) {
                if (!seen_[match] || r > rssi_[match]) {
                    seen_[match] = true;
                    rssi_[match] = static_cast<int8_t>(r < -127 ? -127 : r);
                    auth_[match] = static_cast<int8_t>(WiFi.encryptionType(k));
                    chan_[match] = static_cast<int32_t>(WiFi.channel(k));
                    // MEASURED 25 Aug 2026: the survey found -72 dBm on ch6 and
                    // the association landed on a DIFFERENT BSSID, ch1, at -84.
                    // WIFI_CONNECT_AP_BY_SIGNAL sorts the DRIVER's own scan,
                    // which is a separate scan taken seconds later -- and this
                    // link swings more than 10 dB in that time. So remember the
                    // BSSID we actually chose and ask for it by name.
                    const uint8_t* b = WiFi.BSSID(k);
                    if (b) {
                        std::memcpy(bssid_[match], b, 6);
                        haveBssid_[match] = true;
                    } else {
                        haveBssid_[match] = false;
                    }
                }
                Serial.printf("[scan]  * %-22s %4d dBm  ch%-3d %s\n", s.c_str(), r,
                              static_cast<int>(WiFi.channel(k)),
                              wifiAuthModeText(static_cast<int>(WiFi.encryptionType(k))));
            } else if (r > -75) {
                // Neighbours, but only the loud ones: this is a diagnostic, not
                // an inventory of the building.
                Serial.printf("[scan]    %-22s %4d dBm  ch%d\n", s.c_str(), r,
                              static_cast<int>(WiFi.channel(k)));
            }
        }
        WiFi.scanDelete();
        lastSurveyMs_ = millis();

        for (size_t i = 0; i < count_; ++i) {
            if (!nets_[i].valid()) continue;
            if (!seen_[i]) {
                Serial.printf("[scan] %d. %-22s not visible\n", static_cast<int>(i) + 1,
                              nets_[i].ssid);
                continue;
            }
            Serial.printf("[scan] %d. %-22s %4d dBm  %s  %s%s\n", static_cast<int>(i) + 1,
                          nets_[i].ssid, rssi_[i], wifiAuthModeText(auth_[i]),
                          rssiVerdict(rssi_[i]),
                          gaveUp_[i] ? "  [given up: credentials refused]" : "");
            // The mismatch that cost two sessions: an 802.1X SSID joined with no
            // EAP username looks exactly like weak signal from the outside.
            if (auth_[i] == 5 && !nets_[i].isEnterprise()) {
                Serial.printf("[scan]    MISMATCH: %s is WPA2-ENTERPRISE but no EAP username is set\n",
                              nets_[i].ssid);
            } else if (auth_[i] >= 2 && auth_[i] <= 4 && nets_[i].isEnterprise()) {
                Serial.printf("[scan]    MISMATCH: %s is a pre-shared-key network but EAP is set\n",
                              nets_[i].ssid);
            } else if (auth_[i] == 6 && !nets_[i].isEnterprise()) {
                Serial.printf("[scan]    %s is WPA3-only; this chip needs WPA2 or WPA2/WPA3\n",
                              nets_[i].ssid);
            }
        }
    }

    // Nakul's order, with one qualification: a network only outranks the next
    // one down while it is audible at or above its own minRssi. If nothing
    // clears its own bar, take the loudest thing we can hear rather than
    // nothing at all.
    bool eligible(size_t i) const {
        return nets_[i].valid() && !gaveUp_[i] && !resting_[i] && seen_[i];
    }

    int choose() const {
        for (size_t i = 0; i < count_; ++i) {
            if (!eligible(i)) continue;
            if (rssi_[i] >= nets_[i].minRssi) return static_cast<int>(i);
        }
        int best = -1;
        for (size_t i = 0; i < count_; ++i) {
            if (!eligible(i)) continue;
            if (best < 0 || rssi_[i] > rssi_[best]) best = static_cast<int>(i);
        }
        if (best >= 0) return best;
        // Everything is resting. Rather than sit here with no link at all, take
        // the best of the rested ones -- a cooldown is a preference, not a ban.
        for (size_t i = 0; i < count_; ++i) {
            if (!nets_[i].valid() || gaveUp_[i] || !seen_[i]) continue;
            if (best < 0 || rssi_[i] > rssi_[best]) best = static_cast<int>(i);
        }
        return best;
    }

    // Call often. Does nothing while the link is up.
    void loop(uint32_t nowMs) {
        pollLog();
        if (connected()) {
            if (!wasUp_) {
                wasUp_ = true;
                note_ = "connected";
                lastReason_ = 0;
                if (current_ >= 0) authFailures_[current_] = 0;
            }
            return;
        }
        if (wasUp_) { wasUp_ = false; ++reconnects_; note_ = "link lost"; }
        if (allGaveUp()) return;

        // Put a network that keeps half-connecting into the corner and move on.
        // Done here, in the main context, rather than in the event callback:
        // millis() is safe there but state machines with two owners are not.
        // A pinned attempt that failed means that access point is gone or will
        // not have us. Drop the pin before blaming the network.
        if (pinnedAttempt_ && current_ >= 0 && lastReason_ != 0) {
            pinFailed_[current_] = true;
            pinnedAttempt_ = false;
            Serial.printf("[wifi] %s: that BSSID did not work; letting the driver choose\n",
                          nets_[current_].ssid);
        }

        if (current_ >= 0 && softFailures_[current_] >= opt_.maxSoftFailures &&
            !resting_[current_]) {
            resting_[current_] = true;
            cooldownUntilMs_[current_] = nowMs + opt_.softCooldownMs;
            Serial.printf("[wifi] %s half-connected %u times; resting it for %u min\n",
                          nets_[current_].ssid,
                          static_cast<unsigned>(softFailures_[current_]),
                          static_cast<unsigned>(opt_.softCooldownMs / 60000));
            softFailures_[current_] = 0;
        }
        for (size_t i = 0; i < count_; ++i) {
            if (resting_[i] && static_cast<int32_t>(nowMs - cooldownUntilMs_[i]) >= 0) {
                resting_[i] = false;         // rested long enough; eligible again
                pinFailed_[i] = false;       // and worth trying the good AP again
            }
        }

        const uint32_t backoff = wifiReasonIsRejection(lastReason_)
                                     ? opt_.authFailBackoffMs
                                     : opt_.retryBackoffMs;
        if (nowMs - lastAttemptMs_ < backoff) return;
        lastAttemptMs_ = nowMs;

        // Re-survey before choosing again: the board may have moved, or the
        // hotspot may have been switched on since we last looked.
        if (nowMs - lastSurveyMs_ >= opt_.rescanEveryMs) survey();
        connectBest();
    }

private:
    void connectBest() {
        const int pick = choose();
        if (pick < 0) {
            note_ = "none in range";
            Serial.println("[wifi] none of the configured networks are in range");
            return;
        }
        current_ = pick;

        // When priority passes over something much louder, SAY SO. The policy is
        // right -- it is Nakul's order and the board should honour it -- but a
        // silent policy is indistinguishable from a bug, and the first time this
        // ran it joined a -88 dBm dorm AP with a -24 dBm hotspot sitting right
        // there. That was the configuration doing exactly what it was told, and
        // nothing on the wire said so.
        int louder = -1;
        for (size_t i = 0; i < count_; ++i) {
            if (static_cast<int>(i) == pick) continue;
            if (!eligible(i)) continue;
            if (rssi_[i] < rssi_[pick] + 10) continue;
            if (louder < 0 || rssi_[i] > rssi_[louder]) louder = static_cast<int>(i);
        }
        if (louder >= 0) {
            Serial.printf("[scan] NOTE: #%d %s (%d dBm) beats #%d %s (%d dBm) on signal,\n",
                          louder + 1, nets_[louder].ssid, static_cast<int>(rssi_[louder]),
                          pick + 1, nets_[pick].ssid, static_cast<int>(rssi_[pick]));
            Serial.printf("[scan]   but #%d is higher in your list and is above its own\n",
                          pick + 1);
            Serial.printf("[scan]   minRssi (%d). Set %s's minRssi above %d in secrets.h\n",
                          static_cast<int>(nets_[pick].minRssi), nets_[pick].ssid,
                          static_cast<int>(rssi_[pick]));
            Serial.println("[scan]   to fall through to the stronger one at this signal.");
        }

        Serial.printf("[wifi] joining %s (%s), %d dBm\n", nets_[pick].ssid,
                      nets_[pick].isEnterprise() ? "802.1X" : "psk",
                      static_cast<int>(rssi_[pick]));
        connect(nets_[pick]);
    }

    void listen() {
        if (listening_) return;
        listening_ = true;
        WiFi.onEvent(
            [this](WiFiEvent_t, WiFiEventInfo_t info) {
                const uint8_t r = static_cast<uint8_t>(info.wifi_sta_disconnected.reason);
                lastReason_ = r;
                pending_ = true;
                if (current_ < 0) return;
                switch (wifiReasonKind(r)) {
                    case ReasonKind::Rejected:
                        // Per network, not global. A wrong dorm credential must
                        // not stop the board falling back to the hotspot.
                        if (authFailures_[current_] < 255) ++authFailures_[current_];
                        if (opt_.maxAuthFailures != 0 &&
                            authFailures_[current_] >= opt_.maxAuthFailures) {
                            gaveUp_[current_] = true;
                        }
                        break;
                    case ReasonKind::Ambiguous:
                        if (softFailures_[current_] < 255) ++softFailures_[current_];
                        break;
                    case ReasonKind::Signal:
                        break;      // never counts; it fixes itself
                }
            },
            ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    }

    // Ask for the exact access point the survey liked, rather than re-scanning
    // and hoping. Falls back to "any BSSID for this SSID" when we do not have
    // one, or when pinning has already failed here -- an AP that has gone away
    // must not strand us on a network that is otherwise fine.
    void beginOn(const WifiNetwork& n, const char* password) {
        const int i = current_;
        const bool pin = i >= 0 && haveBssid_[i] && !pinFailed_[i];
        if (pin) {
            Serial.printf("[wifi] pinned to %02X:%02X:%02X:%02X:%02X:%02X ch%d\n",
                          bssid_[i][0], bssid_[i][1], bssid_[i][2],
                          bssid_[i][3], bssid_[i][4], bssid_[i][5],
                          static_cast<int>(chan_[i]));
            WiFi.begin(n.ssid, password, chan_[i], bssid_[i]);
            pinnedAttempt_ = true;
        } else {
            WiFi.begin(n.ssid, password);
            pinnedAttempt_ = false;
        }
    }

    // esp_err_t is worth printing here. Silence from these calls is exactly what
    // "twenty-five seconds of dots" is made of.
    static void say(const char* what, esp_err_t err) {
        if (err != ESP_OK) Serial.printf("[wifi] %s -> %s\n", what, esp_err_to_name(err));
    }

    void connect(const WifiNetwork& n) {
        // ORDER MATTERS. WiFi.disconnect(true) powers the radio down, and on
        // Arduino-ESP32 2.x that takes esp_wifi_deinit() with it -- after which
        // the enterprise-config calls below have nothing to configure and fail
        // quietly. Re-entering station mode re-initialises the driver so the EAP
        // state we set actually survives to WiFi.begin().
        WiFi.disconnect(true);
        delay(100);
        WiFi.mode(mode());
        // esp_wifi_deinit() took the power-save setting with it. On a marginal
        // link, modem sleep is the difference between a slow reply and a lost one.
        WiFi.setSleep(false);
        // MEASURED: five Campus BSSIDs are in range and the default fast scan
        // joined one at -83 dBm while a -75 was visible, because it takes the
        // FIRST acceptable AP rather than the strongest.
        WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
        WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
        delay(50);
        printedReason_ = 0;   // a fresh attempt earns a fresh line

        if (n.isEnterprise()) {
#if defined(COMMUTE_TV_EAP_NONE)
            note_ = "no EAP support in this core";
            return;
#else
            const char* id = (n.identity && n.identity[0]) ? n.identity : n.username;
            const int idLen = static_cast<int>(std::strlen(id));
            const int userLen = static_cast<int>(std::strlen(n.username));
            const int passLen = static_cast<int>(std::strlen(n.eapPassword));
            // Lengths, never the values: this line ends up in a serial log.
            Serial.printf("[wifi] EAP identity %d chars, username %d, password %d\n",
                          idLen, userLen, passLen);
  #if COMMUTE_TV_EAP_V3
            say("set_identity", esp_eap_client_set_identity(
                                    reinterpret_cast<const unsigned char*>(id), idLen));
            say("set_username", esp_eap_client_set_username(
                                    reinterpret_cast<const unsigned char*>(n.username), userLen));
            say("set_password", esp_eap_client_set_password(
                                    reinterpret_cast<const unsigned char*>(n.eapPassword), passLen));
            say("enterprise_enable", esp_wifi_sta_enterprise_enable());
  #else
            say("set_identity", esp_wifi_sta_wpa2_ent_set_identity(
                                    reinterpret_cast<const unsigned char*>(id), idLen));
            say("set_username", esp_wifi_sta_wpa2_ent_set_username(
                                    reinterpret_cast<const unsigned char*>(n.username), userLen));
            say("set_password", esp_wifi_sta_wpa2_ent_set_password(
                                    reinterpret_cast<const unsigned char*>(n.eapPassword), passLen));
            say("wpa2_ent_enable", esp_wifi_sta_wpa2_ent_enable());
  #endif
            beginOn(n, nullptr);
            note_ = "802.1X";
#endif
        } else {
            // ⚠️ THE ONE THAT ONLY BITES WITH A LIST. Enterprise mode is sticky:
            // having enabled it for the dorm, joining the hotspot with a
            // passphrase while it is still on fails in a way that looks exactly
            // like a wrong password. Turn it off on the way past.
#if !defined(COMMUTE_TV_EAP_NONE)
  #if COMMUTE_TV_EAP_V3
            esp_wifi_sta_enterprise_disable();
  #else
            esp_wifi_sta_wpa2_ent_disable();
  #endif
#endif
            beginOn(n, n.password);
            note_ = "psk";
        }
    }

    wifi_mode_t mode() const { return apMode_ ? WIFI_AP_STA : WIFI_STA; }

    bool apMode_ = false;
    const WifiNetwork* nets_ = nullptr;
    size_t count_ = 0;
    WifiOptions opt_;

    int current_ = -1;
    bool seen_[kMaxNetworks] = {};
    int8_t rssi_[kMaxNetworks] = {};
    int8_t auth_[kMaxNetworks] = {};
    bool gaveUp_[kMaxNetworks] = {};
    // ⚠️ A separate flag rather than "cooldownUntilMs_ != 0". This project has
    // already paid for that mistake once, in core::RequestBudget's ring buffer:
    // zero is a legal instant, and millis() passes through it every 49.7 days.
    // Using a value as its own sentinel is the bug, not the odds of hitting it.
    uint32_t cooldownUntilMs_[kMaxNetworks] = {};
    bool resting_[kMaxNetworks] = {};
    uint8_t bssid_[kMaxNetworks][6] = {};
    bool haveBssid_[kMaxNetworks] = {};
    int32_t chan_[kMaxNetworks] = {};
    bool pinFailed_[kMaxNetworks] = {};
    bool pinnedAttempt_ = false;
    volatile uint8_t authFailures_[kMaxNetworks] = {};
    volatile uint8_t softFailures_[kMaxNetworks] = {};

    uint32_t lastAttemptMs_ = 0;
    uint32_t lastSurveyMs_ = 0;
    uint32_t reconnects_ = 0;
    bool wasUp_ = false;
    bool listening_ = false;
    // Written by the WiFi event task, read by loop(). Single-byte stores on this
    // core are atomic; volatile is here to stop the compiler caching them.
    volatile uint8_t lastReason_ = 0;
    volatile bool pending_ = false;
    uint8_t printedReason_ = 0;   // main context only
    const char* note_ = "idle";
};

} // namespace platform
