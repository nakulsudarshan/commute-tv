// smalltv_link.h - the private radio link to the television, and the socket
// that talks to it.
//
// ⚠️ THE SMALLTV NEVER JOINS THE DORM NETWORK. Its /update endpoint is
// unauthenticated -- the stock binary literally contains the string
// "Unauthenticated Update" -- so on a shared network any resident could reflash
// it. Instead the ESP32 runs an access point that is structurally incapable of
// being anything else:
//
//   * hidden SSID, WPA2, a passphrase typed into the television exactly once.
//   * NO ROUTE OUT. This is the property that matters, and it is worth being
//     exact about which mechanism provides it.
//
// ⚠️ WHY THERE ARE TWO CLIENT SLOTS AND NOT ONE.
//
// It used to be one, on the reasoning that dorm rules forbid sharing the
// connection and a one-client radio makes sharing impossible rather than merely
// forbidden. That reasoning was sound but it was guarding the wrong thing:
// what actually makes this link unshareable is that NOTHING IS FORWARDED. With
// NAPT off -- and this Arduino core has no IP_NAPT, confirmed on the board -- a
// device on this AP can reach the ESP32 and the television and nowhere else. It
// is a dead end, not an uplink, however many devices are on it.
//
// The second slot exists because the television's own web UI is only reachable
// from this network, and with one slot there was no way to open it at all: you
// cannot be on the AP and on the dorm network at once. So the second slot is
// for Nakul's own laptop or phone, on a network that goes nowhere.
//
// The guarantee is kept where it belongs: enableTvInternet() REFUSES to switch
// NAPT on while there is more than one slot. If a future core does have
// IP_NAPT, the choice becomes explicit -- internet for the television, or a
// second device on the AP -- rather than silently becoming a shared uplink.
//
// ONE RADIO, ONE CHANNEL. The access point cannot choose its own channel while
// the station is connected -- the driver forces it onto the station's. So the
// AP is started AFTER the station is up, on the channel it actually got, which
// avoids a pointless hop at boot. When the dorm link later moves channel the AP
// moves with it and the television re-associates by itself: a gap of seconds,
// not a failure.
//
// FINDING THE TELEVISION. It gets a DHCP lease from us, so its address is ours
// to know -- but the IDF call that reads the lease table has moved between
// versions, and a header this project cannot compile against is a header worth
// not depending on. Instead it asks: a handful of GETs for /v.json across the
// small DHCP pool, on a private network with exactly one possible client. The
// first lease is almost always .2, so this is usually one request, and it is
// built on nothing more exotic than a socket.
#pragma once
#include <WiFi.h>

#include <cstdio>
#include <cstring>

#include "net/http_parse.h"
#include "smalltv/smalltv_client.h"

namespace platform {

struct SmallTvApConfig {
    const char* ssid = "";
    const char* pass = "";
    // Hidden keeps the SSID out of the dorm's air. It is obscurity and not
    // security -- WPA2 and the passphrase are the security -- so flip it to
    // false if typing a hidden SSID into a phone every time gets tiresome.
    bool hidden = true;
    // The television, plus ONE of Nakul's own devices for reaching its web UI.
    // Raising this further is not a security hole by itself; see the note at
    // the top about which mechanism actually guards what, and note that
    // enableTvInternet() refuses to run while this is above 1.
    uint8_t maxClients = 2;
    // The DHCP pool to look in. The ESP32 hands out .2 first; the rest is for
    // the case where the television reconnected and got a different lease.
    uint8_t firstHost = 2;
    uint8_t lastHost = 6;
    uint32_t httpTimeoutMs = 4000;
};

class SmallTvLink final : public smalltv::HttpTransport {
public:
    explicit SmallTvLink(const SmallTvApConfig& cfg) : cfg_(cfg) {}

