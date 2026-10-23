// commute-tv. The whole thing, on the real board.
//
// Calendar -> place -> journey -> alert -> the television, with the OLED
// telling the truth about all of it the entire time.
//
// This file is wiring and nothing else. Every decision lives in app::Orchestrator
// and is tested on the laptop against a fake; every protocol lives in a client
// that is tested the same way. What is here is the part that cannot be tested
// off the chip: which object gets which socket, and where the buffers come from.
//
// ⚠️ MEMORY. Two rules, both measured on this board (largest contiguous block:
// 110,580 bytes).
//
//   * The band buffer and the JPEG buffer are STATIC, in .bss. A malloc that
//     fails at alert time is the one failure you cannot recover from
//     gracefully -- it fails exactly when the device is finally needed.
//   * TLS and the frame never overlap. The orchestrator fetches over HTTPS,
//     closes the session, and only then renders and uploads. That ordering is a
//     property of the orchestrator being sequential; nothing enforces it, which
//     is why it is written down here as well as there.
#include <Arduino.h>

#include <cstring>

#include "app/live_sources.h"
#include "app/orchestrator.h"
#include "core/places.h"
#include "core/textutil.h"
#include "google/calendar.h"
#include "hafas/hafas_client.h"
#include "oled/health.h"
#include "oled/mono_canvas.h"
#include "oled/ssd1306.h"
#include "ors/ors_client.h"
#include "platform/local_clock.h"
#include "platform/net_client.h"
#include "platform/rsa_signer.h"
#include "platform/smalltv_link.h"
#include "platform/sntp_server.h"
#include "platform/tv_internet.h"
#include "platform/wifi_link.h"
#include "platform/wire_i2c.h"
#include "weather/meteo_client.h"

#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #error "Copy include/secrets.h.example to include/secrets.h and fill it in."
#endif
#if __has_include("secrets_generated.h")
  #include "secrets_generated.h"
#else
  #error "Run: python3 tools/gen_secrets.py"
#endif
#if __has_include("root_ca.h")
  #include "root_ca.h"
  #define HAVE_ROOT_CA 1
#else
  #define HAVE_ROOT_CA 0
#endif

// ⚠️ The Arduino loop task gets 8,192 bytes of stack by default, and an mbedTLS
// handshake wants several KB of that on its own before any of our code has
// asked for anything. Raising it is belt and braces -- the two 5.8 KB trip
// buffers that actually caused the `Double exception` reboot loop now live in
// .bss -- but a TLS handshake plus a JSON reader plus our own frames is not a
// thing to run on a stack sized for blinking an LED.
#if defined(SET_LOOP_TASK_STACK_SIZE)
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
#endif

