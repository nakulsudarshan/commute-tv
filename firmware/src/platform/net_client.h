// net_client.h - net::Client over WiFiClient / WiFiClientSecure.
//
// Everything interesting about reading an HTTP response -- de-chunking, header
// parsing, refusing a body that stopped early -- is in src/net/http_parse.cpp
// and is tested on the laptop. This file is the part that cannot be: open a
// socket, write a request line, shovel bytes into the reader, close.
//
// TWO THINGS ABOUT MEMORY, both measured on this board.
//
// 1. ONE TLS SESSION AT A TIME, AND NEVER WHILE A FRAME IS IN FLIGHT. An
//    mbedTLS session on this chip wants tens of KB, and the free heap after
//    boot is ~240 KB with a largest contiguous block of 110,580 bytes. The
//    orchestrator is sequential, so this is naturally satisfied -- but it is
//    satisfied by ACCIDENT unless someone writes it down, so: fetch over TLS,
//    close, then render and upload. The SmallTV is plain HTTP and never needs
//    a session at all.
// 2. stop() ON EVERY PATH. A WiFiClientSecure that goes out of scope without
//    stop() leaks its context, and four upstreams polled all day is a slow
//    death rather than a crash -- the worst kind to diagnose. Hence Session
//    below, which is an RAII guard and nothing else.
//
// ⚠️ CERTIFICATES. By default this REFUSES to make a TLS connection it cannot
// verify. A JWT signed with Nakul's Google service-account key goes over one of
// these; handing it to whoever answers on port 443 is not a trade worth making
// for convenience. Roots come from tools/fetch_root_ca.py, which extracts them
// from the machine's own CA store -- see INSTRUCTIONS.md. `allowInsecure` exists
// for bring-up, logs a warning every single time, and must not survive into a
// build that carries real credentials.
#pragma once
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <cstdio>
#include <cstring>

#include "net/http_parse.h"

namespace platform {

// A root certificate, and the host suffix it is good for. Suffix rather than
// exact match because Google answers on several names.
struct RootCa {
    const char* hostSuffix = "";
    const char* pem = "";
};

struct NetOptions {
    const RootCa* roots = nullptr;
    size_t rootCount = 0;

    // Bring-up only. Every connection made this way says so on the serial line.
    bool allowInsecure = false;

    uint32_t connectTimeoutSec = 10;
    uint32_t handshakeTimeoutSec = 15;
    // How long to wait for the whole response once the request has gone out.
    uint32_t readTimeoutMs = 15000;

    const char* userAgent = "commute-tv/1.0";
};

class NetClient final : public net::Client {
public:
    explicit NetClient(const NetOptions& opt) : opt_(opt) {}

    net::Result get(const char* url, const net::Header* headers, size_t headerCount,
                    net::BodySink& body) override {
        return run("GET", url, nullptr, nullptr, 0, headers, headerCount, body);
    }

    net::Result post(const char* url, const char* contentType, const char* reqBody,
                     size_t reqBodyLen, const net::Header* headers,
                     size_t headerCount, net::BodySink& out) override {
        return run("POST", url, contentType, reqBody, reqBodyLen, headers,
                   headerCount, out);
    }

    uint32_t tlsConnections() const { return tlsConnections_; }
    uint32_t insecureConnections() const { return insecureConnections_; }

private:
    // stop() on every path, including the ones that throw us out early.
    struct Session {
        WiFiClient* c;
        explicit Session(WiFiClient* client) : c(client) {}
        ~Session() { if (c) c->stop(); }
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
    };

    static bool iEqual(const char* a, const char* b) {
        for (; *a && *b; ++a, ++b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
            if (ca != cb) return false;
        }
        return *a == '\0' && *b == '\0';
    }

    const char* rootFor(const char* host) const {
        const size_t hlen = std::strlen(host);
        for (size_t i = 0; i < opt_.rootCount; ++i) {
            const char* suf = opt_.roots[i].hostSuffix;
            if (!suf || !suf[0]) continue;
            const size_t slen = std::strlen(suf);
            if (slen <= hlen && std::strcmp(host + (hlen - slen), suf) == 0) {
                return opt_.roots[i].pem;
            }
        }
        return nullptr;
    }

