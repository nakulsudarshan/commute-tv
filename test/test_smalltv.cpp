// SmallTV request construction and client sequencing.
//
// The device is replaced by a mock that behaves the way the stock firmware is
// believed to: it accepts exactly one multipart field name, remembers its
// theme, and can be made unreachable or made to refuse. That lets the awkward
// paths -- the device vanishing mid-alert, the field name being wrong, the
// screen being taken twice -- be exercised deterministically, which is not
// something you can arrange with real hardware on a desk.
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "test.h"
#include "../src/smalltv/smalltv_client.h"
#include "../src/smalltv/smalltv_request.h"

using namespace smalltv;

namespace {

std::string percentDecode(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            const std::string hex = in.substr(i + 1, 2);
            out += static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
            i += 2;
        } else {
            out += in[i];
        }
    }
    return out;
}

class MockDevice : public HttpTransport {
public:
    // --- knobs -------------------------------------------------------------
    bool reachable = true;
    const char* acceptField = "data";   // null: refuse every field name
    int theme = 7;
    bool appJsonBroken = false;
    int forceStatus = 0;                // non-zero: answer everything with this

    // The device keeps every uploaded image in LittleFS, permanently. That is
    // not a guess: Nakul found the probe's test image still on the device days
    // after the probe ran. So the mock models a filesystem, and the tests can
    // assert the firmware does not fill 4 MB of flash with countdowns.
    std::vector<std::string> storedFiles;
    bool deleteSupported = true;
    // Which spelling of the path this firmware honours. V9.0.40 answers the
    // others with HTTP 200 and the body "Fail" -- a success status for an
    // operation it did not perform -- so that is what the mock does too.
    // The exact prefix this firmware wants in /delete?file=. "/image//" is
    // what V9.0.40's own web UI sends; "" means it wants a bare filename.
    std::string acceptedDeletePrefix = "/image//";
    int deleteAttempts = 0;
    // A correct server percent-decodes a query value, so %2F and / arrive the
    // same. Some minimal embedded parsers do not, which is exactly why the
    // client carries both spellings -- set this to model one that does not.
    bool naiveQueryParsing = false;
    // V9.0.40 answers every delete with "Fail", successful or not.
    bool alwaysSaysFail = false;
    uint32_t freeBytes = 943 * 1024;
    bool spaceJsonSupported = true;
    int autoplay = 0;
    int imageInterval = 5;
    bool albumJsonSupported = true;
    // Whether uploading a name that already exists replaces it. Unknown on the
    // real firmware until the probe checks; both behaviours are modelled so the
    // firmware is correct either way.
    bool overwriteOnUpload = true;

    // --- observations ------------------------------------------------------
    std::vector<std::string> log;
    std::vector<uint8_t> lastUpload;
    std::string lastContentType;
    std::string lastUploadField;
    int uploadAttempts = 0;

    HttpResponse get(const char* path) override {
        log.push_back(path);
        HttpResponse r;
        if (!reachable) { r.transport = Transport::Unreachable; return r; }
        r.transport = Transport::Ok;
        if (forceStatus) { r.status = forceStatus; return r; }
        r.status = 200;

        const std::string p = path;
        if (p == "/app.json") {
            const std::string body = appJsonBroken
                ? std::string("{\"mode\":\"clock\"}")
                : std::string("{\"theme\":") + std::to_string(theme) + "}";
            std::memcpy(r.body, body.c_str(), body.size());
            r.bodyLen = body.size();
        } else if (p == "/v.json") {
            const std::string body = "{\"model\":\"SmallTV-Ultra\",\"v\":\"9.0.50\"}";
            std::memcpy(r.body, body.c_str(), body.size());
            r.bodyLen = body.size();
        } else if (p.rfind("/set?theme=", 0) == 0) {
            theme = std::atoi(p.c_str() + 11);
        } else if (p.rfind("/filelist", 0) == 0) {
            // The real device answers with an HTML table, not JSON, and spells
            // every href with the slash doubled.
            std::string body = "<table id='list'><tbody><tr><th>#</th><th>Name</th></tr>";
            for (size_t i = 0; i < storedFiles.size(); ++i) {
                const std::string& full = storedFiles[i];
                const std::string name = full.substr(full.rfind('/') + 1);
                body += "<tr><td>" + std::to_string(i + 1) + "</td><td><a href='/image//"
                      + name + "'>" + name + "</a></td></tr>";
            }
            body += "</tbody></table>";
            if (body.size() >= sizeof r.body) body.resize(sizeof r.body - 1);
            std::memcpy(r.body, body.c_str(), body.size());
            r.bodyLen = body.size();
        } else if (p.rfind("/album.json", 0) == 0) {
            if (!albumJsonSupported) { r.status = 404; return r; }
            const std::string body = "{\"autoplay\":" + std::to_string(autoplay) +
                                     ",\"i_i\":" + std::to_string(imageInterval) + "}";
            std::memcpy(r.body, body.c_str(), body.size());
            r.bodyLen = body.size();
        } else if (p.rfind("/set?i_i=", 0) == 0) {
            const size_t amp = p.find("&autoplay=");
            if (amp != std::string::npos) {
                imageInterval = std::atoi(p.c_str() + 9);
                autoplay = std::atoi(p.c_str() + amp + 10);
            }
        } else if (p.rfind("/space.json", 0) == 0) {
            if (!spaceJsonSupported) { r.status = 404; return r; }
            const std::string body =
                "{\"total\":1048576,\"used\":" + std::to_string(1048576 - freeBytes) +
                ",\"free\":" + std::to_string(freeBytes) + "}";
            std::memcpy(r.body, body.c_str(), body.size());
            r.bodyLen = body.size();
        } else if (p.rfind("/delete?file=", 0) == 0) {
            ++deleteAttempts;
            if (!deleteSupported) { r.status = 404; return r; }

            std::string want = p.substr(13);
            if (!naiveQueryParsing) want = percentDecode(want);
            // Exact form, not a prefix: "/image/" is a prefix of
            // "/image//commute.jpg" too, and a mock that accepted both would
            // make the discovery tests pass without discovering anything.
            const std::string bare = want.substr(want.rfind('/') + 1);
            const bool spelledRight = (want == acceptedDeletePrefix + bare);

            bool removed = false;
            if (spelledRight) {
                const std::string name = want.substr(want.rfind('/') + 1);
                for (size_t i = 0; i < storedFiles.size(); ++i) {
                    const std::string stored = storedFiles[i];
                    if (stored.substr(stored.rfind('/') + 1) == name) {
                        storedFiles.erase(storedFiles.begin() + static_cast<long>(i));
                        removed = true;
                        break;
                    }
                }
            }
            if (!removed || alwaysSaysFail) {
                // 200 with "Fail" -- the observed behaviour, and the reason the
                // client can trust neither the status nor the body here.
                const std::string body = "Fail";
                std::memcpy(r.body, body.c_str(), body.size());
                r.bodyLen = body.size();
            }
        }
        return r;
    }

