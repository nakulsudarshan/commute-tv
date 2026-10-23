#include "calendar.h"

#include <cstdio>
#include <cstring>

#include "../core/textutil.h"
#include "../core/timeutil.h"
#include "../core/urlencode.h"
#include "../net/json_sink.h"

namespace google {
namespace {

// --- the token response ------------------------------------------------------

struct TokenHandler final : json::Handler {
    // The token is copied into its destination the moment it arrives, NOT
    // remembered as a pointer into the reader's value buffer: `expires_in`,
    // `token_type` and `scope` all follow it in Google's response and reuse
    // that same buffer. Holding a pointer here produced an Authorization
    // header reading "Bearer 359" -- valid-looking, and rejected.
    Token* token = nullptr;
    int32_t expiresIn = 0;
    bool gotToken = false;
    char error[64] = {0};
    char errorDesc[128] = {0};

    void onScalar(const json::Path& p, json::Type, const char* text, size_t len,
                  bool truncated) override {
        if (p.is("access_token")) {
            // A truncated token is useless and, worse, would authenticate
            // nothing while looking like success. Treat it as absent.
            if (truncated || len == 0) { gotToken = false; return; }
            token->set(text, len, 0);          // expiry is filled in by the caller
            gotToken = token->header[0] != '\0';
        } else if (p.is("expires_in")) {
            // A non-integer or overflowing value is not a lifetime. Taking the
            // integral part of "3.9" would make the token expire in three
            // seconds and re-mint one on every single poll.
            //
            // Range-checked BEFORE the narrowing cast, not after. `long` is 64
            // bits on the host and 32 on the ESP32, so a value like 4294967596
            // overflows toLong on the device but survives on the host and then
            // truncates to 300 in the cast -- a plausible-looking lifetime that
            // would make every poll mint a new token. Checking first behaves
            // the same on both.
            bool okNum = false;
            const long v = json::toLong(text, &okNum);
            expiresIn = (okNum && v > 0 && v <= kMaxAssertionLifetimeSec)
                            ? static_cast<int32_t>(v) : 0;
        } else if (p.is("error")) {
            core::copyStr(error, sizeof error, text, len);
        } else if (p.is("error_description")) {
            core::copyStr(errorDesc, sizeof errorDesc, text, len);
        }
    }
};

// --- the events response -----------------------------------------------------

struct EventHandler final : json::Handler {
    EventList* out = nullptr;

    // One event under construction. Fields arrive in no guaranteed order, so
    // the decision to keep it can only be made when the object closes.
    core::CalendarEvent cur{};
    bool inItem = false;
    bool cancelled = false;
    bool hasLocation = false;
    bool allDay = false;
    bool startValid = false;

    char error[64] = {0};

    void onEnter(const json::Path& p, bool isArray) override {
        if (isArray) return;
        if (p.depth() == 2 && p.is("items[*]")) {
            cur = core::CalendarEvent{};
            inItem = true;
            cancelled = false;
            hasLocation = false;
            allDay = false;
            startValid = false;
            if (out->seen < 255) ++out->seen;
        }
    }

    void onLeave(const json::Path& p, bool isArray) override {
        if (isArray || !inItem) return;
        if (!(p.depth() == 2 && p.is("items[*]"))) return;
        inItem = false;

        // The product rule, applied here rather than downstream: an event with
        // no location is not a commute, and there are only six slots.
        if (cancelled)      { if (out->skippedCancelled < 255) ++out->skippedCancelled; return; }
        if (allDay)         { if (out->skippedAllDay < 255) ++out->skippedAllDay; return; }
        if (!hasLocation)   { if (out->skippedNoLocation < 255) ++out->skippedNoLocation; return; }
        // Counted separately: an event that names a place but whose start we
        // could not read is a parsing problem, and blaming it on the location
        // would send the next person looking in the wrong place.
        if (!startValid)    { if (out->skippedNoStart < 255) ++out->skippedNoStart; return; }

        if (out->count < kMaxEvents) {
            out->events[out->count] = cur;
            ++out->count;
        } else {
            out->more = true;      // a match we could not carry
        }
    }

