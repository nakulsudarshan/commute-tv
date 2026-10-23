#include "timeutil.h"

namespace core {

int32_t daysFromCivil(int32_t y, uint32_t m, uint32_t d) {
    y -= m <= 2;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = static_cast<uint32_t>(y - era * 400);            // [0, 399]
    const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
    return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

void civilFromDays(int32_t z, int32_t& y, uint32_t& m, uint32_t& d) {
    z += 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int32_t yr = static_cast<int32_t>(yoe) + era * 400;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = yr + (m <= 2);
}

static bool digits(const char* s, int n, int32_t& out) {
    int32_t v = 0;
    for (int i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    out = v;
    return true;
}

int32_t parseDate(const char* s) {
    if (!s) return INT32_MIN;
    int32_t y, m, d;
    if (!digits(s, 4, y) || s[4] != '-') return INT32_MIN;
    if (!digits(s + 5, 2, m) || s[7] != '-') return INT32_MIN;
    if (!digits(s + 8, 2, d)) return INT32_MIN;
    if (m < 1 || m > 12 || d < 1 || d > 31) return INT32_MIN;
    // Timestamps are minutes in an int32, so days * 1440 must not overflow.
    // A calendar can hold an event dated 9999-12-31 -- a user can type one --
    // and that multiplication is undefined behaviour, not a large number.
    // Nothing this device does is meaningful outside this range anyway.
    if (y < 1970 || y > 2999) return INT32_MIN;
    return daysFromCivil(y, static_cast<uint32_t>(m), static_cast<uint32_t>(d));
}

int32_t parseTimeOfDay(const char* s) {
    if (!s) return -1;
    int32_t h, mi;
    if (!digits(s, 2, h) || s[2] != ':') return -1;
    if (!digits(s + 3, 2, mi)) return -1;
    // Seconds are accepted but ignored: we never need sub-minute precision and
    // rounding them in would make displayed countdowns inconsistent.
    if (h < 0 || h > 47 || mi < 0 || mi > 59) return -1;
    return h * 60 + mi;
}

Timestamp parseDateTime(const char* date, const char* time) {
    const int32_t days = parseDate(date);
    const int32_t tod = parseTimeOfDay(time);
    if (days == INT32_MIN || tod < 0) return invalidTs();
    return makeTs(days * 1440 + tod);
}

Timestamp parseIso8601Local(const char* s) {
    if (!s) return invalidTs();
    // "YYYY-MM-DDThh:mm..." -- parseDate and parseTimeOfDay both stop at the
    // end of the part they need, so the seconds and offset need no handling.
    if (s[10] != 'T' && s[10] != ' ') return invalidTs();
    return parseDateTime(s, s + 11);
}

int32_t parseIsoDuration(const char* s) {
    if (!s || s[0] != 'P') return -1;
    int32_t total = 0, acc = 0;
    bool inTime = false, any = false, sawDigit = false;
    for (const char* p = s + 1; *p; ++p) {
        if (*p == 'T') { inTime = true; sawDigit = false; acc = 0; continue; }
        if (*p >= '0' && *p <= '9') { acc = acc * 10 + (*p - '0'); sawDigit = true; continue; }
        if (!sawDigit) return -1;
        switch (*p) {
            case 'D': total += acc * 1440; break;
            case 'H': if (!inTime) return -1; total += acc * 60; break;
            case 'M': if (!inTime) return -1; total += acc;      break;  // month vs minute
            case 'S': if (!inTime) return -1; /* dropped on purpose */   break;
            default: return -1;
        }
        acc = 0; sawDigit = false; any = true;
    }
    return any ? total : -1;
}

void formatRfc3339(Timestamp t, int16_t utcOffsetMin, char* buf) {
    if (!t.valid()) { buf[0] = '\0'; return; }

    int32_t days = t.min / 1440;
    int32_t tod = t.min % 1440;
    if (tod < 0) { tod += 1440; --days; }

    int32_t y; uint32_t mo, d;
    civilFromDays(days, y, mo, d);
    const int32_t h = tod / 60, mi = tod % 60;

    const bool neg = utcOffsetMin < 0;
    const int32_t off = neg ? -utcOffsetMin : utcOffsetMin;

    char* p = buf;
    auto two = [&p](int32_t v) {
        *p++ = static_cast<char>('0' + (v / 10) % 10);
        *p++ = static_cast<char>('0' + v % 10);
    };
    // Year, four digits. Anything outside 0..9999 is not representable in
    // RFC 3339 and cannot arise from a calendar this device would read.
    *p++ = static_cast<char>('0' + (y / 1000) % 10);
    *p++ = static_cast<char>('0' + (y / 100) % 10);
    *p++ = static_cast<char>('0' + (y / 10) % 10);
    *p++ = static_cast<char>('0' + y % 10);
    *p++ = '-'; two(static_cast<int32_t>(mo));
    *p++ = '-'; two(static_cast<int32_t>(d));
    *p++ = 'T'; two(h);
    *p++ = ':'; two(mi);
    *p++ = ':'; two(0);
    *p++ = neg ? '-' : '+';
    two(off / 60);
    *p++ = ':'; two(off % 60);
    *p = '\0';
}

void formatHHMM(Timestamp t, char* buf) {
    if (!t.valid()) { buf[0] = '-'; buf[1] = '-'; buf[2] = ':'; buf[3] = '-'; buf[4] = '-'; buf[5] = 0; return; }
    int32_t tod = t.min % 1440;
    if (tod < 0) tod += 1440;
    const int32_t h = tod / 60, m = tod % 60;
    buf[0] = static_cast<char>('0' + (h / 10));
    buf[1] = static_cast<char>('0' + (h % 10));
    buf[2] = ':';
    buf[3] = static_cast<char>('0' + (m / 10));
    buf[4] = static_cast<char>('0' + (m % 10));
    buf[5] = 0;
}

} // namespace core
