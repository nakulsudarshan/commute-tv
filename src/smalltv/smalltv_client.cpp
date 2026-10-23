#include "smalltv_client.h"

#include <cstdio>
#include <cstring>

namespace smalltv {

const char* errorText(Error e) {
    switch (e) {
        case Error::None:           return "ok";
        case Error::Unreachable:    return "no reply";     // fits the OLED line
        case Error::HttpStatus:     return "http";
        case Error::UploadRejected: return "upload";
        case Error::ThemeUnknown:   return "theme?";
        case Error::BadResponse:    return "bad reply";
        case Error::DeviceFull:     return "tv full";      // fits the OLED line
        case Error::BadArgument:    return "bad arg";
    }
    return "?";
}

bool Client::note(const HttpResponse& r) {
    status_ = r.status;
    if (r.transport != Transport::Ok) { err_ = Error::Unreachable; return false; }
    if (!r.ok()) { err_ = Error::HttpStatus; return false; }
    err_ = Error::None;
    return true;
}

bool Client::ping() {
    return note(http_.get(kVersionPath));
}

bool Client::readTheme(int& theme) {
    const HttpResponse r = http_.get(kAppJsonPath);
    if (!note(r)) return false;
    if (!parseThemeFromAppJson(r.body, r.bodyLen, theme)) {
        err_ = Error::ThemeUnknown;
        return false;
    }
    return true;
}

bool Client::setTheme(int theme) {
    char path[kMaxPath];
    if (buildSetThemePath(theme, path, sizeof path) < 0) {
        err_ = Error::BadArgument;
        return false;
    }
    return note(http_.get(path));
}

bool Client::showImage(const char* name) {
    char path[kMaxPath];
    if (buildSetImagePath(cfg_.imageDir, name, path, sizeof path) < 0) {
        err_ = Error::BadArgument;
        return false;
    }
    return note(http_.get(path));
}

bool Client::readAlbum(int& autoplay, int& intervalSec) {
    char path[kMaxPath];
    if (buildAlbumPath(path, sizeof path) < 0) { err_ = Error::BadArgument; return false; }
    const HttpResponse r = http_.get(path);
    if (!note(r)) return false;
    if (!parseAlbum(r.body, r.bodyLen, autoplay, intervalSec)) {
        err_ = Error::BadResponse;
        return false;
    }
    return true;
}

bool Client::setAutoplay(int autoplay, int intervalSec) {
    char path[kMaxPath];
    if (buildSetAlbumPath(intervalSec, autoplay, path, sizeof path) < 0) {
        err_ = Error::BadArgument;
        return false;
    }
    return note(http_.get(path));
}

bool Client::readFreeSpace(uint32_t& freeBytes) {
    char path[kMaxPath];
    if (buildSpacePath(path, sizeof path) < 0) { err_ = Error::BadArgument; return false; }
    const HttpResponse r = http_.get(path);
    if (!note(r)) return false;
    uint32_t v = 0;
    if (!parseFreeBytes(r.body, r.bodyLen, v)) { err_ = Error::BadResponse; return false; }
    freeBytes = v;
    lastFreeBytes_ = v;
    freeSpaceKnown_ = true;
    return true;
}

// Ask the device what it actually has. The status of a delete is not evidence:
// V9.0.40 answers a delete it did not perform with HTTP 200 and the body
// "Fail", so a client that trusted the status would report success while the
// image partition filled up.
bool Client::stillListed(const char* name) {
    char path[kMaxPath];
    if (buildFileListPath(cfg_.imageDir, path, sizeof path) < 0) return false;
    const HttpResponse r = http_.get(path);
    if (!r.ok()) return false;          // cannot tell; do not claim it is there
    return fileListMentions(r.body, r.bodyLen, name);
}

bool Client::deleteOnce(const char* name, DeleteStyle style) {
    char path[kMaxPath];
    if (buildDeletePath(cfg_.imageDir, name, style, path, sizeof path) < 0) {
        err_ = Error::BadArgument;
        return false;
    }
    const HttpResponse r = http_.get(path);
    if (!note(r)) return false;

    // The body is NOT consulted. V9.0.40 answers "Fail" whether or not it
    // deleted the file -- measured both ways, minutes apart. An earlier version
    // of this function treated that body as a refusal, which would have made
    // every successful delete look like a failure, retried all five spellings
    // each time, and left imageOnDevice() permanently true.
    const bool said = bodySaysFailure(r.body, r.bodyLen);
    const bool gone = !stillListed(name);
    if (gone && said) ++lyingDeletes_;
    return gone;
}

bool Client::deleteImage(const char* name) {
    if (deleteOnce(name, deleteStyle_)) {
        deleteStyleKnown_ = true;
        return true;
    }
    if (!cfg_.discoverDeleteStyle) return false;

    // The remembered spelling stopped working, or was never right. Try the
    // others once each -- the same pattern as discovering the upload field
    // name, and for the same reason.
    for (int i = 0; i < kDeleteStyleCount; ++i) {
        const DeleteStyle s = static_cast<DeleteStyle>(i);
        if (s == deleteStyle_) continue;
        if (deleteOnce(name, s)) {
            deleteStyle_ = s;
            deleteStyleKnown_ = true;
            return true;
        }
    }
    return false;
}

bool Client::uploadWithField(const char* field, const char* name,
                             const uint8_t* jpegData, size_t len) {
    char path[kMaxPath];
    if (buildUploadPath(cfg_.imageDir, path, sizeof path) < 0) {
        err_ = Error::BadArgument;
        return false;
    }

    // Re-roll until the boundary provably does not occur inside the image.
    // Bounded, because after a few tries the cause is not chance.
    char boundary[48];
    bool haveBoundary = false;
    for (int i = 0; i < 4; ++i) {
        nonce_ = nonce_ * 1664525u + 1013904223u;
        if (!makeBoundary(nonce_, boundary, sizeof boundary)) break;
        if (boundaryIsSafe(boundary, jpegData, len)) { haveBoundary = true; break; }
    }
    if (!haveBoundary) { err_ = Error::BadArgument; return false; }

    MultipartFrame frame;
    if (!buildMultipart(boundary, field, name, "image/jpeg", frame)) {
        err_ = Error::BadArgument;
        return false;
    }

    const Slice parts[3] = {
        {reinterpret_cast<const uint8_t*>(frame.header), frame.headerLen},
        {jpegData, len},
        {reinterpret_cast<const uint8_t*>(frame.trailer), frame.trailerLen},
    };
    return note(http_.post(path, frame.contentType, parts, 3));
}

bool Client::uploadImage(const char* name, const uint8_t* jpegData, size_t len) {
    if (name == nullptr || jpegData == nullptr || len == 0) {
        err_ = Error::BadArgument;
        return false;
    }

    const int attempts = cfg_.uploadAttempts > 0 ? cfg_.uploadAttempts : 1;

    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (field_ != nullptr) {
            if (uploadWithField(field_, name, jpegData, len)) return true;
            // A field name that worked once and now returns 4xx means the
            // device changed under us (firmware update, factory reset), so
            // fall back to rediscovery rather than retrying a dead guess.
            if (err_ == Error::HttpStatus && status_ >= 400 && status_ < 500)
                field_ = nullptr;
            else
                continue;
        }

        for (size_t i = 0; i < kUploadFieldCandidateCount; ++i) {
            if (uploadWithField(kUploadFieldCandidates[i], name, jpegData, len)) {
                field_ = kUploadFieldCandidates[i];
                return true;
            }
            // No point walking the rest of the list if nothing is listening.
            if (err_ == Error::Unreachable) break;
        }
        // Only a device that answered and refused every candidate is an
        // "upload rejected". Unreachable and BadArgument say something else
        // and must not be flattened into it -- the health panel is only
        // useful if the reason it shows is the real one.
        if (err_ == Error::HttpStatus) err_ = Error::UploadRejected;
    }
    return false;
}