    void onScalar(const json::Path& p, json::Type, const char* text, size_t len,
                  bool truncated) override {
        (void)truncated;

        if (p.is("nextPageToken")) { out->more = true; return; }
        if (p.is("error.message")) { core::copyStr(error, sizeof error, text, len); return; }
        if (p.is("error.status"))  { if (!error[0]) core::copyStr(error, sizeof error, text, len); return; }
        if (!inItem) return;

        if (p.is("items[*].summary")) {
            if (!core::foldInto(cur.title, sizeof cur.title, text, len))
                out->textTruncated = true;
        } else if (p.is("items[*].location")) {
            // Google returns "" for an event whose location was cleared, which
            // must count as absent rather than as an empty place name.
            if (len > 0) {
                hasLocation = true;
                if (!core::foldInto(cur.location, sizeof cur.location, text, len))
                    out->textTruncated = true;
            }
        } else if (p.is("items[*].status")) {
            cancelled = (std::strcmp(text, "cancelled") == 0);
        } else if (p.is("items[*].start.dateTime")) {
            // Requested in the calendar's own zone, so this is local wall time
            // and the trailing offset is read past. Same rule as HAFAS.
            cur.start = core::parseIso8601Local(text);
            startValid = cur.start.valid();
            cur.allDay = false;
        } else if (p.is("items[*].start.date")) {
            // An all-day event has `date` instead of `dateTime`. It names no
            // time to leave for, so it is not actionable -- but the start is
            // still parsed so a caller can show it if it ever wants to.
            const int32_t days = core::parseDate(text);
            if (days != INT32_MIN) {
                cur.start = core::makeTs(days * 1440);
                startValid = true;
            }
            allDay = true;
            cur.allDay = true;
        }
    }
};

} // namespace

void Token::set(const char* token, size_t len, int64_t expiresAt) {
    if (token == nullptr || len == 0 || len > kMaxTokenLen) { clear(); return; }
    std::memcpy(header, "Bearer ", kPrefixLen);
    std::memcpy(header + kPrefixLen, token, len);
    header[kPrefixLen + len] = '\0';
    expiresAtUtcSec = expiresAt;
}

const char* statusName(Status s) {
    switch (s) {
        case Status::Ok:          return "ok";
        case Status::BadConfig:   return "bad config";
        case Status::Transport:   return "transport";
        case Status::HttpError:   return "http error";
        case Status::AuthFailed:  return "auth failed";
        case Status::BadResponse: return "bad response";
    }
    return "?";
}

int buildEventsUrl(const Config& cfg, const Query& q, char* out, size_t outCap) {
    if (!out || outCap == 0) return -1;
    if (!cfg.apiBase || !cfg.calendarId || cfg.calendarId[0] == '\0') return -1;
    if (!q.from.valid() || !q.to.valid()) return -1;

    char calId[160];
    if (core::urlEncode(cfg.calendarId, calId, sizeof calId) < 0) return -1;

    char fromBuf[32], toBuf[32];
    core::formatRfc3339(q.from, q.utcOffsetMin, fromBuf);
    core::formatRfc3339(q.to, q.utcOffsetMin, toBuf);

    // The offset's '+' MUST be escaped. Unescaped it decodes as a space, the
    // timestamp becomes invalid, and Google either errors or -- worse --
    // interprets it as UTC and shifts the whole window by an hour.
    char fromEnc[48], toEnc[48], tzEnc[64];
    if (core::urlEncode(fromBuf, fromEnc, sizeof fromEnc) < 0) return -1;
    if (core::urlEncode(toBuf, toEnc, sizeof toEnc) < 0) return -1;
    if (core::urlEncode(cfg.timeZone ? cfg.timeZone : "UTC", tzEnc, sizeof tzEnc) < 0) return -1;

    // Ask for only the fields we read. On a busy calendar the untrimmed
    // response carries organiser, attendee, conferencing and reminder blocks
    // for every event, none of which this device will ever show.
    char fieldsEnc[128];
    if (core::urlEncode("items(summary,location,status,start),nextPageToken",
                        fieldsEnc, sizeof fieldsEnc) < 0) return -1;

    int maxResults = q.maxResults;
    if (maxResults < 1) maxResults = 1;
    if (maxResults > 250) maxResults = 250;

    // singleEvents=true is required for orderBy=startTime, and it is also what
    // turns a weekly lecture into the individual instance we need.
    const int n = std::snprintf(
        out, outCap,
        "%s/calendars/%s/events"
        "?timeMin=%s&timeMax=%s"
        "&singleEvents=true&orderBy=startTime&showDeleted=false"
        "&maxResults=%d&timeZone=%s&fields=%s",
        cfg.apiBase, calId, fromEnc, toEnc, maxResults, tzEnc, fieldsEnc);

    if (n < 0 || static_cast<size_t>(n) >= outCap) return -1;
    return n;
}

Status Client::fetchToken(int64_t nowUtcSec) {
    if (!cfg_.clientEmail || cfg_.clientEmail[0] == '\0') {
        setError("no service account email");
        return Status::BadConfig;
    }
    googleError_[0] = '\0';

    // Build the assertion straight into the request buffer at the offset where
    // it belongs, then write the fixed prefix in front of it. One buffer rather
    // than two, and no copy of a kilobyte-long string.
    const size_t prefixLen = std::strlen(kTokenGrantPrefix);
    const int assertLen = buildAssertion(cfg_.clientEmail, cfg_.scope, cfg_.tokenUri,
                                         nowUtcSec, cfg_.assertionLifetimeSec, *signer_,
                                         requestBuf_ + prefixLen,
                                         sizeof requestBuf_ - prefixLen);
    if (assertLen < 0) {
        setError("could not sign the assertion");
        return Status::AuthFailed;
    }
    std::memcpy(requestBuf_, kTokenGrantPrefix, prefixLen);
    const size_t bodyLen = prefixLen + static_cast<size_t>(assertLen);

    TokenHandler h;
    h.token = token_;
    token_->clear();
    // The access token is far longer than the reader's built-in value buffer,
    // so the reader is given one big enough to hold it whole.
    json::Reader rd(h, tokenScratch_, sizeof tokenScratch_);
    net::JsonSink sink(rd);

    const net::Result r = http_->post(cfg_.tokenUri,
                                      "application/x-www-form-urlencoded",
                                      requestBuf_, bodyLen, nullptr, 0, sink);
    lastHttpStatus_ = r.status;

    if (r.error != net::Error::None && r.error != net::Error::Aborted) {
        token_->clear();
        setError(net::errorName(r.error));
        return Status::Transport;
    }
    if (!sink.finish()) {
        token_->clear();
        setError(rd.error());
        return Status::BadResponse;
    }

    if (h.error[0] != '\0') {
        // Google's own words are worth keeping: "invalid_grant" with
        // "Invalid JWT Signature" means the key is wrong, while the same code
        // with "JWT is expired" means the device clock is.
        core::copyStr(googleError_, sizeof googleError_, h.error, std::strlen(h.error));
        setError(h.errorDesc[0] ? "google rejected the assertion" : "google returned an error");
        token_->clear();
        return Status::AuthFailed;
    }
    if (r.status < 200 || r.status >= 300) {
        setError("unexpected http status from the token endpoint");
        token_->clear();
        return Status::HttpError;
    }
    if (!h.gotToken) {
        setError("no access token in the response");
        token_->clear();
        return Status::AuthFailed;
    }

    // Google sends expires_in, not an absolute time. If it is missing or
    // implausible, fall back to the assertion lifetime rather than trusting a
    // zero -- a zero would make every single request mint a new token.
    int32_t life = h.expiresIn;
    if (life <= 0 || life > kMaxAssertionLifetimeSec) life = kMaxAssertionLifetimeSec;

    token_->expiresAtUtcSec = nowUtcSec + life;
    setError("");
    return Status::Ok;
}

Status Client::ensureToken(int64_t nowUtcSec) {
    if (token_->usable(nowUtcSec, cfg_.tokenRefreshMarginSec)) return Status::Ok;
    return fetchToken(nowUtcSec);
}

Status Client::listEvents(const Query& q, int64_t nowUtcSec, EventList& out) {
    out.clear();
    if (!cfg_.calendarId || cfg_.calendarId[0] == '\0') {
        setError("no calendar id");
        return Status::BadConfig;
    }

    Status s = ensureToken(nowUtcSec);
    if (s != Status::Ok) return s;

    if (buildEventsUrl(cfg_, q, urlBuf_, sizeof urlBuf_) < 0) {
        setError("could not build the events url");
        return Status::BadConfig;
    }

    s = doListRequest(urlBuf_, out);
    if (s == Status::AuthFailed && lastHttpStatus_ == 401) {
        // A token can be revoked before it expires. One retry with a fresh one
        // is the difference between a blank screen and a working one; a second
        // would just be a loop against a credential Google has rejected.
        token_->clear();
        s = ensureToken(nowUtcSec);
        if (s != Status::Ok) { out.clear(); return s; }
        s = doListRequest(urlBuf_, out);
    }
    // Anything short of success hands back nothing. A truncated body that had
    // parsed two events before it stopped would otherwise reach a caller that
    // checks emptiness rather than status, and be rendered as the whole truth.
    if (s != Status::Ok) out.clear();
    return s;
}

Status Client::doListRequest(const char* url, EventList& out) {
    EventHandler h;
    h.out = &out;
    json::Reader rd(h);
    net::JsonSink sink(rd);

    const net::Header headers[] = {
        {"Authorization", token_->authorization()},
        {"Accept", "application/json"},
    };

    const net::Result r = http_->get(url, headers, 2, sink);
    lastHttpStatus_ = r.status;

    if (r.error != net::Error::None && r.error != net::Error::Aborted) {
        setError(net::errorName(r.error));
        return Status::Transport;
    }
    if (r.status == 401 || r.status == 403) {
        setError("google rejected the token");
        core::copyStr(googleError_, sizeof googleError_, h.error, std::strlen(h.error));
        return Status::AuthFailed;
    }
    if (r.status < 200 || r.status >= 300) {
        setError("unexpected http status");
        core::copyStr(googleError_, sizeof googleError_, h.error, std::strlen(h.error));
        return Status::HttpError;
    }
    // A body that was cut short is a failure, never a short list of events.
    if (!sink.finish()) {
        setError(rd.error());
        return Status::BadResponse;
    }
    setError("");
    return Status::Ok;
}

} // namespace google