    // Start the access point. Call once the station is connected, so the AP
    // opens on the channel it is going to be forced onto anyway.
    bool beginAp(int staChannel) {
        if (!cfg_.ssid || !cfg_.ssid[0] || !cfg_.pass || std::strlen(cfg_.pass) < 8) {
            Serial.println("[tv] no AP ssid/passphrase (WPA2 needs 8+ chars) -- not starting");
            return false;
        }
        const int ch = staChannel > 0 ? staChannel : 1;
        const bool ok = WiFi.softAP(cfg_.ssid, cfg_.pass, ch, cfg_.hidden ? 1 : 0,
                                    cfg_.maxClients);
        if (!ok) {
            Serial.println("[tv] softAP failed to start");
            return false;
        }
        apUp_ = true;
        apChannel_ = ch;
        Serial.printf("[tv] AP \"%s\" up on ch%d, %s, max %u client(s)\n", cfg_.ssid, ch,
                      cfg_.hidden ? "hidden" : "visible",
                      static_cast<unsigned>(cfg_.maxClients));
        Serial.printf("[tv] AP address %s -- point the television here\n",
                      WiFi.softAPIP().toString().c_str());
        if (cfg_.maxClients > 1) {
            Serial.printf("[tv] a second device may join%s to reach the television's\n",
                          cfg_.hidden ? " (add the network by hand: it is hidden)" : "");
            Serial.println("[tv]   own web UI. That network has no route out -- joining it");
            Serial.println("[tv]   costs you the internet until you leave it again.");
        }
        return true;
    }

    bool apUp() const { return apUp_; }
    int apChannel() const { return apChannel_; }
    // How many devices the radio will admit. enableTvInternet() asks, because
    // forwarding traffic for more than the television is the one thing this
    // access point must never do.
    uint8_t maxClients() const { return cfg_.maxClients; }
    int clients() const { return apUp_ ? WiFi.softAPgetStationNum() : 0; }
    bool found() const { return host_[0] != '\0'; }
    const char* host() const { return host_; }

    // Say when a client comes and goes. On a one-client AP this is the
    // television, and knowing it associated is most of the diagnosis when it
    // does not answer.
    void poll() {
        const int n = clients();
        if (n != lastClients_) {
            lastClients_ = n;
            Serial.printf("[tv] %d client(s) on the AP\n", n);
            if (n == 0) host_[0] = '\0';    // its lease means nothing now
        }
    }

    // Find the television, or confirm we still know where it is. Cheap when the
    // answer has not changed.
    bool discover() {
        if (!apUp_) return false;
        if (clients() == 0) { host_[0] = '\0'; return false; }
        if (host_[0] && probe(host_)) return true;

        char candidate[20];
        for (uint8_t h = cfg_.firstHost; h <= cfg_.lastHost; ++h) {
            std::snprintf(candidate, sizeof candidate, "192.168.4.%u",
                          static_cast<unsigned>(h));
            if (!probe(candidate)) continue;
            std::strncpy(host_, candidate, sizeof host_ - 1);
            host_[sizeof host_ - 1] = '\0';
            Serial.printf("[tv] found the television at %s\n", host_);
            return true;
        }
        Serial.println("[tv] a client is associated but nothing answered /v.json");
        return false;
    }

    // --- smalltv::HttpTransport ---------------------------------------------
    smalltv::HttpResponse get(const char* path) override {
        return request("GET", path, nullptr, nullptr, 0);
    }

    smalltv::HttpResponse post(const char* path, const char* contentType,
                               const smalltv::Slice* parts,
                               size_t partCount) override {
        return request("POST", path, contentType, parts, partCount);
    }

private:
    // Fills the small body buffer smalltv::HttpResponse carries, and says so
    // when there was more. The bodies we read are tens of bytes of JSON; an
    // image upload's REPLY is not big either.
    struct BodyInto final : net::BodySink {
        smalltv::HttpResponse* r;
        explicit BodyInto(smalltv::HttpResponse* res) : r(res) {}
        bool write(const char* data, size_t n) override {
            const size_t cap = sizeof r->body - 1;
            size_t take = n;
            if (r->bodyLen + take > cap) {
                take = cap - r->bodyLen;
                r->truncated = true;
            }
            if (take) {
                std::memcpy(r->body + r->bodyLen, data, take);
                r->bodyLen += take;
                r->body[r->bodyLen] = '\0';
            }
            return true;    // keep reading: the response must still be framed
        }
    };