bool Client::setNtpServer(const char* host) {
    if (!host || !host[0]) { err_ = Error::BadArgument; return false; }
    char path[96];
    // The device's own UI does no encoding beyond stripping spaces, so an IP
    // literal -- which is all we ever send -- needs none either.
    if (std::snprintf(path, sizeof path, "/set?ntp=%s", host) < 0) {
        err_ = Error::BadArgument;
        return false;
    }
    return note(http_.get(path));
}

bool Client::readNtpServer(char* out, size_t outCap) {
    if (!out || outCap == 0) return false;
    out[0] = '\0';
    const HttpResponse r = http_.get("/ntp.json");
    if (!note(r)) return false;
    if (!parseJsonString(r.body, r.bodyLen, "ntp", out, outCap)) {
        err_ = Error::BadResponse;
        return false;
    }
    return true;
}

bool Client::rebootDevice() {
    return note(http_.get("/set?reboot=1"));
}

bool Client::reconcile() {
    if (holdsScreen_) return true;         // this run owns it; nothing to recover

    int theme = -1;
    if (!readTheme(theme)) {
        err_ = Error::ThemeUnknown;
        return false;
    }
    if (theme != cfg_.pictureTheme) {
        err_ = Error::None;
        return false;                      // not ours, not our business
    }

    // It IS on our theme and we are not holding it, so whatever put it there is
    // gone. There is no saved theme to go back to -- that died with the process
    // that took the screen -- so the fallback is the whole answer.
    int target = cfg_.fallbackTheme;
    if (target <= 0 || target == cfg_.pictureTheme) target = 1;
    if (!setTheme(target)) return false;

    // And clear the leftover frame. A failed delete is housekeeping, not a
    // failed recovery: the screen is already back.
    if (deleteImage(cfg_.imageName)) imageOnDevice_ = false;
    else ++failedDeletes_;
    err_ = Error::None;
    return true;
}

