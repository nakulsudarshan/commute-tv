// smalltv_client.h - driving the SmallTV Ultra, over an abstract transport.
//
// The transport is an interface rather than an HTTPClient because the
// interesting logic here is sequencing, not sockets: take the screen, put it
// back, discover the upload field, notice the device has gone away. All of
// that is testable against a mock, and none of it needs hardware.
//
// The contract with the device is deliberately narrow and reversible. We
// record its current theme before touching anything and restore that exact
// theme afterwards, so the worst case for the user is that the SmallTV shows
// its own clock -- never a device left stuck on a stale commute screen.
#pragma once
#include <cstddef>
#include <cstdint>

#include "smalltv_request.h"

namespace smalltv {

// One piece of a request body. Bodies are scatter-gathered so the JPEG is
// never copied to prepend a multipart header to it.
struct Slice {
    const uint8_t* data = nullptr;
    size_t len = 0;
};

enum class Transport : uint8_t {
    Ok,           // a response arrived, whatever its status
    Unreachable,  // no route, refused, DNS -- the device is not there
    Timeout,
    Failed,       // transport-level failure that is none of the above
};

struct HttpResponse {
    Transport transport = Transport::Unreachable;
    int status = 0;
    char body[384] = {0};   // truncated; the JSON we read is tens of bytes
    size_t bodyLen = 0;
    bool truncated = false;

    bool ok() const {
        return transport == Transport::Ok && status >= 200 && status < 300;
    }
};

struct HttpTransport {
    virtual ~HttpTransport() = default;
    virtual HttpResponse get(const char* path) = 0;
    virtual HttpResponse post(const char* path, const char* contentType,
                              const Slice* parts, size_t partCount) = 0;
};

struct Config {
    // The picture-mode theme index. The plan assumes 3; the probe confirms it.
    // A config value rather than a constant precisely because it is an
    // assumption -- being wrong here should cost a settings change, not a
    // reflash.
    int pictureTheme = 3;
    // ⚠️ WHERE TO PUT THE SCREEN BACK when we could not read where it was.
    //
    // This used to fall back to 0, which is NOT A THEME -- the device numbers
    // them 1..7 (1 Weather Clock Today, 2 Forecast, 3 Photo Album, 4-6 Time
    // Styles, 7 Simple Weather Clock). Setting 0 left it on Photo Album with an
    // album we had just emptied, so the screen read "No images." over the last
    // commute frame until someone touched it by hand. Seen on the real device.
    //
    // 1 is what Nakul's idles on, and any sane fallback beats an invalid one.
    int fallbackTheme = 1;

    const char* imageDir = "/image/";
    const char* imageName = "commute.jpg";

    // Leave null to discover the /doUpload field name on first use. Set it
    // once the probe has told us, to skip the discovery round trips.
    const char* uploadField = nullptr;

    // Which spelling /delete?file= wants. Discovered the same way as the upload
    // field, for the same reason: the evidence is a web page's onclick handler,
    // not documentation, and a wrong guess fails SILENTLY -- the device answers
    // HTTP 200 with the body "Fail".
    smalltv::DeleteStyle deleteStyle = smalltv::DeleteStyle::DoubledSlash;
    bool discoverDeleteStyle = true;

    // Attempts per upload before giving up and reporting to the health panel.
    int uploadAttempts = 2;

    // Delete our image from the device when the screen is handed back.
    //
    // This is not tidiness. The SmallTV keeps every uploaded image in its
    // LittleFS partition permanently -- Nakul found the probe's test image
    // still sitting there days later -- and the device has 4 MB of flash
    // total. At ~20 KB a frame and one frame a minute through a 35-minute
    // alert, an alert that left its frames behind would cost ~700 KB. A few
    // days of commuting would fill the partition, and a full filesystem on a
    // device we have promised never to reflash is not a state to be in.
    bool deleteOnRelease = true;

    // Refuse to upload when the device has less free space than this.
    //
    // The image area is about 1 MB (measured: 943 KB free with one 21 KB image
    // on it), and a frame is ~20 KB. If the deletes are silently failing, this
    // is what stops the firmware from being the thing that fills the last of
    // it. Zero disables the check.
    uint32_t minFreeBytes = 64 * 1024;