namespace {

// --- the little screen -------------------------------------------------------
uint8_t g_frame[oled::kBufferBytes];          // 1 KB
platform::WireI2c g_bus;
oled::Config g_oledCfg;
oled::Display g_display(g_oledCfg, g_bus);
oled::MonoCanvas g_canvas(g_frame);

// --- the radios --------------------------------------------------------------
platform::WifiLink g_wifi;
#if defined(SMALLTV_AP_SSID) && defined(SMALLTV_AP_PASS)
platform::SmallTvApConfig g_apCfg{SMALLTV_AP_SSID, SMALLTV_AP_PASS};
constexpr bool kWantAp = true;
#else
platform::SmallTvApConfig g_apCfg{};
constexpr bool kWantAp = false;
#endif
platform::SmallTvLink g_tvLink(g_apCfg);
platform::LocalClock g_clock;

// The television gets its clock from us. See sntp_server.h for why this rather
// than NAT -- IP_NAPT is absent from this core, confirmed on the board.
platform::SntpServer g_ntp;
bool g_tvTimeSet = false;          // its NTP server has been dealt with, once per boot
bool g_ntpKicked = false;          // and it has been rebooted once to make that stick
bool g_tvSeen = false;             // it was on the AP on the previous pass
uint32_t g_tvSinceMs = 0;          // when it most recently turned up
constexpr const char* kOurNtpAddress = "192.168.4.1";

// ⚠️ HOW LONG TO WAIT BEFORE CONCLUDING IT IS NOT ASKING US THE TIME.
//
// The television queries NTP shortly after it boots and then rarely. Ten
// minutes of it sitting on our access point without a single request is not
// slowness; it is a device that has never read the setting.
constexpr uint32_t kNtpPatienceMs = 10UL * 60UL * 1000UL;

// --- the big screen's frame ---------------------------------------------------
//
// 240 x 16 rows x 2 bytes. The whole 240x240 frame is 115,200 bytes and CANNOT
// be allocated on this chip -- measured twice -- which is the entire reason the
// renderer works in bands.
constexpr int16_t kBandRows = 16;
render::Color g_band[render::kScreenW * kBandRows];      // 7,680 bytes, static

// ⚠️ THE JPEG BUFFER IS NOT STATIC, and the link error that taught us why is
// worth keeping: `region dram0_0_seg overflowed by 56 bytes`.
//
// dram0_0_seg is the limit on STATICALLY allocated data, and it is far tighter
// than the heap -- about 160 KB against 320 KB of RAM. 48 KB of .bss on top of
// what the WiFi and TLS stacks already claim statically does not fit, and it
// missed by 56 bytes, which is the kind of margin that would have come back at
// the worst moment anyway.
//
// So it is allocated ONCE at boot and checked. That keeps the property that
// actually mattered -- no allocation can fail at alert time, when the device is
// finally needed -- while spending heap, which there is plenty of (240 KB free,
// 110,580-byte largest block).
// MEASURED, not guessed: the largest of the twelve preview frames encodes to
// 22,348 bytes at quality 90, 4:4:4. 32 KB is 43% headroom, and the budget
// encoder steps quality down rather than failing if a busier screen ever needs
// it. The 16 KB saved goes back to the heap, where a TLS handshake wants it.
constexpr size_t kJpegCap = 32 * 1024;
uint8_t* g_jpeg = nullptr;

// --- talking to the world ----------------------------------------------------
#if HAVE_ROOT_CA
const platform::RootCa g_roots[] = { COMMUTE_TV_ROOT_CAS };
#else
const platform::RootCa g_roots[1] = {};
#endif

platform::NetOptions netOptions() {
    platform::NetOptions o;
#if HAVE_ROOT_CA
    o.roots = g_roots;
    o.rootCount = sizeof g_roots / sizeof g_roots[0];
#endif
    // Never true in a build carrying the Google key. See the hard constraints.
    o.allowInsecure = false;
    return o;
}
platform::NetClient g_http(netOptions());
platform::RsaSigner g_signer(secrets::kGooglePrivateKey, secrets::kGoogleKeyLen);
google::Token g_token;

google::Config calendarConfig() {
    google::Config c;
    c.clientEmail = secrets::kGoogleClientEmail;
    c.calendarId = secrets::kGoogleCalendarId;
    return c;
}
google::Client g_calendar(calendarConfig(), g_http, g_signer, g_token);

hafas::Endpoint avvEndpoint() {
    hafas::Endpoint e;
    e.env = secrets::kAvvProd ? hafas::Env::Prod : hafas::Env::Demo;
    e.accessId = secrets::kAvvAccessId;
    return e;
}
hafas::Client g_transit(avvEndpoint(), g_http);

ors::Config orsConfig() {
    ors::Config c;
    c.apiKey = secrets::kOrsApiKey;
    return c;
}
ors::Client g_cycling(orsConfig(), g_http);

meteo::Config meteoConfig() { return meteo::Config{}; }
meteo::Client g_forecast(meteoConfig(), g_http);

// Where the screen goes when no alert is up. Nakul's choice: Time Style 3 --
// NOT the Weather Clock, because the weather panel cannot be right on a device
// with no route to a weather service, and a wrong forecast is worse than none.
//
// The device numbers its themes 1..7 and the names come from its own settings
// page, not from guesswork: 1 Weather Clock Today, 2 Weather Forecast, 3 Photo
// Album, 4 Time Style 1, 5 Time Style 2, 6 Time Style 3, 7 Simple Weather
// Clock. There is no theme 0, which cost an evening to learn.
constexpr int kIdleTheme = 5;

smalltv::Config tvConfig() {
    smalltv::Config c;
    c.uploadField = "file";        // discovered by the probe; still verified at runtime
    c.fallbackTheme = kIdleTheme;
    return c;
}
smalltv::Client g_tv(g_tvLink, tvConfig());

// LiveSources is compiled for the host tests too and must not know what a
// Serial port is, so it takes a plain function pointer and this is it.
void sourceLog(const char* line) { Serial.printf("[src] %s\n", line); }

app::LiveDeps liveDeps() {
    app::LiveDeps d;
    d.calendar = &g_calendar;
    d.transit = &g_transit;
    d.cycling = &g_cycling;
    d.forecast = &g_forecast;
    d.tv = &g_tv;
    d.bandBuf = g_band;
    d.bandRows = kBandRows;
    d.log = sourceLog;
    // jpegBuf is filled in at boot -- see setJpegBuffer below.
    return d;
}
app::LiveSources g_sources(liveDeps());

core::PlaceBook g_places;

app::Config orchestratorConfig() {
    app::Config c;
    c.homeLat = secrets::kHomeLat;
    c.homeLon = secrets::kHomeLon;
    c.alert.leadMin = 30;
    c.alert.safetyBufferMin = 5;
    c.alert.graceMin = 5;
    // Straight from .env, because clause 6.a is a contract and not a default.
    c.transit.minIntervalSec = secrets::kAvvMinIntervalSec;
    c.transit.maxPerHour = static_cast<uint16_t>(secrets::kAvvMaxPerHour);
    c.transit.maxPerDay = static_cast<uint16_t>(secrets::kAvvMaxPerDay);
    c.cycleEnabled = secrets::kOrsApiKey[0] != '\0';
    return c;
}
app::Orchestrator g_app(orchestratorConfig(), g_sources, g_places);

uint32_t g_lastDraw = 0;
uint32_t g_lastTick = 0;
uint32_t g_lastStatus = 0;
uint32_t g_lastConnectedMs = 0;
app::Phase g_lastPhase = app::Phase::NoClock;

// See the note in the previous firmware: a reboot while we hold the television's
// screen would leave our file on a ~1 MB partition with nothing left running
// that remembers to delete it. The guard is the point, not the timer.
constexpr uint32_t kRecoveryRestartMs = 30UL * 60UL * 1000UL;

void showBoot(const char* what) {
    oled::renderBoot(g_canvas, what);
    g_display.flush(g_canvas);
    Serial.printf("[boot] %s\n", what);
}

app::Clock nowClock() {
    app::Clock c;
    c.local = g_clock.now();
    c.utcSec = g_clock.utcSeconds();
    c.sec = static_cast<uint32_t>(c.utcSec);
    c.utcOffsetMin = g_clock.utcOffsetMin();
    c.synced = g_clock.synced();
    return c;
}

} // namespace

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\ncommute-tv");

    if (!g_bus.begin(21, 22)) Serial.println("[oled] Wire.begin failed");
    if (!g_bus.present(g_oledCfg.address)) {
        Serial.printf("[oled] nothing acknowledging at 0x%02X\n", g_oledCfg.address);
    }
    Serial.println(g_display.begin() ? "[oled] ok" : "[oled] init not acknowledged");
    showBoot("starting");

    // --- the frame buffer, once, while failing is still survivable ----------
    g_jpeg = static_cast<uint8_t*>(malloc(kJpegCap));
    if (g_jpeg) {
        g_sources.setJpegBuffer(g_jpeg, kJpegCap);
        Serial.printf("[mem] frame buffer %u bytes, largest block now %u\n",
                      static_cast<unsigned>(kJpegCap),
                      static_cast<unsigned>(ESP.getMaxAllocHeap()));
    } else {
        // Not fatal, and it must not be: the countdown on the OLED is still
        // worth having. The television simply never gets taken over, and the
        // panel says why instead of pretending.
        Serial.println("[mem] ⚠ could not allocate the frame buffer -- the OLED will");
        Serial.println("[mem]   still work; the television will not be used.");
    }

    // --- the places you go ---------------------------------------------------
    {
        struct Seed { const char* label; double lat, lon; };
        static const Seed seeds[] = { COMMUTE_TV_PLACES };
        for (const Seed& s : seeds) g_places.add(s.label, s.lat, s.lon);
        Serial.printf("[app] %d known place(s); anything else is looked up\n",
                      g_places.count());
    }