bool Client::enforceTheme(int theme) {
    // The alert owns the screen. A preference that fought it would blank the
    // one thing the whole device exists to show.
    if (holdsScreen_) { err_ = Error::None; return false; }
    if (theme <= 0) { err_ = Error::BadArgument; return false; }

    int current = -1;
    if (!readTheme(current)) {
        err_ = Error::ThemeUnknown;
        return false;
    }
    if (current == theme) { err_ = Error::None; return false; }   // nothing to do

    const bool wasOnOurPicture = current == cfg_.pictureTheme;
    if (!setTheme(theme)) return false;

    // If it was parked on Photo Album showing our leftover frame, take the file
    // with us. A failed delete is housekeeping, not a failed enforcement: the
    // screen is already where it should be.
    if (wasOnOurPicture && imageOnDevice_) {
        if (deleteImage(cfg_.imageName)) imageOnDevice_ = false;
        else ++failedDeletes_;
    }
    err_ = Error::None;
    return true;
}

bool Client::takeScreen(const uint8_t* jpegData, size_t len) {
    // Capture the theme only on the transition into holding the screen.
    // Reading it again mid-alert would read back our own picture theme and
    // destroy the only record of what to restore.
    if (!holdsScreen_) {
        int theme = -1;
        if (readTheme(theme)) savedTheme_ = theme;
        // A device that will not say what theme it is on is not a reason to
        // refuse the alert -- the alert is the point. We just cannot promise a
        // faithful restore, and releaseScreen() says so by falling back.

        // Same rule for autoplay: read once, on the way in. A device that will
        // not answer simply does not get its autoplay touched.
        if (cfg_.suppressAutoplay) {
            int autoplay = 0, interval = 5;
            if (readAlbum(autoplay, interval) && autoplay != 0) {
                if (setAutoplay(0, interval)) {
                    savedAutoplay_ = autoplay;
                    savedInterval_ = interval;
                }
            }
            err_ = Error::None;
        }
    }

    // Only if the firmware turns out to append rather than overwrite; see
    // Config::deleteBeforeUpload. A failed delete here is not a reason to skip
    // the frame -- showing the commute is the point.
    if (cfg_.deleteBeforeUpload && imageOnDevice_) {
        if (!deleteImage(cfg_.imageName)) ++failedDeletes_;
        err_ = Error::None;
    }

    // Do not be the thing that fills the last of a 1 MB filesystem. Checked
    // only when we are not already holding the screen: mid-alert we are
    // overwriting our own file, and a check every minute would be pure traffic.
    if (cfg_.minFreeBytes > 0 && !holdsScreen_) {
        uint32_t freeBytes = 0;
        if (readFreeSpace(freeBytes) && freeBytes < cfg_.minFreeBytes) {
            err_ = Error::DeviceFull;
            return false;
        }
        err_ = Error::None;   // an unanswered /space.json is not a refusal
    }

    if (!uploadImage(cfg_.imageName, jpegData, len)) return false;
    imageOnDevice_ = true;
    if (!setTheme(cfg_.pictureTheme)) return false;
    if (!showImage(cfg_.imageName)) return false;

    holdsScreen_ = true;
    return true;
}

