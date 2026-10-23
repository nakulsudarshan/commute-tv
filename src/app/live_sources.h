// live_sources.h - Sources, implemented with the real clients.
//
// The orchestrator decides WHEN to ask and what to do with the answer. This
// decides HOW to ask. It is deliberately thin and deliberately dull: every line
// here is either building a query out of what the orchestrator gave us, or
// turning one client's status enum into the two facts the orchestrator acts on
// -- did it work, and did the far end refuse us.
//
// That translation is the only judgement in the file, and it is worth stating:
//
//   * A refusal (a rejected key, a spent quota, an AVV denial) sets
//     Outcome::refused, which the orchestrator turns into a long backoff.
//     Retrying a refusal quickly is how a key gets banned.
//   * An EMPTY ANSWER IS NOT A FAILURE. "There is no way to cycle there" and
//     "AVV has no connection for that" are answers; reporting them as errors
//     would make the panel cry wolf and would make the orchestrator retry
//     something that will never change.
//
// It holds no state of its own beyond the scratch buffers the frame needs.
#pragma once
#include <cstddef>
#include <cstdint>

#include "../hafas/hafas_client.h"
#include "../jpeg/jpeg_encoder.h"
#include "../ors/ors_client.h"
#include "../render/band_renderer.h"
#include "../smalltv/smalltv_client.h"
#include "../weather/meteo_client.h"
#include "orchestrator.h"

namespace app {

// Everything LiveSources needs, gathered in one place so the firmware's wiring
// is a single struct literal rather than a nine-argument constructor.
struct LiveDeps {
    google::Client* calendar = nullptr;
    hafas::Client*  transit  = nullptr;
    ors::Client*    cycling  = nullptr;
    meteo::Client*  forecast = nullptr;
    smalltv::Client* tv      = nullptr;

    // Scratch for turning a ScreenModel into a JPEG. The 240x240 frame is
    // 115,200 bytes and CANNOT be allocated on this chip -- measured, twice, on
    // the real board -- so the renderer works in horizontal bands and this is
    // the band, not the frame.
    render::Color* bandBuf = nullptr;
    int16_t bandRows = 0;
    uint8_t* jpegBuf = nullptr;
    size_t jpegCap = 0;

    // Optional. Called with one formatted line whenever a source call fails or
    // returns something surprising.
    //
    // It is a function pointer rather than anything cleverer because this file
    // is compiled for the host tests too and must not learn what a Serial port
    // is. Without it, a failure reaches the panel as a 24-character summary --
    // "ors http" -- which is exactly enough to know something broke and not
    // nearly enough to know what.
    void (*log)(const char* line) = nullptr;

    // The SmallTV's image partition is ~1 MB. Encoding is retried at lower
    // quality rather than uploading something that will not fit.
    size_t maxJpegBytes = 30 * 1024;   // measured max frame: 22,348 bytes
    int jpegQuality = 90;
    int jpegMinQuality = 60;
};

class LiveSources final : public Sources {
public:
    explicit LiveSources(const LiveDeps& deps) : d_(deps) {}

    Outcome events(const Clock& now, int32_t lookaheadMin,
                   google::EventList& out) override;
    Outcome resolve(const char* text, double& lat, double& lon) override;
    Outcome trip(const TripRequest& q, core::Itinerary* out, int cap,
                 int& count) override;
    Outcome cycle(const TripRequest& q, core::CycleRoute& out) override;
    Outcome weather(double lat, double lon, core::Timestamp at,
                    core::Weather& out) override;
    Outcome pingScreen(ScreenState& out) override;
    Outcome showScreen(const render::ScreenModel& m) override;
    Outcome releaseScreen() override;

    // The JPEG buffer is handed over AFTER construction, because on the ESP32 it
    // is allocated at boot rather than declared static: 48 KB of .bss overflows
    // dram0_0_seg, which is a hard limit on STATIC data and much tighter than
    // the heap. Allocating once at boot and refusing to arm if it fails keeps
    // the property that mattered -- no allocation can fail at alert time.
    void setJpegBuffer(uint8_t* buf, size_t cap) {
        d_.jpegBuf = buf;
        d_.jpegCap = cap;
    }
    bool ready() const { return d_.jpegBuf != nullptr && d_.bandBuf != nullptr; }

    // How big the last frame encoded to, and at what quality it had to settle.
    size_t lastJpegBytes() const { return lastJpegBytes_; }
    int lastJpegQuality() const { return lastJpegQuality_; }

private:
    // One line about something that went wrong, with the HTTP status attached.
    void note(const char* what, int http, const char* detail);

    LiveDeps d_;

    // ⚠️ NOT A STACK LOCAL. hafas::TripResult is 5,968 bytes -- six itineraries
    // of six legs, each leg carrying three 40-character names. The Arduino loop
    // task on ESP32 gets an 8,192-byte stack, and this used to sit on it at the
    // same moment the orchestrator's own six-itinerary buffer did. That is
    // 11,776 bytes of the 8,192 available, and the board rebooted with
    // `Double exception` on the first tick, every tick.
    //
    // Being a member costs the same bytes in .bss, where there is room, and
    // costs them once rather than per call.
    hafas::TripResult tripScratch_;

    size_t lastJpegBytes_ = 0;
    int lastJpegQuality_ = 0;
};

} // namespace app
