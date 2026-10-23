// tv_internet.h - letting the television reach the internet, and nothing reach
// the television.
//
// The SmallTV's stock firmware fetches its own clock and weather. On a private
// access point with no route out it can do neither, which is why it sits at
// reduced brightness with the wrong time. NAPT in lwIP forwards its outbound
// traffic through the dorm connection.
//
// ⚠️ THIS DOES NOT WEAKEN THE PROPERTY THE PRIVATE AP EXISTS FOR.
//
// NAPT is outbound-only. A connection can be opened from the television to the
// internet; nothing on the dorm network -- or anywhere else -- can open one to
// the television. Its unauthenticated /update endpoint stays exactly as
// unreachable as it was, which was the entire reason for the private AP.
//
// ⚠️ NOR MAY IT MAKE THE LINK SHAREABLE, AND THAT IS NOW A CHECK RATHER THAN A
// COMMENT. The access point has a second client slot so that a laptop can reach
// the television's own web UI (see smalltv_link.h). A dead-end network with two
// devices on it is nobody's business but Nakul's; a NAT'd one with two devices
// on it is a shared dorm connection under a MAC registered for one board, which
// is exactly what the rules forbid. So enableTvInternet() takes the number of
// slots and REFUSES rather than trusting anyone to remember the interaction.
//
// ⚠️ IT MAY NOT COMPILE, AND THAT IS THE POINT OF THE GUARD.
//
// IP_NAPT has to be enabled in the lwIP that ships with the installed Arduino
// core, and whether it is depends on the build. So everything here is behind
// __has_include and IP_NAPT, and a core without it produces a firmware that
// says so on the serial line rather than one that fails to link. There is no
// way to answer that question from a laptop with no ESP32 toolchain.
#pragma once
#include <WiFi.h>

#include <cstdio>

#if __has_include(<lwip/lwip_napt.h>)
  #include <lwip/lwip_napt.h>
  #include <lwip/netif.h>
  #define COMMUTE_TV_HAVE_NAPT_HEADER 1
#else
  #define COMMUTE_TV_HAVE_NAPT_HEADER 0
#endif

namespace platform {

// Hands the access point's clients a DNS server and turns on address
// translation. Call once, after the station is up and the AP has been started,
// passing the number of clients the AP admits.
//
// Returns false when the core cannot do it -- which is a fact about the build,
// not a failure of this run, and is reported as such -- and when forwarding
// would turn a private link into a shared one.
inline bool enableTvInternet(uint8_t apMaxClients) {
    if (apMaxClients > 1) {
        Serial.println("[tv] NOT enabling NAT: the AP admits more than one device,");
        Serial.println("[tv]   and forwarding for anything but the television would");
        Serial.println("[tv]   be sharing the dorm connection. Set maxClients = 1 in");
        Serial.println("[tv]   SmallTvApConfig if the television's weather matters");
        Serial.println("[tv]   more than reaching its web UI from a laptop.");
        return false;
    }
#if COMMUTE_TV_HAVE_NAPT_HEADER && defined(IP_NAPT) && IP_NAPT
    // The television asks for a DNS server in its DHCP lease. Without one it
    // resolves nothing and the clock stays wrong no matter how good the route
    // is -- which would look exactly like NAT not working.
    const ip_addr_t* upstream = dns_getserver(0);
    if (upstream) {
        dhcps_offer_t offer = OFFER_DNS;
        dhcps_set_option_info(DOMAIN_NAME_SERVER, &offer, sizeof offer);
    }

    // Translate on the access point's netif. esp_netif hands out 192.168.4.1.
    const ip_addr_t apIp = IPADDR4_INIT_BYTES(192, 168, 4, 1);
    const err_t rc = ip_napt_enable(apIp.u_addr.ip4.addr, 1);
    if (rc != ERR_OK) {
        Serial.printf("[tv] NAT could not be enabled (lwip err %d)\n",
                      static_cast<int>(rc));
        return false;
    }
    Serial.println("[tv] internet enabled for the television (outbound only --");
    Serial.println("[tv]   nothing can open a connection TO it)");
    return true;
#else
    Serial.println("[tv] this Arduino core has no IP_NAPT, so the television");
    Serial.println("[tv]   cannot reach the internet. Its clock and weather will");
    Serial.println("[tv]   stay wrong; everything commute-tv shows is unaffected.");
    return false;
#endif
}

} // namespace platform
