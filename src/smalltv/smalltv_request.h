// smalltv_request.h - the SmallTV Ultra's stock HTTP API, as bytes.
//
// Pure construction and parsing: no sockets, no Arduino, so every URL and
// every byte of the multipart body is unit-testable on the host. The client
// layer above only has to move the bytes.
//
// The API was recovered from the V9.0.50 firmware binary and matches the
// profile documented by adrienbrault/geekmagic-hacs:
//
//   POST /doUpload?dir=/image/   multipart upload into LittleFS
//   GET  /set?theme=<n>          switch active theme
//   GET  /set?img=/image/<name>  display one stored image
//   GET  /filelist?dir=/image/   list stored files
//   GET  /delete?file=...        delete one
//   GET  /app.json               {"theme":N}
//   GET  /v.json                 firmware / model identity
//
// One thing the binary does NOT reveal is the multipart field name /doUpload
// expects. Rather than hard-code a guess, the client discovers it at runtime
// from the candidate list below -- see smalltv_client.h.
#pragma once
#include <cstddef>
#include <cstdint>

namespace smalltv {

// Field names to try against /doUpload, in order of likelihood. Kept identical
// to UPLOAD_FIELD_CANDIDATES in tools/probe_smalltv.py: if the probe finds one
// that works, the firmware must be trying the same set in the same way, or the
// probe's result would not transfer.
extern const char* const kUploadFieldCandidates[];
extern const size_t kUploadFieldCandidateCount;

constexpr size_t kMaxPath = 192;

// --- URLs. Each returns the length written, or -1 if it did not fit. --------

int buildUploadPath(const char* dir, char* out, size_t outSize);
int buildSetThemePath(int theme, char* out, size_t outSize);
int buildSetImagePath(const char* dir, const char* name, char* out, size_t outSize);
int buildFileListPath(const char* dir, char* out, size_t outSize);

// How this firmware wants a path spelled in /delete?file=.
//
// Not a style question -- V9.0.40 returns HTTP 200 with the body "Fail" for a
// form it does not like, so getting this wrong silently stops the device ever
// being cleaned up. Its own web UI calls deletef('/image//name.jpg'), with the
// slash doubled, which is the shape you get from dir + "/" + name when dir
// already ends in one. That is the first thing to try; the others exist because
// the evidence is a page's onclick handler, not documentation.
// Recovered from the device's own web page (image.html, 25 Aug 2026):
//
//     function deletef(h) {
//         var fnstring = String("/delete?file=") + encodeURIComponent(h);
//     }
//     ... onclick="deletef('/image//spaceman.gif')"
//
// So the UI sends both the doubled slash and full percent-encoding --
// encodeURIComponent escapes '/' as %2F, unlike the encoding used everywhere
// else in this file.
//
// PROVEN ON THE DEVICE, 25 Aug 2026: the doubled slash is what matters, and
// the encoding does not. `/delete?file=/image//probe.jpg` removed the file;
// `/delete?file=/image/probe.jpg` did not. So DoubledSlash is the default --
// it is the form with direct evidence behind it -- and the others remain as
// fallbacks for firmware we have not seen.
enum class DeleteStyle : uint8_t {
    DoubledSlash,   // /image//commute.jpg        <- confirmed working
    UiExact,        // %2Fimage%2F%2Fcommute.jpg  <- what the web UI sends
    SingleSlashEnc, // %2Fimage%2Fcommute.jpg
    SingleSlash,    // /image/commute.jpg         <- confirmed NOT working
    NameOnly,       // commute.jpg
};

constexpr int kDeleteStyleCount = 5;
const char* deleteStyleName(DeleteStyle s);
int buildDeletePath(const char* dir, const char* name, char* out, size_t outSize);
int buildDeletePath(const char* dir, const char* name, DeleteStyle style,
                    char* out, size_t outSize);

constexpr const char* kAppJsonPath = "/app.json";
constexpr const char* kVersionPath = "/v.json";

// --- Multipart framing ------------------------------------------------------

// The payload is never copied. A frame is a header and a trailer that bracket
// the caller's JPEG buffer, so uploading a 22 KB image costs 22 KB of RAM and
// not 44 KB -- which on a device with an ~80 KB margin is the difference
// between comfortable and interesting.
struct MultipartFrame {
    char contentType[96] = {0};   // for the Content-Type request header
    char header[224] = {0};
    size_t headerLen = 0;
    char trailer[80] = {0};
    size_t trailerLen = 0;