bool Client::releaseScreen() {
    if (!holdsScreen_) { err_ = Error::None; return true; }

    // Never restore our own picture theme: that would hand the screen back
    // still showing the commute frame, which is exactly the state this
    // function exists to prevent. And never 0, which is not a theme at all --
    // see Config::fallbackTheme for what that cost.
    int target = savedTheme_;
    if (target <= 0 || target == cfg_.pictureTheme) target = cfg_.fallbackTheme;
    if (target <= 0) target = 1;

    if (!setTheme(target)) return false;
    holdsScreen_ = false;
    savedTheme_ = -1;

    // Put autoplay back exactly as it was. Doing this before the delete keeps
    // the user's own settings the first thing restored.
    if (savedAutoplay_ > 0) {
        if (setAutoplay(savedAutoplay_, savedInterval_ > 0 ? savedInterval_ : 5)) {
            savedAutoplay_ = -1;
            savedInterval_ = -1;
        }
        err_ = Error::None;
    }

    // Delete only AFTER the theme is back: removing a file the device is
    // currently displaying is not something the stock firmware promises to
    // handle, and the visible half of the release has already succeeded.
    //
    // A failed delete does not fail the release either. The screen has been
    // handed back, which is what the caller asked for; the leftover file is a
    // housekeeping problem, and it is reported through imageOnDevice() rather
    // than by turning a successful release into an error.
    if (cfg_.deleteOnRelease && imageOnDevice_) {
        if (deleteImage(cfg_.imageName)) {
            imageOnDevice_ = false;
        } else {
            ++failedDeletes_;
        }
        err_ = Error::None;
    }
    return true;
}

// ---------------------------------------------------------------------------

uint64_t frameHash(const uint16_t* px, size_t count, uint32_t variant) {
    // FNV-1a, 64-bit. Not cryptographic and does not need to be: the question
    // is "did these pixels change", and the cost of a collision is one skipped
    // refresh, not a wrong screen.
    uint64_t h = 1469598103934665603ull;
    const uint64_t prime = 1099511628211ull;

    for (int i = 0; i < 4; ++i) {
        h ^= static_cast<uint64_t>((variant >> (i * 8)) & 0xFFu);
        h *= prime;
    }
    if (px != nullptr) {
        for (size_t i = 0; i < count; ++i) {
            h ^= static_cast<uint64_t>(px[i] & 0xFFu);
            h *= prime;
            h ^= static_cast<uint64_t>((px[i] >> 8) & 0xFFu);
            h *= prime;
        }
    }
    return h;
}

bool FrameGate::shouldUpload(const uint16_t* px, size_t count, uint32_t variant) {
    const uint64_t h = frameHash(px, count, variant);
    if (has_ && h == hash_) {
        ++skipped_;
        return false;
    }
    hash_ = h;
    has_ = true;
    return true;
}

} // namespace smalltv