#if !HAVE_ROOT_CA
    Serial.println("[net] ⚠ no root_ca.h -- every HTTPS request will be REFUSED.");
    Serial.println("[net]   run: python3 tools/fetch_root_ca.py");
#endif

    // --- the radio -----------------------------------------------------------
    Serial.printf("[wifi] station MAC %s\n", WiFi.macAddress().c_str());
    static const platform::WifiNetwork kNetworks[] = { WIFI_NETWORK_LIST };
    constexpr size_t kNetworkCount = sizeof kNetworks / sizeof kNetworks[0];
    showBoot("wifi");
    g_wifi.keepAccessPoint(kWantAp);
    g_wifi.begin(kNetworks, kNetworkCount);

    const uint32_t deadline = millis() + 25000;
    while (!g_wifi.connected() && millis() < deadline) {
        delay(250);
        Serial.print(".");
        g_wifi.pollLog();
        if (g_wifi.allGaveUp() || platform::wifiReasonIsRejection(g_wifi.lastReason())) break;
    }
    Serial.println();
    g_wifi.pollLog();
    if (g_wifi.connected()) {
        Serial.printf("[wifi] up: %s\n", WiFi.localIP().toString().c_str());
        g_wifi.reportLink();
        g_lastConnectedMs = millis();
    } else {
        Serial.printf("[wifi] not connected. reason %u (%s)\n",
                      static_cast<unsigned>(g_wifi.lastReason()),
                      platform::wifiReasonText(g_wifi.lastReason()));
    }

    // --- the private link to the television ----------------------------------
    if (kWantAp) {
        showBoot("tv link");
        if (g_tvLink.beginAp(g_wifi.connected() ? static_cast<int>(WiFi.channel()) : 1)
            && g_wifi.connected()) {
            // Outbound only, if this core can do it at all: the television can
            // then fetch its clock AND weather. See tv_internet.h.
            platform::enableTvInternet(g_tvLink.maxClients());
        }
        // Either way, we can be its clock. This works on every core, and the
        // time comes from a source we can see. Weather still needs the internet.
        g_ntp.begin();
    }

    // --- the clock -----------------------------------------------------------
    showBoot("clock");
    g_clock.begin();
    const uint32_t ntpDeadline = millis() + 15000;
    while (!g_clock.synced() && millis() < ntpDeadline) delay(250);
    Serial.printf("[time] %s, offset %+d min\n", g_clock.synced() ? "synced" : "NOT synced",
                  static_cast<int>(g_clock.utcOffsetMin()));

    Serial.printf("[mem] free heap %u, largest block %u\n",
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMaxAllocHeap()));
    Serial.println("[app] running");
}