    // Turn photo-album autoplay off while we hold the screen, and put it back
    // afterwards. Without this, a device with autoplay enabled replaces our
    // frame with the next stored image after a few seconds -- the alert would
    // flash up once a minute and then vanish, which reads as a broken upload
    // rather than a setting.
    bool suppressAutoplay = true;

    // Delete before each upload as well.
    //
    // Only needed if the firmware appends rather than overwriting when the
    // same filename is uploaded twice. Uploading to a fixed name SHOULD
    // overwrite, which would make the release-time delete sufficient; until
    // the probe confirms it on the real device this stays off, because it
    // doubles the requests per frame.
    bool deleteBeforeUpload = false;
};

enum class Error : uint8_t {
    None,
    Unreachable,      // device not answering at all
    HttpStatus,       // it answered, and said no
    UploadRejected,   // no candidate field name was accepted
    ThemeUnknown,     // /app.json did not tell us what to restore
    BadResponse,      // it answered with something we could not read
    DeviceFull,       // too little free space to upload another frame
    BadArgument,
};

const char* errorText(Error e);

class Client {
public:
    Client(HttpTransport& transport, const Config& cfg)
        : http_(transport), cfg_(cfg), field_(cfg.uploadField),
          deleteStyle_(cfg.deleteStyle) {}

    // --- primitives --------------------------------------------------------
    bool ping();                                  // GET /v.json
    bool readTheme(int& theme);                   // GET /app.json
    bool setTheme(int theme);                     // GET /set?theme=
    bool showImage(const char* name);             // GET /set?img=
    bool uploadImage(const char* name, const uint8_t* jpegData, size_t len);
    // Delete, and CHECK. A 200 means nothing here -- the only trustworthy
    // answer is that the name is gone from /filelist. If the configured
    // spelling does not work, the others are tried and the one that does is
    // remembered.
    bool deleteImage(const char* name);

    // GET /space.json. Returns false if the device did not answer with a
    // usable figure -- which is NOT the same as zero free space.
    bool readFreeSpace(uint32_t& freeBytes);

    // Photo-album autoplay, which cycles stored images on a timer.
    bool readAlbum(int& autoplay, int& intervalSec);
    bool setAutoplay(int autoplay, int intervalSec);

    // True when autoplay was found enabled and has been turned off for the
    // duration of the alert. Worth showing: it is a change to the user's
    // settings, and it is restored on release.
    bool autoplaySuppressed() const { return savedAutoplay_ > 0; }

    // The last figure read, and whether one has been read at all. Health-panel
    // material: a number that only goes down means the deletes are not working.
    uint32_t lastFreeBytes() const { return lastFreeBytes_; }
    bool freeSpaceKnown() const { return freeSpaceKnown_; }

    // Which spelling actually worked, once one has.
    DeleteStyle deleteStyle() const { return deleteStyle_; }
    bool deleteStyleKnown() const { return deleteStyleKnown_; }

    // --- the two operations the orchestrator actually calls -----------------

    // Records the current theme (once, on the first take), uploads the frame
    // and switches the screen to it. Safe to call repeatedly while armed: the
    // saved theme is captured only on the transition, so a mid-alert refresh
    // cannot overwrite it with our own picture theme -- which would otherwise
    // leave the device stuck showing a commute screen forever.
    // Put the screen back if it was left on OUR picture theme by something
    // that is no longer running -- a reboot mid-alert, a power cut, a firmware
    // update. Without this the television sits on Photo Album with an album we
    // emptied, reading "No images.", until a human touches it.
    //
    // Safe to call at boot and cheap: one /app.json, and it does nothing at all
    // unless the device is on our theme.
    bool reconcile();

    // Put the device on `theme` and leave it there.
    //
    // reconcile() only RESCUES a screen stranded on our own picture theme; a
    // device sitting happily on the Weather Clock is none of its business, and
    // so a device that has never shown a commute alert never moves. This is the
    // other half: it asserts what the screen should be showing when nothing is
    // going on, which is the only way `Config::fallbackTheme` ever becomes true
    // of a device rather than merely being where we put it back to.
    //
    // Refuses while we are holding the screen -- an alert outranks a
    // preference. Returns true ONLY when it actually changed the theme; already
    // being right is not a failure and leaves lastError() == None.
    bool enforceTheme(int theme);