    HttpResponse post(const char* path, const char* contentType,
                      const Slice* parts, size_t partCount) override {
        log.push_back(path);
        ++uploadAttempts;

        HttpResponse r;
        if (!reachable) { r.transport = Transport::Unreachable; return r; }
        r.transport = Transport::Ok;

        // Reassemble the body the way a real server would, then read the field
        // name out of it -- rather than being told what it is.
        std::string body;
        for (size_t i = 0; i < partCount; ++i)
            body.append(reinterpret_cast<const char*>(parts[i].data), parts[i].len);
        lastContentType = contentType;

        const std::string key = "name=\"";
        const size_t a = body.find(key);
        lastUploadField.clear();
        if (a != std::string::npos) {
            const size_t b = body.find('"', a + key.size());
            if (b != std::string::npos)
                lastUploadField = body.substr(a + key.size(), b - a - key.size());
        }

        if (acceptField == nullptr || lastUploadField != acceptField) {
            r.status = 400;
            return r;
        }

        const size_t start = body.find("\r\n\r\n");
        const size_t end = body.rfind("\r\n--");
        if (start != std::string::npos && end != std::string::npos && end > start + 4)
            lastUpload.assign(body.begin() + static_cast<long>(start) + 4,
                              body.begin() + static_cast<long>(end));
        // Record the upload the way the device's filesystem would see it.
        const std::string fkey = "filename=\"";
        const size_t fa = body.find(fkey);
        if (fa != std::string::npos) {
            const size_t fb = body.find('"', fa + fkey.size());
            if (fb != std::string::npos) {
                const std::string name =
                    "/image/" + body.substr(fa + fkey.size(), fb - fa - fkey.size());
                bool exists = false;
                for (const auto& f : storedFiles) if (f == name) exists = true;
                if (!exists || !overwriteOnUpload) storedFiles.push_back(name);
            }
        }

        r.status = 200;
        return r;
    }

    int indexOf(const std::string& p) const {
        for (size_t i = 0; i < log.size(); ++i)
            if (log[i] == p) return static_cast<int>(i);
        return -1;
    }

    bool sawPath(const std::string& p) const {
        for (const auto& e : log) if (e == p) return true;
        return false;
    }
};

const uint8_t kFakeJpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x11, 0x22, 0x33, 0xFF, 0xD9};

} // namespace