void loop() {
    const uint32_t nowMs = millis();
    g_wifi.loop(nowMs);
    g_tvLink.poll();
    g_ntp.poll();

    // --- the television, every time it turns up ------------------------------
    //
    // `found()` goes true once it has answered /v.json and false when it leaves
    // the access point, so this runs at boot and again after every
    // reassociation. That is the right cadence for a PREFERENCE -- assert it
    // when the device appears, then leave it alone, so changing the theme by
    // hand is not a fight with the firmware.
    const bool tvHere = g_tvLink.found();
    if (tvHere && !g_tvSeen) {
        g_tvSeen = true;
        g_tvSinceMs = nowMs;

        // First: rescue a screen stranded on our own picture theme by a run
        // that died holding it.
        if (g_tv.reconcile()) {
            Serial.println("[tv] the screen was left on our theme -- put back");
        }

        // Then: assert the idle theme. reconcile() alone left it on theme 1 for
        // days -- correctly, because nothing had ever stranded it on ours and
        // it therefore had nothing to rescue. "Which theme should it show when
        // nothing is happening" is a different question, and this is it.
        if (g_tv.enforceTheme(kIdleTheme)) {
            Serial.printf("[tv] put it on theme %d\n", kIdleTheme);
        } else if (g_tv.lastError() != smalltv::Error::None) {
            Serial.printf("[tv] could not set theme %d: %s\n", kIdleTheme,
                          smalltv::errorText(g_tv.lastError()));
        }

        // And once per boot of OURS: point its clock at us.
        if (!g_tvTimeSet) {
            g_tvTimeSet = true;

            // Read before writing, so the reboot below happens once in the
            // device's life rather than on every boot of ours.
            char current[48] = {0};
            const bool read = g_tv.readNtpServer(current, sizeof current);
            Serial.printf("[tv] its NTP server is currently \"%s\"%s\n", current,
                          read ? "" : "  (could not read it)");

            if (read && std::strcmp(current, kOurNtpAddress) == 0) {
                Serial.println("[tv] already pointed at us -- nothing to write");
            } else if (g_tv.setNtpServer(kOurNtpAddress)) {
                Serial.printf("[tv] set its NTP server to %s\n", kOurNtpAddress);
                // ⚠️ It reads that setting at STARTUP, so the change does
                // nothing until it restarts.
                if (g_tv.rebootDevice()) {
                    g_ntpKicked = true;
                    Serial.println("[tv] rebooting it once so the change takes effect");
                } else {
                    Serial.printf("[tv] could not reboot it: %s -- power-cycle it by hand\n",
                                  smalltv::errorText(g_tv.lastError()));
                }
            } else {
                Serial.printf("[tv] could not set its NTP server: %s\n",
                              smalltv::errorText(g_tv.lastError()));
            }
        }
    }
    if (!tvHere) g_tvSeen = false;

    // ⚠️ THE STANDOFF THIS BREAKS, and it was a hole in the paragraph above.
    //
    // Reading before writing was meant to stop us rebooting the television on
    // every boot of ours. It did that, and one thing more: once the value had
    // been written by a version of this code that never rebooted it, every
    // later run read back "192.168.4.1", concluded there was nothing to do, and
    // left a device that had NEVER READ THE SETTING. `ntp served` sat at 0 for
    // two days and Nakul power-cycled it by hand.
    //
    // The setting is not the evidence. The requests are. If it is pointed at us
    // and has still asked us nothing after ten minutes on our access point,
    // then the setting is not in force, and a reboot is the only thing that
    // puts it there. Once per boot of ours, and never mid-alert -- a blank
    // screen where the countdown should be is a worse bug than a wrong clock.
    if (!g_ntpKicked && g_tvSeen && g_ntp.running() && g_ntp.served() == 0 &&
        !g_app.mayHoldScreen() && nowMs - g_tvSinceMs > kNtpPatienceMs) {
        g_ntpKicked = true;
        Serial.println("[tv] it is pointed at us and has asked nothing in 10 min --");
        Serial.println("[tv]   the setting cannot be in force. Rebooting it once.");
        if (!g_tv.rebootDevice()) {
            Serial.printf("[tv] could not reboot it: %s -- power-cycle it by hand\n",
                          smalltv::errorText(g_tv.lastError()));
        }
    }

    if (g_wifi.connected()) {
        g_lastConnectedMs = nowMs;
    } else if (!g_app.mayHoldScreen() && nowMs - g_lastConnectedMs > kRecoveryRestartMs) {
        Serial.println("[boot] no link for 30 min -- restarting to clear it");
        Serial.flush();
        ESP.restart();
    }

    // Once a second. Everything inside decides for itself what is actually due,
    // and the budgets decide whether it is allowed.
    if (nowMs - g_lastTick >= 1000) {
        g_lastTick = nowMs;
        if (g_wifi.connected() || g_app.mayHoldScreen()) {
            g_app.tick(nowClock());
            // On every phase change, and otherwise every half minute. The
            // previous version printed only on change, which meant a board
            // sitting in one phase for an hour said nothing at all about why.
            const bool changed = g_app.phase() != g_lastPhase;
            if (changed || nowMs - g_lastStatus >= 30000) {
                g_lastPhase = g_app.phase();
                g_lastStatus = nowMs;

                Serial.printf("[app] %-9s %s", app::phaseName(g_lastPhase),
                              g_app.note());
                if (g_app.haveEvent()) {
                    char at[8];
                    core::formatHHMM(g_app.event().start, at);
                    Serial.printf("\n[app]   next %s at %s, leave in %ld min", at,
                                  g_app.event().location,
                                  static_cast<long>(g_app.alert().minutesToLeave));
                }
                if (g_app.fault() != app::Fault::None) {
                    Serial.printf("\n[app]   %s: %s", app::faultName(g_app.fault()),
                                  g_app.faultText());
                }
                // Whether the television is ASKING is the thing that tells
                // "it never asked" apart from "we answered wrongly", and no
                // amount of staring at its screen distinguishes those.
                Serial.printf("\n[app]   ntp served %u, refused %u, ignored %u",
                              static_cast<unsigned>(g_ntp.served()),
                              static_cast<unsigned>(g_ntp.refused()),
                              static_cast<unsigned>(g_ntp.ignored()));
                Serial.printf("\n[app]   avv %u/%u this hour, heap %u\n",
                              static_cast<unsigned>(g_app.transitBudget().usedLastHour(
                                  static_cast<uint32_t>(g_clock.utcSeconds()))),
                              static_cast<unsigned>(
                                  g_app.transitBudget().config().maxPerHour),
                              static_cast<unsigned>(ESP.getFreeHeap()));
            }
        }
    }

    if (nowMs - g_lastDraw < 1000) {
        delay(10);
        return;
    }
    g_lastDraw = nowMs;

    oled::HealthModel m;
    g_app.fillHealth(m);            // everything except the radio
    m.wifiUp = g_wifi.connected();
    core::copyStr(m.wifiName, sizeof m.wifiName, g_wifi.ssid(),
                  std::strlen(g_wifi.ssid()));
    m.rssiDbm = static_cast<int16_t>(g_wifi.rssi());
    if (!m.wifiUp && m.errorText[0] == '\0') {
        core::copyStr(m.errorText, sizeof m.errorText, g_wifi.failureText(),
                      std::strlen(g_wifi.failureText()));
    }

    oled::renderHealth(g_canvas, m);
    if (!g_display.flush(g_canvas)) {
        Serial.printf("[oled] flush failed (%u)\n",
                      static_cast<unsigned>(g_display.failedWrites()));
    }
}