    size_t contentLength(size_t payloadLen) const {
        return headerLen + payloadLen + trailerLen;
    }
};

// Byte-for-byte the framing tools/probe_smalltv.py sends. That equality is the
// point: the probe's answer about the field name is only transferable if the
// request around it is the same.
bool buildMultipart(const char* boundary, const char* field, const char* filename,
                    const char* payloadContentType, MultipartFrame& out);

// Derives a boundary from a counter. Deliberately not a fixed string: the
// boundary must not occur inside the payload, and JPEG entropy-coded data is
// effectively random bytes, so a constant would fail once in a very long while
// and be almost impossible to diagnose from the symptom.
bool makeBoundary(uint32_t nonce, char* out, size_t outSize);

// True when `boundary` does not appear anywhere in the payload. Cheap
// insurance -- the caller re-rolls the nonce on a collision.
bool boundaryIsSafe(const char* boundary, const uint8_t* payload, size_t len);

// "Basic <base64(user:pass)>". The firmware supports HTTP Basic auth and we
// want it enabled the moment the device is anywhere but its own private link.
int buildBasicAuth(const char* user, const char* pass, char* out, size_t outSize);

// --- Response parsing -------------------------------------------------------

// Reads the theme index out of /app.json. Tolerant of whitespace and of extra
// keys, because the exact shape is not confirmed until the probe runs, and a
// parser that only accepts one spelling would fail on contact with the device.
bool parseThemeFromAppJson(const char* body, size_t len, int& theme);

// Whether a /filelist response mentions `name`. The response format is not yet
// confirmed, so this is a substring test rather than a parse -- honest about
// what it knows. Used for reporting, never as a correctness gate.
bool fileListMentions(const char* body, size_t len, const char* name);

// True when a response body reads as a refusal. The device's own settings.js
// treats a body of exactly "OK" as success and anything else as failure, so
// this is the right test for /set?... calls.
//
// ⚠️ It is the WRONG test for /delete. Measured on V9.0.40, 25 Aug 2026:
// that endpoint answers "Fail" whether or not it deleted the file. Both of
// these were observed, minutes apart:
//
//     /delete?file=/image/probe.jpg    HTTP 200  Fail   -> file still there
//     /delete?file=/image//probe.jpg   HTTP 200  Fail   -> file GONE
//
// Neither the status nor the body carries any information. The only evidence
// is whether the name is still in /filelist afterwards.
bool bodySaysFailure(const char* body, size_t len);

// GET /space.json -> {"free":965632,...}. Recovered from the device's own
// picture page, which renders it as "Free Space:943KB".
//
// This is the number that decides whether the housekeeping is working. The
// image area is about a megabyte, and a frame is ~20 KB, so a device whose
// deletes are silently failing has days rather than months.
int buildSpacePath(char* out, size_t outSize);

// GET /album.json -> {"autoplay":1,"i_i":5}
//
// ⚠️ Photo-album autoplay is the setting that would quietly ruin the alert. If
// it is on, the device cycles through every stored image every `i_i` seconds --
// so our commute frame would be replaced by somebody's cat GIF five seconds
// after it appeared, once a minute, for thirty-five minutes. Recovered from the
// device's own picture page:
//
//     var url = "/set?i_i=" + image_interval.value + "&autoplay=" + autoplay.value;
int buildAlbumPath(char* out, size_t outSize);
int buildSetAlbumPath(int intervalSec, int autoplay, char* out, size_t outSize);

// Reads both values. Returns false if the body held neither.
bool parseAlbum(const char* body, size_t len, int& autoplay, int& intervalSec);

// Reads `free` out of that body. Returns false if it is not there -- absence
// must not read as zero, which would look like a full filesystem.
bool parseFreeBytes(const char* body, size_t len, uint32_t& freeBytes);

// The string value of `key` out of a flat object, e.g. `ntp` from
// {"ntp":"ntp.aliyun.com"}. Enough JSON for a device that answers in one line,
// and no more: this is not a parser, it is a field reader.
bool parseJsonString(const char* body, size_t len, const char* key, char* out,
                     size_t outCap);

} // namespace smalltv