    bool probe(const char* ip) {
        WiFiClient c;
        c.setTimeout(2);
        if (!c.connect(ip, 80)) return false;
        c.printf("GET /v.json HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", ip);
        const uint32_t deadline = millis() + 2000;
        // ⚠️ THE STATUS CODE, NOT MERELY THE WORD "HTTP". With a second device
        // allowed on the AP, "something answered on port 80" is no longer the
        // same statement as "the television is here" -- a laptop running any
        // web server would say HTTP too, and then every command meant for the
        // television would go to it. Only the device that SERVES /v.json is the
        // television; anything else answers 404 and is passed over.
        char head[16] = {0};
        size_t got = 0;
        while (millis() < deadline && (c.connected() || c.available())) {
            while (c.available() && got < sizeof head - 1) {
                head[got++] = static_cast<char>(c.read());
            }
            if (got >= 12) break;
            delay(5);
        }
        c.stop();
        return got >= 12 && std::strncmp(head, "HTTP/1.", 7) == 0 &&
               std::strncmp(head + 9, "200", 3) == 0;
    }

    smalltv::HttpResponse request(const char* method, const char* path,
                                  const char* contentType,
                                  const smalltv::Slice* parts, size_t partCount) {
        smalltv::HttpResponse res;
        if (!host_[0] && !discover()) {
            res.transport = smalltv::Transport::Unreachable;
            return res;
        }

        WiFiClient c;
        // ⚠️ SECONDS on ESP32, not milliseconds. See net_client.h.
        c.setTimeout(cfg_.httpTimeoutMs / 1000 + 1);
        if (!c.connect(host_, 80)) {
            res.transport = smalltv::Transport::Unreachable;
            host_[0] = '\0';                 // make the next call rediscover
            return res;
        }

        size_t bodyLen = 0;
        for (size_t i = 0; i < partCount; ++i) bodyLen += parts[i].len;

        char head[320];
        int n = std::snprintf(head, sizeof head,
                              "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n",
                              method, path, host_);
        if (n < 0 || static_cast<size_t>(n) >= sizeof head) {
            c.stop();
            res.transport = smalltv::Transport::Failed;
            return res;
        }
        c.write(reinterpret_cast<const uint8_t*>(head), static_cast<size_t>(n));
        if (contentType && contentType[0]) {
            n = std::snprintf(head, sizeof head,
                              "Content-Type: %s\r\nContent-Length: %u\r\n",
                              contentType, static_cast<unsigned>(bodyLen));
            if (n > 0) c.write(reinterpret_cast<const uint8_t*>(head),
                               static_cast<size_t>(n));
        }
        c.write(reinterpret_cast<const uint8_t*>("\r\n"), 2);
        for (size_t i = 0; i < partCount; ++i) {
            if (!writeAll(c, parts[i].data, parts[i].len)) {
                c.stop();
                res.transport = smalltv::Transport::Failed;
                return res;
            }
        }

        // The same reader the four HTTPS upstreams use, and for the same reason:
        // it is the one that has been fed every possible split of a byte stream.
        BodyInto sink(&res);
        net::ResponseReader rd(sink);
        uint8_t buf[256];
        const uint32_t deadline = millis() + cfg_.httpTimeoutMs;
        bool ok = true;
        while (!rd.complete()) {
            if (millis() > deadline) {
                c.stop();
                res.transport = smalltv::Transport::Timeout;
                return res;
            }
            if (c.available() > 0) {
                const int r = c.read(buf, sizeof buf);
                if (r <= 0) break;
                if (!rd.feed(reinterpret_cast<const char*>(buf),
                             static_cast<size_t>(r))) { ok = false; break; }
                continue;
            }
            if (!c.connected()) break;
            delay(2);
        }
        if (ok && !rd.complete()) ok = rd.finish();
        c.stop();

        res.status = rd.status();
        res.transport = ok ? smalltv::Transport::Ok : smalltv::Transport::Failed;
        return res;
    }

    static bool writeAll(WiFiClient& c, const uint8_t* p, size_t n) {
        size_t sent = 0;
        while (sent < n) {
            const size_t w = c.write(p + sent, n - sent);
            if (w == 0) return false;
            sent += w;
        }
        return true;
    }

    SmallTvApConfig cfg_;
    bool apUp_ = false;
    int apChannel_ = 0;
    int lastClients_ = -1;
    char host_[20] = {0};
};

} // namespace platform