    // Point the device's clock at an NTP server -- ours. `/set?ntp=<host>` is
    // the endpoint behind the "NTP Server Domain" field in its own web UI, read
    // out of the saved copy of that UI rather than guessed at.
    bool setNtpServer(const char* host);

    // What it currently thinks its NTP server is. Reading before writing means
    // we only ever reboot the device when the value actually changes.
    bool readNtpServer(char* out, size_t outCap);

    // /set?reboot=1. The device re-reads its NTP server at startup, so a change
    // does not take effect until it restarts. Used ONCE, and only after a value
    // really changed -- rebooting a display on every boot would be its own bug.
    bool rebootDevice();

    bool takeScreen(const uint8_t* jpegData, size_t len);

    // Puts back exactly the theme that was there before. Idempotent.
    bool releaseScreen();

    // --- state -------------------------------------------------------------
    Error lastError() const { return err_; }
    int lastStatus() const { return status_; }
    const char* uploadField() const { return field_; }
    bool holdsScreen() const { return holdsScreen_; }

    // True when a frame of ours is still stored on the device. The health panel
    // should show this: a device that keeps refusing deletes will fill up, and
    // it will do so silently.
    bool imageOnDevice() const { return imageOnDevice_; }

    // How many delete attempts have failed since boot. Non-zero is a reason to
    // look at the device's file list.
    uint32_t failedDeletes() const { return failedDeletes_; }

    // How many deletes answered "Fail" and removed the file anyway. Purely a
    // curiosity counter: on V9.0.40 this equals the number of successful
    // deletes, because that firmware always says Fail.
    uint32_t lyingDeletes() const { return lyingDeletes_; }
    int savedTheme() const { return savedTheme_; }

    // Only for tests and for restoring persisted state across a reboot.
    void restoreState(bool holdsScreen, int savedTheme, int savedAutoplay = -1,
                      int savedInterval = -1) {
        imageOnDevice_ = holdsScreen;
        savedAutoplay_ = savedAutoplay;
        savedInterval_ = savedInterval;
        holdsScreen_ = holdsScreen;
        savedTheme_ = savedTheme;
    }

private:
    bool deleteOnce(const char* name, DeleteStyle style);
    bool stillListed(const char* name);
    bool uploadWithField(const char* field, const char* name,
                         const uint8_t* jpegData, size_t len);
    bool note(const HttpResponse& r);

    HttpTransport& http_;
    Config cfg_;
    const char* field_ = nullptr;
    Error err_ = Error::None;
    int status_ = 0;
    bool holdsScreen_ = false;
    bool imageOnDevice_ = false;
    uint32_t failedDeletes_ = 0;
    uint32_t lyingDeletes_ = 0;
    DeleteStyle deleteStyle_;
    bool deleteStyleKnown_ = false;
    uint32_t lastFreeBytes_ = 0;
    bool freeSpaceKnown_ = false;

    // Saved on the transition into holding the screen, like savedTheme_, and
    // for the same reason: re-reading mid-alert would read back our own
    // suppression and destroy the only record of what to restore.
    int savedAutoplay_ = -1;
    int savedInterval_ = -1;
    int savedTheme_ = -1;
    uint32_t nonce_ = 0x9E3779B9u;
};

// ---------------------------------------------------------------------------

// Suppresses uploads of frames identical to the last one that went through.
//
// This is a flash-wear measure, not an optimisation. An ARMED alert re-renders
// every 60 s for 35 minutes; without the gate that is ~35 LittleFS writes per
// appointment, and with it, three or four -- because the countdown only
// actually changes the pixels once a minute and most refreshes change nothing
// at all.
//
// It hashes the canvas rather than the encoded JPEG so an unchanged frame
// skips the encode as well as the upload.
class FrameGate {
public:
    // `variant` folds in anything that changes the encoding without changing
    // the pixels (quality, chroma sampling), so a settings change is not
    // mistaken for "no change".
    bool shouldUpload(const uint16_t* px, size_t count, uint32_t variant = 0);

    void reset() { has_ = false; skipped_ = 0; }
    uint64_t lastHash() const { return hash_; }
    uint32_t skipped() const { return skipped_; }

private:
    uint64_t hash_ = 0;
    uint32_t skipped_ = 0;
    bool has_ = false;
};

uint64_t frameHash(const uint16_t* px, size_t count, uint32_t variant);

} // namespace smalltv
