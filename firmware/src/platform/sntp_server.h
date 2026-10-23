// sntp_server.h - the ESP32 answers "what time is it" for the television.
//
// WHY THIS EXISTS INSTEAD OF NAT.
//
// The SmallTV fetches its own clock over the internet, and on a private access
// point with no route out it cannot. NAPT would have solved it, but the Arduino
// core in use has no IP_NAPT -- confirmed on the board, not assumed:
//
//     [tv] this Arduino core has no IP_NAPT
//
// So take the other route. The television's web UI has an "NTP Server Domain"
// field, and the endpoint behind it is `/set?ntp=<host>` -- read out of the
// saved copy of its own UI rather than guessed at. Point it at 192.168.4.1 and
// answer the question ourselves. The ESP32 already knows the correct time to
// the second, from real NTP over the dorm connection.
//
// This is better than NAT for the clock in two ways: it works on this core, and
// the television's time comes from a source we can see rather than from
// whatever its firmware would otherwise have picked.
//
// ⚠️ IT DOES NOT FIX THE WEATHER. That comes from GeekMagic's own service over
// HTTPS, and nothing here can stand in for it. Say so rather than let it look
// like a bug: the clock, the date and the day/night brightness will be right,
// and the weather panel will not.
//
// The protocol implemented is SNTP as a server, which is a 48-byte request and
// a 48-byte reply. No leap seconds, no stratum negotiation, no authentication:
// this serves exactly one client, on a link that only it can reach.
#pragma once
#include <WiFiUdp.h>

#include <cstdint>
#include <cstring>
#include <ctime>

namespace platform {

// NTP counts seconds from 1900; time_t counts from 1970.
constexpr uint32_t kNtpEpochOffset = 2208988800UL;

class SntpServer {
public:
    // Returns false if the socket could not be opened, which is worth knowing
    // at boot rather than discovering by a television that never syncs.
    bool begin(uint16_t port = 123) {
        started_ = udp_.begin(port) == 1;
        if (!started_) Serial.println("[ntp] could not listen on UDP 123");
        else Serial.println("[ntp] serving time to the television on 192.168.4.1:123");
        return started_;
    }

    // Call often. Cheap when there is nothing waiting.
    void poll() {
        if (!started_) return;
        const int size = udp_.parsePacket();
        if (size <= 0) return;

        uint8_t pkt[48];
        const int n = udp_.read(pkt, sizeof pkt);
        // Anything that is not a 48-byte SNTP packet is not a question we know
        // how to answer. Drop it rather than reply with something malformed.
        if (n < 48) { ++ignored_; return; }

        const std::time_t nowSec = std::time(nullptr);
        if (nowSec < 1700000000) {
            // Our own clock is not trustworthy yet. Answering anyway would
            // teach the television a wrong time with full confidence, which is
            // worse than leaving it unsynced.
            ++refused_;
            return;
        }
        const uint32_t secs = static_cast<uint32_t>(nowSec) + kNtpEpochOffset;

        uint8_t out[48];
        std::memset(out, 0, sizeof out);
        // LI = 0 (no warning), VN = 4, Mode = 4 (server).
        out[0] = 0x24;
        out[1] = 1;      // stratum 1: we are synchronised to a real NTP source
        out[2] = 6;      // poll interval, log2 seconds
        out[3] = 0xEC;   // precision, log2 seconds (~15 ms)
        // Reference identifier: four ASCII characters for a stratum-1 source.
        std::memcpy(out + 12, "GPS\0", 4);

        writeTs(out + 16, secs);   // reference: when we last knew the time
        std::memcpy(out + 24, pkt + 40, 8);   // originate: echo their transmit
        writeTs(out + 32, secs);   // receive
        writeTs(out + 40, secs);   // transmit

        udp_.beginPacket(udp_.remoteIP(), udp_.remotePort());
        udp_.write(out, sizeof out);
        udp_.endPacket();
        ++served_;
    }

    bool running() const { return started_; }
    uint32_t served() const { return served_; }
    uint32_t refused() const { return refused_; }
    uint32_t ignored() const { return ignored_; }

private:
    static void writeTs(uint8_t* p, uint32_t seconds) {
        p[0] = static_cast<uint8_t>(seconds >> 24);
        p[1] = static_cast<uint8_t>(seconds >> 16);
        p[2] = static_cast<uint8_t>(seconds >> 8);
        p[3] = static_cast<uint8_t>(seconds);
        // Fraction left at zero. A second of precision is three orders of
        // magnitude better than a device that currently has no idea what year
        // it is, and this is a clock on a shelf.
        p[4] = p[5] = p[6] = p[7] = 0;
    }

    WiFiUDP udp_;
    bool started_ = false;
    uint32_t served_ = 0;
    uint32_t refused_ = 0;
    uint32_t ignored_ = 0;
};

} // namespace platform
