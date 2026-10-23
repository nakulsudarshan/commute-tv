#include "hafas_request.h"

namespace hafas {

static const char* kDemo = "https://avv.demo.hafas.de/restproxy/";
static const char* kProd = "https://auskunft.avv.de/restproxy/";

const char* Endpoint::baseUrl() const {
    if (baseUrlOverride) return baseUrlOverride;
    return env == Env::Prod ? kProd : kDemo;
}

int formatFixed(double v, int decimals, char* out, size_t outSize) {
    if (!out || outSize < 2 || decimals < 0 || decimals > 9) return -1;
    size_t n = 0;
    bool neg = v < 0;
    if (neg) v = -v;

    // Round at the requested precision before splitting, otherwise 1.99999
    // with 3 decimals renders as "1.999" rather than "2.000".
    double scale = 1.0;
    for (int i = 0; i < decimals; ++i) scale *= 10.0;
    long long scaled = static_cast<long long>(v * scale + 0.5);
    long long ip = scaled / static_cast<long long>(scale);
    long long fp = scaled % static_cast<long long>(scale);

    char tmp[32];
    int t = 0;
    if (ip == 0) tmp[t++] = '0';
    while (ip > 0 && t < 30) { tmp[t++] = static_cast<char>('0' + ip % 10); ip /= 10; }

    if (neg) { if (n + 1 >= outSize) return -1; out[n++] = '-'; }
    while (t > 0) { if (n + 1 >= outSize) return -1; out[n++] = tmp[--t]; }

    if (decimals > 0) {
        if (n + 1 >= outSize) return -1;
        out[n++] = '.';
        for (int i = decimals - 1; i >= 0; --i) {
            long long d = fp;
            for (int k = 0; k < i; ++k) d /= 10;
            if (n + 1 >= outSize) return -1;
            out[n++] = static_cast<char>('0' + (d % 10));
        }
    }
    out[n] = 0;
    return static_cast<int>(n);
}

namespace {
struct Appender {
    char* out; size_t cap; size_t n = 0; bool ok = true;
    Appender(char* o, size_t c) : out(o), cap(c) { if (cap) out[0] = 0; else ok = false; }
    void str(const char* s) {
        if (!ok || !s) return;
        for (const char* p = s; *p; ++p) {
            if (n + 1 >= cap) { ok = false; return; }
            out[n++] = *p;
        }
        out[n] = 0;
    }
    void num(long long v) {
        if (!ok) return;
        char tmp[24]; int t = 0;
        bool neg = v < 0; if (neg) v = -v;
        if (v == 0) tmp[t++] = '0';
        while (v > 0 && t < 22) { tmp[t++] = static_cast<char>('0' + v % 10); v /= 10; }
        if (neg) { if (n + 1 >= cap) { ok = false; return; } out[n++] = '-'; }
        while (t > 0) { if (n + 1 >= cap) { ok = false; return; } out[n++] = tmp[--t]; }
        out[n] = 0;
    }
    void two(int32_t v) {                     // zero-padded, for dates and times
        if (!ok) return;
        if (n + 2 >= cap) { ok = false; return; }
        out[n++] = static_cast<char>('0' + (v / 10) % 10);
        out[n++] = static_cast<char>('0' + v % 10);
        out[n] = 0;
    }
    void coord(double v) {
        if (!ok) return;
        char buf[24];
        // 6 decimals is ~0.1 m -- far beyond what a bus stop needs, and keeps
        // the URL short.
        if (formatFixed(v, 6, buf, sizeof buf) < 0) { ok = false; return; }
        str(buf);
    }
    void encoded(const char* s) {
        if (!ok) return;
        char buf[256];
        if (urlEncode(s, buf, sizeof buf) < 0) { ok = false; return; }
        str(buf);
    }
};
} // namespace

static void appendDate(Appender& a, int32_t days) {
    int32_t y; uint32_t m, d;
    core::civilFromDays(days, y, m, d);
    a.num(y); a.str("-"); a.two(static_cast<int32_t>(m)); a.str("-"); a.two(static_cast<int32_t>(d));
}

static void appendTime(Appender& a, int32_t tod) {
    a.two(tod / 60); a.str(":"); a.two(tod % 60);
}

int buildTripUrl(const Endpoint& ep, const TripQuery& q, char* out, size_t outSize) {
    Appender a(out, outSize);
    a.str(ep.baseUrl());
    a.str("trip?accessId="); a.encoded(ep.accessId);
    a.str("&format=json");
    a.str("&originCoordLat=");  a.coord(q.originLat);
    a.str("&originCoordLong="); a.coord(q.originLon);
    a.str("&destCoordLat=");    a.coord(q.destLat);
    a.str("&destCoordLong=");   a.coord(q.destLon);
    a.str("&date="); appendDate(a, q.dateDays);
    a.str("&time="); appendTime(a, q.timeOfDayMin);
    a.str("&searchForArrival="); a.str(q.searchForArrival ? "1" : "0");
    a.str("&rtMode="); a.encoded(q.rtMode);
    if (q.originWalk) a.str("&originWalk=1");
    if (q.destWalk)   a.str("&destWalk=1");
    a.str("&numF="); a.num(q.numF < 1 ? 1 : (q.numF > 6 ? 6 : q.numF));
    // Explicitly off: these default to 0 for trip, but stating it protects us
    // if a future server build flips the defaults and quietly inflates every
    // response beyond what the parser budget expects.
    a.str("&poly=0&passlist=0");
    // Explicitly off because their defaults are NOT 0 -- withFreq defaults to 1
    // and tariff/trafficMessages default to whatever the operator provisioned.
    // Freq alone was half of a real response.
    a.str("&withFreq="); a.str(q.withFreq ? "1" : "0");
    a.str("&tariff="); a.str(q.tariff ? "1" : "0");
    a.str("&trafficMessages="); a.str(q.trafficMessages ? "1" : "0");
    return a.ok ? static_cast<int>(a.n) : -1;
}

int buildLocationNameUrl(const Endpoint& ep, const char* input, int maxNo,
                         char* out, size_t outSize) {
    Appender a(out, outSize);
    a.str(ep.baseUrl());
    a.str("location.name?accessId="); a.encoded(ep.accessId);
    a.str("&format=json");
    a.str("&input="); a.encoded(input);
    a.str("&maxNo="); a.num(maxNo < 1 ? 1 : maxNo);
    a.str("&attributes=0&infotexts=0");
    return a.ok ? static_cast<int>(a.n) : -1;
}

int buildJourneyPosUrl(const Endpoint& ep, const char* journeyRef,
                       char* out, size_t outSize) {
    Appender a(out, outSize);
    a.str(ep.baseUrl());
    a.str("journeyPos?accessId="); a.encoded(ep.accessId);
    a.str("&format=json");
    a.str("&id="); a.encoded(journeyRef);
    return a.ok ? static_cast<int>(a.n) : -1;
}

} // namespace hafas
