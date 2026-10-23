// calendar.h - read the appointments that drive everything else.
//
// The device watches one calendar, owned by the service account and shared with
// Nakul. That direction is deliberate and was arrived at the hard way: sharing a
// consumer Google calendar TO a service account does not work -- the web UI
// drops the entry on reload and acl.insert returns a persistent 503. Reversing
// the ownership works and needs no OAuth consent screen, no refresh token, and
// nothing that silently expires. See memory.md.
//
// An event matters only if it has a location. That is the product rule, so it
// is applied here at the parse boundary rather than downstream: the slots are
// few, and filling them with lunch reminders that name no place would push out
// the appointment the screen actually needs.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../core/textutil.h"

#include "../core/types.h"
#include "../json/json_stream.h"
#include "../net/http.h"
#include "jwt.h"

namespace google {

// Google documents no maximum access-token length and asks callers not to
// assume one. 2 KB is the size their own guidance suggests planning for.
constexpr size_t kMaxTokenLen = 2048;

// The alert logic needs the next appointment and a little lookahead. Six is
// already more than a day's worth of events that name a place.
constexpr int kMaxEvents = 6;

// Every string here is borrowed, not owned -- the same convention as
// hafas::Endpoint. Whatever supplies them must outlive the Client.
struct Config {
    const char* clientEmail = "";       // ...@....iam.gserviceaccount.com
    const char* calendarId  = "";       // ...@group.calendar.google.com

    // Read-only on the device. The read/write scope exists only in the setup
    // tools; the firmware must never be able to change the calendar.
    const char* scope    = "https://www.googleapis.com/auth/calendar.readonly";
    const char* tokenUri = "https://oauth2.googleapis.com/token";
    const char* apiBase  = "https://www.googleapis.com/calendar/v3";

    // Asking for the response in the calendar's own zone means start.dateTime
    // comes back as local wall time, which is what the rest of this firmware
    // works in. Nothing has to convert anything.
    const char* timeZone = "Europe/Berlin";

    int32_t assertionLifetimeSec  = 3600;
    // Refresh this far before expiry. A token that expires between the check
    // and the request costs a round trip and a retry; five minutes of margin
    // costs nothing.
    int32_t tokenRefreshMarginSec = 300;
};

// Held as the complete Authorization header value -- "Bearer <token>" -- so no
// per-request buffer is needed to assemble one. On a 2 KB token that is 2 KB of
// stack not spent inside a TLS session.
struct Token {
    static constexpr size_t kPrefixLen = 7;      // "Bearer "
    char    header[kPrefixLen + kMaxTokenLen + 1] = {0};
    int64_t expiresAtUtcSec = 0;

    const char* authorization() const { return header; }
    const char* value() const { return header[0] ? header + kPrefixLen : header; }

    bool usable(int64_t nowUtcSec, int32_t marginSec) const {
        return header[0] != '\0' && nowUtcSec + marginSec < expiresAtUtcSec;
    }
    void clear() { header[0] = '\0'; expiresAtUtcSec = 0; }
    void set(const char* token, size_t len, int64_t expiresAtUtcSec_);
};

struct Query {
    core::Timestamp from, to;      // local, as everywhere else
    int16_t utcOffsetMin = 0;      // from localtime_r()'s tm_gmtoff
    int     maxResults = kMaxEvents;
};

struct EventList {
    core::CalendarEvent events[kMaxEvents];
    uint8_t count = 0;

    // Diagnostics for the health panel. `seen` counting far above `count` is
    // normal -- most calendar entries name no place. `more` means the window
    // held more matching events than we can carry, which is the only one of
    // these that could hide the appointment that mattered.
    uint8_t seen = 0;
    uint8_t skippedNoLocation = 0;
    uint8_t skippedAllDay = 0;
    uint8_t skippedCancelled = 0;
    uint8_t skippedNoStart = 0;
    bool    more = false;
    bool    textTruncated = false;

    bool empty() const { return count == 0; }

    // Cheaper than assigning from a temporary: EventList is 776 bytes, and the
    // temporary lands on the stack inside a TLS session.
    void clear() { *this = EventList(); }
};

enum class Status : uint8_t {
    Ok,
    BadConfig,      // no calendar id, no client email
    Transport,      // could not reach Google
    HttpError,      // a status we cannot act on
    AuthFailed,     // signing failed, or Google rejected the assertion
    BadResponse,    // malformed or truncated JSON
};

const char* statusName(Status s);

// Exposed for testing: the URL is where the easy mistakes live -- an unescaped
// '+' in an RFC 3339 offset silently becomes a space and shifts the window by
// an hour.
int buildEventsUrl(const Config& cfg, const Query& q, char* out, size_t outCap);

class Client {
public:
    Client(const Config& cfg, net::Client& http, Signer& signer, Token& token)
        : cfg_(cfg), http_(&http), signer_(&signer), token_(&token) {}

    // Mint a token if the held one is missing or close to expiry.
    Status ensureToken(int64_t nowUtcSec);

    // The whole job: token, request, parse. A 401 clears the token and retries
    // exactly once, because a token can be revoked before it expires and a
    // single retry is the difference between a blank screen and a working one.
    Status listEvents(const Query& q, int64_t nowUtcSec, EventList& out);

    int lastHttpStatus() const { return lastHttpStatus_; }
    const char* lastError() const { return lastError_; }

    // Set when Google returns an error document, e.g. "invalid_grant".
    const char* lastGoogleError() const { return googleError_; }

private:
    Status fetchToken(int64_t nowUtcSec);
    Status doListRequest(const char* url, EventList& out);

    Config cfg_;
    net::Client* http_;
    Signer* signer_;
    Token* token_;

    int lastHttpStatus_ = 0;
    // Copied, never aliased. An earlier version pointed lastError_ straight at
    // a char[] inside a handler that lived on the stack of the call -- the
    // message read fine in the non-sanitised build and was a use-after-return
    // the moment anyone actually looked at it. A buffer makes that impossible
    // rather than merely unlikely.
    void setError(const char* s) {
        core::copyStr(lastError_, sizeof lastError_, s ? s : "",
                      s ? std::strlen(s) : 0);
    }

    char lastError_[96] = {0};
    char googleError_[64] = {0};

    // The request URL has to stay live across the whole transfer, so it lives
    // here rather than on the stack beneath the TLS session.
    char urlBuf_[512] = {0};

    // Scratch for the token response, and for the token request body. Both are
    // members rather than locals, so the cost is visible in sizeof(Client)
    // instead of appearing as stack beneath a TLS session. Measured with
    // -fstack-usage on the host build, the deepest path (listEvents ->
    // fetchToken -> buildAssertion) still reaches about 2.5 KB of stack; give
    // the task that runs this at least 12 KB.
    char tokenScratch_[kMaxTokenLen + 1] = {0};
    char requestBuf_[1280] = {0};
};


} // namespace google