void test_smalltv() {
    t::suite("smalltv / urls");
    {
        char buf[kMaxPath];
        CHECK(buildUploadPath("/image/", buf, sizeof buf) > 0);
        CHECK_STR(buf, "/doUpload?dir=/image/");

        CHECK(buildSetThemePath(3, buf, sizeof buf) > 0);
        CHECK_STR(buf, "/set?theme=3");
        CHECK(buildSetThemePath(-1, buf, sizeof buf) < 0);

        // Slashes stay literal, exactly as tools/probe_smalltv.py sends them.
        CHECK(buildSetImagePath("/image/", "commute.jpg", buf, sizeof buf) > 0);
        CHECK_STR(buf, "/set?img=/image/commute.jpg");

        // A directory without a trailing slash must not produce "/imagecommute.jpg".
        CHECK(buildSetImagePath("/image", "commute.jpg", buf, sizeof buf) > 0);
        CHECK_STR(buf, "/set?img=/image/commute.jpg");

        // Anything else in the name is escaped.
        CHECK(buildSetImagePath("/image/", "a b&c.jpg", buf, sizeof buf) > 0);
        CHECK_STR(buf, "/set?img=/image/a%20b%26c.jpg");

        CHECK(buildFileListPath("/image/", buf, sizeof buf) > 0);
        CHECK_STR(buf, "/filelist?dir=/image/");

        CHECK(buildDeletePath("/image/", "commute.jpg", buf, sizeof buf) > 0);
        // The default is the form PROVEN on the device: doubled slash, plain.
        // /delete?file=/image//probe.jpg removed the file;
        // /delete?file=/image/probe.jpg did not.
        CHECK_STR(buf, "/delete?file=/image//commute.jpg");
        // The web UI's own spelling, kept as the first fallback.
        CHECK(buildDeletePath("/image/", "commute.jpg", DeleteStyle::UiExact,
                              buf, sizeof buf) > 0);
        CHECK_STR(buf, "/delete?file=%2Fimage%2F%2Fcommute.jpg");
        CHECK(buildDeletePath("/image/", "commute.jpg", DeleteStyle::SingleSlashEnc,
                              buf, sizeof buf) > 0);
        CHECK_STR(buf, "/delete?file=%2Fimage%2Fcommute.jpg");
        CHECK(buildDeletePath("/image/", "commute.jpg", DeleteStyle::SingleSlash,
                              buf, sizeof buf) > 0);
        CHECK_STR(buf, "/delete?file=/image/commute.jpg");
        CHECK(buildDeletePath("/image/", "commute.jpg", DeleteStyle::NameOnly,
                              buf, sizeof buf) > 0);
        CHECK_STR(buf, "/delete?file=commute.jpg");
        // A directory without its trailing slash must still produce the doubled
        // separator, not three slashes or one.
        CHECK(buildDeletePath("/image", "commute.jpg", DeleteStyle::DoubledSlash,
                              buf, sizeof buf) > 0);
        CHECK_STR(buf, "/delete?file=/image//commute.jpg");

        CHECK(buildSetImagePath("/image/", "", buf, sizeof buf) < 0);
        CHECK(buildSetImagePath("/image/", nullptr, buf, sizeof buf) < 0);

        // Too small a buffer fails outright rather than truncating: a
        // truncated path is a valid request for the wrong thing.
        char tiny[8];
        CHECK(buildSetImagePath("/image/", "commute.jpg", tiny, sizeof tiny) < 0);
    }

    t::suite("smalltv / multipart");
    {
        MultipartFrame f;
        CHECK(buildMultipart("BND", "file", "commute.jpg", "image/jpeg", f));
        CHECK_STR(f.contentType, "multipart/form-data; boundary=BND");
        CHECK_STR(f.header,
                  "--BND\r\nContent-Disposition: form-data; name=\"file\"; "
                  "filename=\"commute.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n");
        CHECK_STR(f.trailer, "\r\n--BND--\r\n");
        CHECK_EQ(f.contentLength(1000), f.headerLen + 1000 + f.trailerLen);

        CHECK(!buildMultipart("", "file", "a.jpg", "image/jpeg", f));
        CHECK(!buildMultipart("BND", "", "a.jpg", "image/jpeg", f));
        CHECK(!buildMultipart("BND", "file", "", "image/jpeg", f));
    }

    t::suite("smalltv / boundary safety");
    {
        char b1[48], b2[48];
        CHECK(makeBoundary(0x12345678u, b1, sizeof b1));
        CHECK_STR(b1, "----commutetv12345678");
        CHECK(makeBoundary(0x87654321u, b2, sizeof b2));
        CHECK(std::strcmp(b1, b2) != 0);

        const uint8_t clean[] = {0xFF, 0xD8, 0x00, 0x01};
        CHECK(boundaryIsSafe(b1, clean, sizeof clean));

        // A payload that happens to contain the boundary must be detected --
        // otherwise the server truncates the image at that point and we would
        // be chasing a "sometimes the picture is corrupt" bug for weeks.
        std::vector<uint8_t> dirty(200, 0x42);
        std::memcpy(dirty.data() + 50, b1, std::strlen(b1));
        CHECK(!boundaryIsSafe(b1, dirty.data(), dirty.size()));

        CHECK(boundaryIsSafe(b1, nullptr, 0));
        CHECK(!boundaryIsSafe("", clean, sizeof clean));
    }

    t::suite("smalltv / basic auth");
    {
        char buf[128];
        // RFC 7617's own example.
        CHECK(buildBasicAuth("Aladdin", "open sesame", buf, sizeof buf) > 0);
        CHECK_STR(buf, "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==");

        CHECK(buildBasicAuth("a", "b", buf, sizeof buf) > 0);   // 3 bytes, no padding
        CHECK_STR(buf, "Basic YTpi");
        CHECK(buildBasicAuth("ab", "c", buf, sizeof buf) > 0);  // 4 bytes, two pads
        CHECK_STR(buf, "Basic YWI6Yw==");
        CHECK(buildBasicAuth("user", nullptr, buf, sizeof buf) > 0);
        CHECK_STR(buf, "Basic dXNlcjo=");

        char tiny[6];
        CHECK(buildBasicAuth("Aladdin", "open sesame", tiny, sizeof tiny) < 0);
    }

    t::suite("smalltv / app.json parsing");
    {
        int theme = -1;
        CHECK(parseThemeFromAppJson("{\"theme\":3}", 11, theme));
        CHECK_EQ(theme, 3);
        CHECK(parseThemeFromAppJson("{ \"theme\" : 12 }", 16, theme));
        CHECK_EQ(theme, 12);
        CHECK(parseThemeFromAppJson("{\"a\":1,\"theme\":0}", 17, theme));
        CHECK_EQ(theme, 0);
        // Some firmwares quote numbers. Tolerated, because the exact shape is
        // not confirmed until the probe runs.
        CHECK(parseThemeFromAppJson("{\"theme\":\"5\"}", 13, theme));
        CHECK_EQ(theme, 5);

        CHECK(!parseThemeFromAppJson("{\"mode\":\"clock\"}", 16, theme));
        CHECK(!parseThemeFromAppJson("{\"theme\":}", 10, theme));
        CHECK(!parseThemeFromAppJson("", 0, theme));
        CHECK(!parseThemeFromAppJson(nullptr, 0, theme));
        CHECK(!parseThemeFromAppJson("{\"theme\":99999999}", 18, theme));
    }

    t::suite("smalltv / file list");
    {
        const char* body = "[{\"name\":\"commute.jpg\"},{\"name\":\"a.gif\"}]";
        CHECK(fileListMentions(body, std::strlen(body), "commute.jpg"));
        CHECK(!fileListMentions(body, std::strlen(body), "missing.jpg"));
        CHECK(!fileListMentions(body, std::strlen(body), ""));
    }

    t::suite("smalltv / upload field discovery");
    {
        MockDevice dev;
        dev.acceptField = "data";           // third candidate
        Config cfg;
        Client c(dev, cfg);

        CHECK(c.uploadImage("commute.jpg", kFakeJpeg, sizeof kFakeJpeg));
        CHECK_STR(c.uploadField(), "data");
        CHECK_EQ(dev.uploadAttempts, 3);    // walked "file", "upload", "data"
        CHECK_EQ(dev.lastUpload.size(), sizeof kFakeJpeg);
        CHECK_EQ(std::memcmp(dev.lastUpload.data(), kFakeJpeg, sizeof kFakeJpeg), 0);

        // Once known, it is used directly -- discovery is a one-time cost.
        dev.uploadAttempts = 0;
        CHECK(c.uploadImage("commute.jpg", kFakeJpeg, sizeof kFakeJpeg));
        CHECK_EQ(dev.uploadAttempts, 1);
    }

    t::suite("smalltv / upload failure modes");
    {
        MockDevice dev;
        dev.acceptField = nullptr;          // refuses everything
        Config cfg;
        cfg.uploadAttempts = 1;
        Client c(dev, cfg);
        CHECK(!c.uploadImage("commute.jpg", kFakeJpeg, sizeof kFakeJpeg));
        CHECK(c.lastError() == Error::UploadRejected);
        CHECK_EQ(dev.uploadAttempts, static_cast<int>(kUploadFieldCandidateCount));

        // An absent device is reported as absent, not as a rejected upload,
        // and we stop after one try instead of hammering six candidates at
        // something that is not there.
        MockDevice gone;
        gone.reachable = false;
        Client c2(gone, cfg);
        CHECK(!c2.uploadImage("commute.jpg", kFakeJpeg, sizeof kFakeJpeg));
        CHECK(c2.lastError() == Error::Unreachable);
        CHECK_EQ(gone.uploadAttempts, 1);
        CHECK_STR(errorText(c2.lastError()), "no reply");

        // Bad arguments never reach the wire.
        MockDevice quiet;
        Client c3(quiet, cfg);
        CHECK(!c3.uploadImage("commute.jpg", nullptr, 0));
        CHECK(c3.lastError() == Error::BadArgument);
        CHECK_EQ(quiet.uploadAttempts, 0);
    }

    t::suite("smalltv / take and release the screen");
    {
        MockDevice dev;
        dev.theme = 7;
        Config cfg;
        cfg.pictureTheme = 3;
        cfg.uploadField = "data";
        Client c(dev, cfg);

        CHECK(!c.holdsScreen());
        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK(c.holdsScreen());
        CHECK_EQ(c.savedTheme(), 7);
        CHECK_EQ(dev.theme, 3);
        CHECK(dev.sawPath("/app.json"));
        CHECK(dev.sawPath("/set?theme=3"));
        CHECK(dev.sawPath("/set?img=/image/commute.jpg"));

        // Refreshing mid-alert must not re-read the theme: doing so would read
        // back our own picture theme and destroy the record of what to restore.
        dev.log.clear();
        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK(!dev.sawPath("/app.json"));
        CHECK_EQ(c.savedTheme(), 7);

        CHECK(c.releaseScreen());
        CHECK_EQ(dev.theme, 7);
        CHECK(!c.holdsScreen());

        // Releasing twice is harmless.
        CHECK(c.releaseScreen());
        CHECK_EQ(dev.theme, 7);
    }

    t::suite("smalltv / release never leaves our own frame up");
    {
        // The device was already on the picture theme when we took it -- for
        // instance because a previous run was interrupted after switching but
        // before recording. Restoring it verbatim would hand the screen back
        // still showing a stale commute frame, which is the exact failure this
        // whole save/restore dance exists to avoid.
        MockDevice dev;
        dev.theme = 3;
        Config cfg;
        cfg.pictureTheme = 3;
        cfg.uploadField = "data";
        Client c(dev, cfg);

        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK_EQ(c.savedTheme(), 3);
        CHECK(c.releaseScreen());
        CHECK(dev.theme != 3);

        // Same guarantee when /app.json never told us anything.
        MockDevice mute;
        mute.appJsonBroken = true;
        Client c2(mute, cfg);
        CHECK(c2.takeScreen(kFakeJpeg, sizeof kFakeJpeg));   // alert still happens
        CHECK_EQ(c2.savedTheme(), -1);
        CHECK(c2.releaseScreen());
        CHECK(mute.theme != 3);
    }

    t::suite("smalltv / device disappears mid-alert");
    {
        MockDevice dev;
        Config cfg;
        cfg.uploadField = "data";
        Client c(dev, cfg);
        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));

        dev.reachable = false;
        CHECK(!c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK(c.lastError() == Error::Unreachable);
        // Still recorded as holding the screen, so when the device comes back
        // the theme is put back rather than left on our picture.
        CHECK(c.holdsScreen());
        CHECK_EQ(c.savedTheme(), 7);

        CHECK(!c.releaseScreen());
        CHECK(c.holdsScreen());

        dev.reachable = true;
        CHECK(c.releaseScreen());
        CHECK_EQ(dev.theme, 7);
        CHECK(!c.holdsScreen());
    }

    t::suite("smalltv / frame gate");
    {
        std::vector<uint16_t> a(64, 0x1234);
        std::vector<uint16_t> b = a;
        b[17] = 0x1235;

        FrameGate g;
        CHECK(g.shouldUpload(a.data(), a.size()));        // first frame always
        CHECK(!g.shouldUpload(a.data(), a.size()));       // unchanged: skipped
        CHECK(!g.shouldUpload(a.data(), a.size()));
        CHECK_EQ(g.skipped(), 2);
        CHECK(g.shouldUpload(b.data(), b.size()));        // one pixel is enough
        CHECK(!g.shouldUpload(b.data(), b.size()));

        // A settings change alters the encoded bytes without altering the
        // pixels, and must not be mistaken for "nothing happened".
        CHECK(g.shouldUpload(b.data(), b.size(), 90));
        CHECK(!g.shouldUpload(b.data(), b.size(), 90));

        g.reset();
        CHECK_EQ(g.skipped(), 0);
        CHECK(g.shouldUpload(b.data(), b.size(), 90));

        // The hash must actually depend on the whole buffer, not just its head.
        std::vector<uint16_t> tail = a;
        tail[63] = 0x9999;
        CHECK(frameHash(a.data(), a.size(), 0) != frameHash(tail.data(), tail.size(), 0));
        CHECK(frameHash(a.data(), a.size(), 0) != frameHash(a.data(), a.size(), 1));
    }

    // ------------------------------------------------------------------------
    // The device has 4 MB of flash and keeps every image it is sent, for ever.
    // An alert uploads a frame a minute for 35 minutes; a few days of that
    // would fill the partition of a device we have promised never to reflash.
    // ------------------------------------------------------------------------
    t::suite("smalltv / the device's flash does not fill up");
    {
        MockDevice dev;
        dev.acceptField = "file";
        Config cfg;
        cfg.uploadField = "file";
        Client c(dev, cfg);

        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK_EQ(dev.storedFiles.size(), 1u);
        CHECK(c.imageOnDevice());

        CHECK(c.releaseScreen());
        CHECK_EQ(dev.storedFiles.size(), 0u);
        CHECK(!c.imageOnDevice());
        CHECK_EQ(c.failedDeletes(), 0u);
    }
    {
        // The real shape of a day: several alerts, each re-uploading the frame
        // every minute. Nothing may be left behind at the end of any of them.
        MockDevice dev;
        dev.acceptField = "file";
        Config cfg;
        cfg.uploadField = "file";
        Client c(dev, cfg);

        for (int alert = 0; alert < 4; ++alert) {
            for (int minute = 0; minute < 35; ++minute) {
                CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
                // Never more than the one frame we are showing, mid-alert.
                if (dev.storedFiles.size() > 1) { CHECK(false); break; }
            }
            CHECK(c.releaseScreen());
            CHECK_EQ(dev.storedFiles.size(), 0u);
        }
    }
    {
        // If the firmware appends rather than overwriting, a single alert would
        // leave 35 files behind. deleteBeforeUpload is the switch for that, and
        // it must keep the count at one.
        MockDevice dev;
        dev.acceptField = "file";
        dev.overwriteOnUpload = false;          // the pessimistic firmware
        Config cfg;
        cfg.uploadField = "file";
        cfg.deleteBeforeUpload = true;
        Client c(dev, cfg);

        for (int minute = 0; minute < 35; ++minute)
            CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK_EQ(dev.storedFiles.size(), 1u);
        CHECK(c.releaseScreen());
        CHECK_EQ(dev.storedFiles.size(), 0u);
    }
    {
        // The delete must come AFTER the theme is restored: removing a file the
        // device is currently displaying is not behaviour the stock firmware
        // promises, and the visible half of the release has already worked.
        MockDevice dev;
        dev.acceptField = "file";
        Config cfg;
        cfg.uploadField = "file";
        Client c(dev, cfg);
        dev.theme = 1;
        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        dev.log.clear();
        CHECK(c.releaseScreen());
        const int themeAt = dev.indexOf("/set?theme=1");
        const int deleteAt = dev.indexOf("/delete?file=/image//commute.jpg");
        CHECK(themeAt >= 0);
        CHECK(deleteAt >= 0);
        CHECK(themeAt < deleteAt);
    }
    {
        // A device that refuses deletes must not turn a working release into a
        // failure -- the screen is already handed back. It is reported instead,
        // because a device quietly filling up is worth seeing on the health
        // panel before the flash is full.
        MockDevice dev;
        dev.acceptField = "file";
        dev.deleteSupported = false;
        Config cfg;
        cfg.uploadField = "file";
        Client c(dev, cfg);

        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK(c.releaseScreen());               // still succeeds
        CHECK(!c.holdsScreen());
        CHECK(c.imageOnDevice());               // but we know it is still there
        CHECK_EQ(c.failedDeletes(), 1u);
        CHECK_EQ(dev.storedFiles.size(), 1u);
    }
    {
        // And the housekeeping can be switched off without breaking release,
        // in case the delete endpoint misbehaves on some firmware version.
        MockDevice dev;
        dev.acceptField = "file";
        Config cfg;
        cfg.uploadField = "file";
        cfg.deleteOnRelease = false;
        Client c(dev, cfg);

        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK(c.releaseScreen());
        CHECK_EQ(dev.storedFiles.size(), 1u);
        CHECK(!dev.sawPath("/delete?file=/image//commute.jpg"));
    }
    {
        // Releasing without ever having taken the screen must not delete
        // anything -- there is nothing of ours on the device to remove.
        MockDevice dev;
        dev.acceptField = "file";
        dev.storedFiles.push_back("/image/somebodys_photo.jpg");
        Config cfg;
        cfg.uploadField = "file";
        Client c(dev, cfg);
        CHECK(c.releaseScreen());
        CHECK_EQ(dev.storedFiles.size(), 1u);
    }

    // ------------------------------------------------------------------------
    // The device answers a delete it did not perform with HTTP 200 and the body
    // "Fail". Observed on V9.0.40, 24 Aug 2026. Every check below exists
    // because a status code is not evidence here.
    // ------------------------------------------------------------------------
    t::suite("smalltv / a 200 is not proof the file is gone");
    {
        MockDevice dev;
        dev.acceptField = "file";
        dev.acceptedDeletePrefix = "/image//";     // the real device's spelling
        Config cfg;
        cfg.uploadField = "file";
        Client c(dev, cfg);

        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK_EQ(dev.storedFiles.size(), 1u);
        CHECK(c.releaseScreen());
        CHECK_EQ(dev.storedFiles.size(), 0u);
        CHECK(!c.imageOnDevice());
        CHECK_EQ(c.failedDeletes(), 0u);
        CHECK(c.deleteStyleKnown());
        CHECK_EQ(static_cast<int>(c.deleteStyle()),
                 static_cast<int>(DeleteStyle::DoubledSlash));
    }
    {
        // A firmware that wants the tidy single-slash form instead. The client
        // must find it rather than being built around one guess -- the evidence
        // for the doubled form is a web page's onclick handler, not a document.
        MockDevice dev;
        dev.acceptField = "file";
        dev.acceptedDeletePrefix = "/image/";
        Config cfg;
        cfg.uploadField = "file";
        Client c(dev, cfg);

        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK(c.releaseScreen());
        CHECK_EQ(dev.storedFiles.size(), 0u);
        CHECK(c.deleteStyleKnown());
        CHECK_EQ(c.failedDeletes(), 0u);
    }
    {
        // And one that wants a bare name.
        MockDevice dev;
        dev.acceptField = "file";
        dev.acceptedDeletePrefix = "";
        Config cfg;
        cfg.uploadField = "file";
        Client c(dev, cfg);

        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK(c.releaseScreen());
        CHECK_EQ(dev.storedFiles.size(), 0u);
        CHECK_EQ(static_cast<int>(c.deleteStyle()),
                 static_cast<int>(DeleteStyle::NameOnly));
    }
    {
        // Once a spelling is known it is reused, so the discovery cost is paid
        // once and not on every alert.
        MockDevice dev;
        dev.acceptField = "file";
        dev.acceptedDeletePrefix = "/image/";      // not the default
        Config cfg;
        cfg.uploadField = "file";
        Client c(dev, cfg);

        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK(c.releaseScreen());
        const int firstRound = dev.deleteAttempts;
        CHECK(firstRound >= 2);                    // tried the default, then found it

        dev.deleteAttempts = 0;
        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK(c.releaseScreen());
        CHECK_EQ(dev.deleteAttempts, 1);           // straight to the one that works
    }
    {
        // A device that refuses every spelling: the release still succeeds --
        // the screen is back -- but nothing pretends the file was removed.
        MockDevice dev;
        dev.acceptField = "file";
        dev.acceptedDeletePrefix = "/nothing-matches/";
        Config cfg;
        cfg.uploadField = "file";
        Client c(dev, cfg);

        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK(c.releaseScreen());
        CHECK(!c.holdsScreen());
        CHECK(c.imageOnDevice());
        CHECK_EQ(c.failedDeletes(), 1u);
        CHECK_EQ(dev.storedFiles.size(), 1u);
        CHECK(!c.deleteStyleKnown());
    }
    {
        // The bug this whole suite is about, stated directly: a device that
        // returns 200 for everything and deletes nothing must NOT be reported
        // as a successful cleanup.
        MockDevice dev;
        dev.acceptField = "file";
        dev.acceptedDeletePrefix = "/never/";
        Config cfg;
        cfg.uploadField = "file";
        Client c(dev, cfg);

        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK(!c.deleteImage("commute.jpg"));
        CHECK_EQ(dev.storedFiles.size(), 1u);
    }
    {
        // Discovery can be switched off for a firmware where the extra attempts
        // are unwelcome.
        MockDevice dev;
        dev.acceptField = "file";
        dev.acceptedDeletePrefix = "/image/";
        Config cfg;
        cfg.uploadField = "file";
        cfg.discoverDeleteStyle = false;
        Client c(dev, cfg);

        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK(c.releaseScreen());
        CHECK_EQ(dev.deleteAttempts, 1);
        CHECK(c.imageOnDevice());              // and it honestly says so
    }

    t::suite("smalltv / reading a body that says Fail");
    {
        CHECK(bodySaysFailure("Fail", 4));
        CHECK(bodySaysFailure("fail", 4));
        CHECK(bodySaysFailure("FAIL", 4));
        CHECK(bodySaysFailure("upload FAILED", 13));
        CHECK(!bodySaysFailure("OK", 2));
        CHECK(!bodySaysFailure("", 0));
        CHECK(!bodySaysFailure(nullptr, 4));
        // A file listing is what a successful call returns, and must not be
        // mistaken for a refusal.
        const char* listing = "<table id='list'><tbody><tr><td>1</td></tr></tbody></table>";
        CHECK(!bodySaysFailure(listing, std::strlen(listing)));
    }

    t::suite("smalltv / a device that does not decode its query");
    {
        // Some minimal embedded query parsers hand back the raw value, so
        // %2Fimage%2F%2Fx.jpg never becomes /image//x.jpg. That possibility is
        // the only reason to carry both spellings; if it is real, the client
        // must still find the one that works.
        MockDevice dev;
        dev.acceptField = "file";
        dev.naiveQueryParsing = true;
        dev.acceptedDeletePrefix = "/image//";
        Config cfg;
        cfg.uploadField = "file";
        Client c(dev, cfg);

        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK(c.releaseScreen());
        CHECK_EQ(dev.storedFiles.size(), 0u);
        CHECK_EQ(static_cast<int>(c.deleteStyle()),
                 static_cast<int>(DeleteStyle::DoubledSlash));
    }

    t::suite("smalltv / the device says Fail even when it succeeds");
    {
        // Measured on V9.0.40, 25 Aug 2026: /delete answers "Fail" whether or
        // not it deleted the file. Reading that body as a refusal -- which an
        // earlier version did -- would make every successful delete look like a
        // failure, retry all five spellings each time, and leave the health
        // panel permanently claiming a file was stranded on the device.
        MockDevice dev;
        dev.acceptField = "file";
        dev.alwaysSaysFail = true;
        Config cfg;
        cfg.uploadField = "file";
        Client c(dev, cfg);

        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK(c.releaseScreen());
        CHECK_EQ(dev.storedFiles.size(), 0u);
        CHECK(!c.imageOnDevice());
        CHECK_EQ(c.failedDeletes(), 0u);
        CHECK_EQ(c.lyingDeletes(), 1u);        // it said Fail, and it lied
        CHECK_EQ(dev.deleteAttempts, 1);       // one request, not five
    }

    // ------------------------------------------------------------------------
    // /space.json, recovered from the device's own picture page. The image area
    // is about 1 MB, so this is the number that says whether the housekeeping
    // is actually working.
    // ------------------------------------------------------------------------
    t::suite("smalltv / free space");
    {
        CHECK(buildSpacePath(nullptr, 0) < 0);
        char buf[kMaxPath];
        CHECK(buildSpacePath(buf, sizeof buf) > 0);
        CHECK_STR(buf, "/space.json");

        uint32_t v = 0;
        const char* body = "{\"total\":1048576,\"used\":83968,\"free\":964608}";
        CHECK(parseFreeBytes(body, std::strlen(body), v));
        CHECK_EQ(v, 964608u);

        // Quoted numbers happen in this firmware's other endpoints.
        const char* quoted = "{\"free\":\"12345\"}";
        CHECK(parseFreeBytes(quoted, std::strlen(quoted), v));
        CHECK_EQ(v, 12345u);

        // Absence must not read as zero -- that would look like a full device
        // and stop the alerts.
        v = 7;
        CHECK(!parseFreeBytes("{\"total\":1048576}", 18, v));
        CHECK_EQ(v, 7u);
        CHECK(!parseFreeBytes(nullptr, 10, v));
        CHECK(!parseFreeBytes("{\"free\":}", 9, v));
    }
    {
        MockDevice dev;
        dev.acceptField = "file";
        Config cfg;
        cfg.uploadField = "file";
        Client c(dev, cfg);

        uint32_t freeBytes = 0;
        CHECK(c.readFreeSpace(freeBytes));
        CHECK_EQ(freeBytes, 943u * 1024u);
        CHECK(c.freeSpaceKnown());
        CHECK_EQ(c.lastFreeBytes(), 943u * 1024u);
    }
    {
        // A device with almost nothing left: refuse rather than being the thing
        // that fills the last of it.
        MockDevice dev;
        dev.acceptField = "file";
        dev.freeBytes = 20 * 1024;
        Config cfg;
        cfg.uploadField = "file";
        Client c(dev, cfg);

        CHECK(!c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK_EQ(static_cast<int>(c.lastError()), static_cast<int>(Error::DeviceFull));
        CHECK_EQ(dev.storedFiles.size(), 0u);
        CHECK(!c.holdsScreen());
    }
    {
        // Mid-alert the check is skipped: we are overwriting our own file every
        // minute, and asking every time would be pure traffic.
        MockDevice dev;
        dev.acceptField = "file";
        Config cfg;
        cfg.uploadField = "file";
        Client c(dev, cfg);

        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        const int before = static_cast<int>(dev.log.size());
        int spaceCalls = 0;
        for (const auto& e : dev.log) if (e == "/space.json") ++spaceCalls;
        CHECK_EQ(spaceCalls, 1);

        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        spaceCalls = 0;
        for (const auto& e : dev.log) if (e == "/space.json") ++spaceCalls;
        CHECK_EQ(spaceCalls, 1);          // still just the one, from the first take
        CHECK(static_cast<int>(dev.log.size()) > before);
    }
    {
        // A firmware without /space.json must not be treated as full. An
        // unanswered question is not a no.
        MockDevice dev;
        dev.acceptField = "file";
        dev.spaceJsonSupported = false;
        Config cfg;
        cfg.uploadField = "file";
        Client c(dev, cfg);

        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK(c.holdsScreen());
        CHECK(!c.freeSpaceKnown());
        CHECK(c.releaseScreen());
    }
    {
        // And the check can be switched off entirely.
        MockDevice dev;
        dev.acceptField = "file";
        dev.freeBytes = 1024;
        Config cfg;
        cfg.uploadField = "file";
        cfg.minFreeBytes = 0;
        Client c(dev, cfg);

        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK(!dev.sawPath("/space.json"));
    }

    // ------------------------------------------------------------------------
    // Photo-album autoplay cycles every stored image on a timer. Left on, it
    // replaces our commute frame with the next picture a few seconds after it
    // appears -- which would read as a broken upload rather than a setting.
    // ------------------------------------------------------------------------
    t::suite("smalltv / album autoplay is suppressed and restored");
    {
        char buf[kMaxPath];
        CHECK(buildAlbumPath(buf, sizeof buf) > 0);
        CHECK_STR(buf, "/album.json");
        CHECK(buildSetAlbumPath(5, 0, buf, sizeof buf) > 0);
        CHECK_STR(buf, "/set?i_i=5&autoplay=0");
        CHECK(buildSetAlbumPath(12, 1, buf, sizeof buf) > 0);
        CHECK_STR(buf, "/set?i_i=12&autoplay=1");
        // An interval the device would reject is clamped, not passed on.
        CHECK(buildSetAlbumPath(0, 1, buf, sizeof buf) > 0);
        CHECK_STR(buf, "/set?i_i=1&autoplay=1");

        int a = -1, i = -1;
        const char* body = "{\"autoplay\":1,\"i_i\":8}";
        CHECK(parseAlbum(body, std::strlen(body), a, i));
        CHECK_EQ(a, 1);
        CHECK_EQ(i, 8);
        const char* quoted = "{\"autoplay\":\"0\",\"i_i\":\"30\"}";
        CHECK(parseAlbum(quoted, std::strlen(quoted), a, i));
        CHECK_EQ(a, 0);
        CHECK_EQ(i, 30);
        CHECK(!parseAlbum("{\"theme\":3}", 11, a, i));
        CHECK(!parseAlbum(nullptr, 5, a, i));
    }
    {
        MockDevice dev;
        dev.acceptField = "file";
        dev.autoplay = 1;
        dev.imageInterval = 7;
        Config cfg;
        cfg.uploadField = "file";
        Client c(dev, cfg);

        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK_EQ(dev.autoplay, 0);              // off while we hold the screen
        CHECK(c.autoplaySuppressed());

        // Re-reading mid-alert would read back our own suppression, so the
        // saved value must survive every subsequent frame.
        for (int k = 0; k < 5; ++k) CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK(c.autoplaySuppressed());
        CHECK_EQ(dev.autoplay, 0);

        CHECK(c.releaseScreen());
        CHECK_EQ(dev.autoplay, 1);              // and exactly as it was
        CHECK_EQ(dev.imageInterval, 7);         // including the interval
        CHECK(!c.autoplaySuppressed());
    }
    {
        // A device that already has it off must not be written to at all.
        MockDevice dev;
        dev.acceptField = "file";
        dev.autoplay = 0;
        Config cfg;
        cfg.uploadField = "file";
        Client c(dev, cfg);

        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK(!c.autoplaySuppressed());
        CHECK(!dev.sawPath("/set?i_i=5&autoplay=0"));
        CHECK(c.releaseScreen());
        CHECK_EQ(dev.autoplay, 0);
    }
    {
        // A firmware with no /album.json is not a reason to refuse the alert.
        MockDevice dev;
        dev.acceptField = "file";
        dev.albumJsonSupported = false;
        Config cfg;
        cfg.uploadField = "file";
        Client c(dev, cfg);

        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK(c.holdsScreen());
        CHECK(!c.autoplaySuppressed());
        CHECK(c.releaseScreen());
    }
    {
        // And it can be switched off, for someone who would rather the firmware
        // never touched their settings.
        MockDevice dev;
        dev.acceptField = "file";
        dev.autoplay = 1;
        Config cfg;
        cfg.uploadField = "file";
        cfg.suppressAutoplay = false;
        Client c(dev, cfg);

        CHECK(c.takeScreen(kFakeJpeg, sizeof kFakeJpeg));
        CHECK_EQ(dev.autoplay, 1);              // left alone
        CHECK(!dev.sawPath("/album.json"));
    }

    t::suite("smalltv / the screen goes back to a REAL theme");
    {
        // ⚠️ FROM THE DEVICE, 25 Aug 2026. When the theme could not be read the
        // release fell back to theme 0 -- and there is no theme 0. The device
        // stayed on Photo Album (3) with an album we had just emptied, and read
        // "No images." over the last commute frame until touched by hand.
        //
        // Every path out of releaseScreen must land on 1..7.
        MockDevice t;
        t.appJsonBroken = true;              // /app.json will not say
        Config cfg;
        Client c(t, cfg);

        const uint8_t frame[] = {0xFF, 0xD8, 0xFF, 0xD9};
        CHECK(c.takeScreen(frame, sizeof frame));
        CHECK(c.holdsScreen());

        CHECK(c.releaseScreen());
        CHECK(!c.holdsScreen());
        CHECK(t.theme >= 1);
        CHECK(t.theme <= 7);
        CHECK_EQ(t.theme, cfg.fallbackTheme);
        CHECK(t.theme != cfg.pictureTheme);
    }
    {
        // And a device that DOES say what it was on gets exactly that back.
        MockDevice t;
        t.theme = 5;
        Config cfg;
        Client c(t, cfg);
        const uint8_t frame[] = {0xFF, 0xD8, 0xFF, 0xD9};
        CHECK(c.takeScreen(frame, sizeof frame));
        CHECK_EQ(t.theme, cfg.pictureTheme);      // ours while alerting
        CHECK(c.releaseScreen());
        CHECK_EQ(t.theme, 5);                     // and his again afterwards
    }

    t::suite("smalltv / recovering a screen nobody is holding");
    {
        // The device is on our picture theme and we are not holding it -- a
        // reboot mid-alert, a power cut. Whatever took the screen is gone and
        // took the saved theme with it, so the fallback IS the answer.
        MockDevice t;
        t.theme = 3;                          // stuck on Photo Album
        Config cfg;
        Client c(t, cfg);
        CHECK(c.reconcile());
        CHECK_EQ(t.theme, cfg.fallbackTheme);
        CHECK(t.theme != cfg.pictureTheme);
    }
    {
        // On any other theme it is not ours and must not be touched.
        MockDevice t;
        t.theme = 5;
        Config cfg;
        Client c(t, cfg);
        CHECK(!c.reconcile());                // "nothing to do", not "failed"
        CHECK_EQ(t.theme, 5);
        CHECK(c.lastError() == Error::None);
    }
    {
        // A device that will not say is left alone rather than guessed at: a
        // reset to theme 1 on a device happily showing something else would be
        // us breaking it, not fixing it.
        MockDevice t;
        t.appJsonBroken = true;
        t.theme = 6;
        Config cfg;
        Client c(t, cfg);
        CHECK(!c.reconcile());
        CHECK_EQ(t.theme, 6);
        CHECK(c.lastError() == Error::ThemeUnknown);
    }
    {
        // And it never fights the alert it is running: holding the screen means
        // reconcile does nothing at all.
        MockDevice t;
        Config cfg;
        Client c(t, cfg);
        const uint8_t frame[] = {0xFF, 0xD8, 0xFF, 0xD9};
        CHECK(c.takeScreen(frame, sizeof frame));
        CHECK(c.reconcile());
        CHECK_EQ(t.theme, cfg.pictureTheme);  // still ours
        CHECK(c.holdsScreen());
    }

    t::suite("smalltv / the theme it idles on");
    {
        // ⚠️ FROM THE DEVICE, 28 Aug 2026. reconcile() alone left it on theme 1
        // for days: the device had never been stranded on OUR theme, so there
        // was nothing for reconcile to rescue and it correctly did nothing.
        // Wanting a particular idle theme is a different question, and this is
        // the one that answers it.
        MockDevice t;
        t.theme = 1;
        Config cfg;
        cfg.fallbackTheme = 6;
        Client c(t, cfg);

        CHECK(!c.reconcile());                  // not ours: not its business
        CHECK_EQ(t.theme, 1);

        CHECK(c.enforceTheme(cfg.fallbackTheme));
        CHECK_EQ(t.theme, 6);
        CHECK(c.lastError() == Error::None);
    }
    {
        // Already right: nothing done, nothing broken, and NOT reported as a
        // failure. A caller that logs every false would otherwise print a
        // complaint on every boot of a device that is doing exactly as asked.
        MockDevice t;
        t.theme = 6;
        Config cfg;
        Client c(t, cfg);
        CHECK(!c.enforceTheme(6));
        CHECK_EQ(t.theme, 6);
        CHECK(c.lastError() == Error::None);
        CHECK(!t.sawPath("/set?theme=6"));      // it did not even ask
    }
    {
        // Stranded on our picture theme with our frame still on it: it moves
        // AND takes the file with it, which is the same debt reconcile()
        // settles. Reached here the way it is reached in life -- a release
        // whose delete did not work, and then something puts the device back on
        // Photo Album.
        MockDevice t;
        t.deleteSupported = false;              // the release cannot clean up
        Config cfg;
        cfg.fallbackTheme = 6;
        Client c(t, cfg);
        const uint8_t frame[] = {0xFF, 0xD8, 0xFF, 0xD9};
        CHECK(c.takeScreen(frame, sizeof frame));
        CHECK(c.releaseScreen());
        CHECK(c.imageOnDevice());               // still there, and we know it
        CHECK_EQ(t.storedFiles.size(), 1u);

        t.deleteSupported = true;               // and now it can
        t.theme = cfg.pictureTheme;             // something put it back on ours
        CHECK(c.enforceTheme(6));
        CHECK_EQ(t.theme, 6);
        CHECK(!c.imageOnDevice());
        CHECK_EQ(t.storedFiles.size(), 0u);
    }
    {
        // It never fights the alert. An idle preference is not worth blanking
        // the countdown for.
        MockDevice t;
        Config cfg;
        cfg.fallbackTheme = 6;
        Client c(t, cfg);
        const uint8_t frame[] = {0xFF, 0xD8, 0xFF, 0xD9};
        CHECK(c.takeScreen(frame, sizeof frame));
        CHECK(!c.enforceTheme(6));
        CHECK_EQ(t.theme, cfg.pictureTheme);
        CHECK(c.holdsScreen());
        CHECK(c.lastError() == Error::None);
    }
    {
        // A device that will not say what it is on is left alone, exactly as
        // reconcile() leaves it: setting a theme blind is how you interrupt
        // something the owner chose on purpose.
        MockDevice t;
        t.appJsonBroken = true;
        t.theme = 2;
        Config cfg;
        Client c(t, cfg);
        CHECK(!c.enforceTheme(6));
        CHECK_EQ(t.theme, 2);
        CHECK(c.lastError() == Error::ThemeUnknown);
    }
    {
        // 0 is not a theme. It was not one for releaseScreen either.
        MockDevice t;
        t.theme = 1;
        Config cfg;
        Client c(t, cfg);
        CHECK(!c.enforceTheme(0));
        CHECK_EQ(t.theme, 1);
        CHECK(c.lastError() == Error::BadArgument);
    }

    t::suite("smalltv / telling it where to get the time");
    {
        // The endpoint is /set?ntp=<host>, taken from the saved copy of the
        // device's own Time page:  var url = "/set?ntp="+ntp.replace(/ /g,"");
        MockDevice t;
        Config cfg;
        Client c(t, cfg);
        CHECK(c.setNtpServer("192.168.4.1"));
        CHECK(t.sawPath("/set?ntp=192.168.4.1"));
    }
    {
        MockDevice t;
        Config cfg;
        Client c(t, cfg);
        CHECK(!c.setNtpServer(""));
        CHECK(!c.setNtpServer(nullptr));
        CHECK(c.lastError() == Error::BadArgument);
    }

    t::suite("smalltv / reading a field out of its one-line json");
    {
        char buf[48];
        CHECK(parseJsonString("{\"ntp\":\"ntp.aliyun.com\"}", 24, "ntp", buf, sizeof buf));
        CHECK_STR(buf, "ntp.aliyun.com");

        CHECK(parseJsonString("{\"a\":1,\"ntp\" : \"192.168.4.1\"}", 30, "ntp",
                              buf, sizeof buf));
        CHECK_STR(buf, "192.168.4.1");

        // A key that is a PREFIX of another must not match it.
        CHECK(!parseJsonString("{\"ntpx\":\"no\"}", 14, "ntp", buf, sizeof buf));

        // Present but not a string, absent, empty, unterminated: all refused,
        // and all leave the buffer empty rather than half-written.
        CHECK(!parseJsonString("{\"ntp\":123}", 11, "ntp", buf, sizeof buf));
        CHECK(!parseJsonString("{\"other\":\"x\"}", 14, "ntp", buf, sizeof buf));
        CHECK(!parseJsonString("{\"ntp\":\"\"}", 11, "ntp", buf, sizeof buf));
        CHECK(!parseJsonString("{\"ntp\":\"unterminated", 21, "ntp", buf, sizeof buf));
        CHECK_STR(buf, "");

        // Too long to hold is refused, not truncated into a different hostname.
        char tiny[6];
        CHECK(!parseJsonString("{\"ntp\":\"ntp.aliyun.com\"}", 24, "ntp", tiny,
                               sizeof tiny));
        CHECK_STR(tiny, "");

        CHECK(!parseJsonString(nullptr, 0, "ntp", buf, sizeof buf));
        CHECK(!parseJsonString("{}", 2, "", buf, sizeof buf));
    }
}