    net::Result run(const char* method, const char* url, const char* contentType,
                    const char* reqBody, size_t reqBodyLen,
                    const net::Header* headers, size_t headerCount,
                    net::BodySink& sink) {
        net::Result res;

        net::Url u;
        if (!net::parseUrl(url, u)) {
            res.error = net::Error::Protocol;
            // Say WHY. The first version of this printed "cannot parse that url"
            // and nothing else, which sent us looking at TLS and certificates
            // when the answer was a buffer 48 bytes too small.
            const size_t n = url ? std::strlen(url) : 0;
            Serial.printf("[net] cannot parse a %u-char url (host cap %u, path cap %u)\n",
                          static_cast<unsigned>(n),
                          static_cast<unsigned>(net::kMaxHostLen),
                          static_cast<unsigned>(net::kMaxPathLen));
            if (url) {
                // Scheme and host only: the query carries the calendar id.
                char head[72];
                size_t i = 0;
                for (; url[i] && i < sizeof head - 1 && url[i] != '?'; ++i) head[i] = url[i];
                head[i] = '\0';
                Serial.printf("[net]   %s%s\n", head, url[i] ? "?..." : "");
            }
            return res;
        }

        WiFiClientSecure tls;
        WiFiClient plain;
        WiFiClient* c = nullptr;

        if (u.tls) {
            const char* pem = rootFor(u.host);
            if (pem) {
                tls.setCACert(pem);
            } else if (opt_.allowInsecure) {
                // Loudly, every time. A warning that scrolls past once is a
                // warning nobody sees.
                Serial.printf("[net] ⚠ NO ROOT CA for %s -- connecting UNVERIFIED\n",
                              u.host);
                tls.setInsecure();
                ++insecureConnections_;
            } else {
                Serial.printf("[net] refusing %s: no root CA configured\n", u.host);
                res.error = net::Error::Tls;
                return res;
            }
            tls.setHandshakeTimeout(opt_.handshakeTimeoutSec);
            c = &tls;
            ++tlsConnections_;
        } else {
            c = &plain;
        }

        Session guard(c);
        // ⚠️ WiFiClient::setTimeout takes SECONDS on ESP32, not milliseconds as
        // Stream::setTimeout does everywhere else in Arduino. Passing 15000 here
        // asks for a four-hour timeout, and it will wait for it.
        c->setTimeout(opt_.connectTimeoutSec);

        if (!c->connect(u.host, u.port)) {
            Serial.printf("[net] cannot reach %s:%u\n", u.host, u.port);
            res.error = u.tls ? net::Error::Tls : net::Error::Unreachable;
            return res;
        }

        if (!writeRequest(*c, method, u, contentType, reqBodyLen, headers,
                          headerCount)) {
            res.error = net::Error::Unreachable;
            return res;
        }
        if (reqBody && reqBodyLen) {
            if (c->write(reinterpret_cast<const uint8_t*>(reqBody), reqBodyLen)
                != reqBodyLen) {
                res.error = net::Error::Unreachable;
                return res;
            }
        }

        net::ResponseReader rd(sink);
        uint8_t buf[512];
        const uint32_t deadline = millis() + opt_.readTimeoutMs;
        bool ok = true;

        while (!rd.complete()) {
            if (millis() > deadline) {
                Serial.printf("[net] %s timed out after %u ms\n", u.host,
                              static_cast<unsigned>(opt_.readTimeoutMs));
                res.error = net::Error::Timeout;
                return res;
            }
            const int avail = c->available();
            if (avail > 0) {
                const int n = c->read(buf, sizeof buf);
                if (n <= 0) break;
                if (!rd.feed(reinterpret_cast<const char*>(buf),
                             static_cast<size_t>(n))) {
                    ok = false;
                    break;
                }
                continue;
            }
            if (!c->connected()) break;    // peer hung up; finish() decides
            delay(5);
        }

        if (ok && !rd.complete()) ok = rd.finish();

        res.status = rd.status();
        res.bytes = rd.bodyBytes();
        res.retryAfterSec = rd.retryAfterSec();
        res.error = ok ? net::Error::None : rd.error();
        if (!ok && res.error == net::Error::None) res.error = net::Error::Protocol;
        return res;
    }

    bool writeRequest(WiFiClient& c, const char* method, const net::Url& u,
                      const char* contentType, size_t bodyLen,
                      const net::Header* headers, size_t headerCount) {
        char head[512];
        // Connection: close, deliberately. Keep-alive would save a handshake and
        // cost a live TLS context sitting on the heap between requests, which is
        // the one thing this chip cannot spare.
        int n = std::snprintf(head, sizeof head,
                              "%s %s HTTP/1.1\r\n"
                              "Host: %s\r\n"
                              "User-Agent: %s\r\n"
                              "Accept-Encoding: identity\r\n"
                              "Connection: close\r\n",
                              method, u.path, u.host, opt_.userAgent);
        if (n < 0 || static_cast<size_t>(n) >= sizeof head) return false;
        if (!writeAll(c, head, static_cast<size_t>(n))) return false;

        if (contentType && contentType[0]) {
            n = std::snprintf(head, sizeof head,
                              "Content-Type: %s\r\nContent-Length: %u\r\n",
                              contentType, static_cast<unsigned>(bodyLen));
            if (n < 0 || static_cast<size_t>(n) >= sizeof head) return false;
            if (!writeAll(c, head, static_cast<size_t>(n))) return false;
        }
        for (size_t i = 0; i < headerCount; ++i) {
            if (!headers[i].name || !headers[i].value) continue;
            // ⚠️ net::Client::post takes a contentType AND a header list, and a
            // caller may reasonably put Content-Type in either. Sending both
            // produced, from openrouteservice:
            //
            //   http 500 -- Invalid mime type "application/json, application/json"
            //
            // The server joined the duplicates, exactly as HTTP says it may. The
            // parameter is authoritative; a Content-Type in the list is dropped.
            if (contentType && contentType[0] && iEqual(headers[i].name, "content-type")) {
                continue;
            }
            n = std::snprintf(head, sizeof head, "%s: %s\r\n", headers[i].name,
                              headers[i].value);
            // An Authorization header is ~2 KB and will not fit this buffer, so
            // long values go out in pieces rather than being silently dropped.
            if (n < 0 || static_cast<size_t>(n) >= sizeof head) {
                if (!writeAll(c, headers[i].name, std::strlen(headers[i].name)) ||
                    !writeAll(c, ": ", 2) ||
                    !writeAll(c, headers[i].value, std::strlen(headers[i].value)) ||
                    !writeAll(c, "\r\n", 2)) {
                    return false;
                }
                continue;
            }
            if (!writeAll(c, head, static_cast<size_t>(n))) return false;
        }
        return writeAll(c, "\r\n", 2);
    }

    static bool writeAll(WiFiClient& c, const char* p, size_t n) {
        size_t sent = 0;
        while (sent < n) {
            const size_t w = c.write(reinterpret_cast<const uint8_t*>(p + sent),
                                     n - sent);
            if (w == 0) return false;
            sent += w;
        }
        return true;
    }

    NetOptions opt_;
    uint32_t tlsConnections_ = 0;
    uint32_t insecureConnections_ = 0;
};

} // namespace platform
