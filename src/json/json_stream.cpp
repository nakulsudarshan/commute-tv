#include "json_stream.h"
#include <climits>
#include <cstring>

namespace json {

// ---------------------------------------------------------------- Path -----

int Path::nthIndex(int n) const {
    int seen = 0;
    for (int i = 0; i < depth_; ++i) {
        if (comp_[i].index >= 0) {
            if (seen == n) return comp_[i].index;
            ++seen;
        }
    }
    return -1;
}

bool Path::match(const char* pattern, bool prefixOnly) const {
    if (pattern == nullptr) return false;
    const char* p = pattern;
    int i = 0;

    while (*p != '\0') {
        if (*p == '.') { ++p; continue; }

        if (*p == '[') {                      // an array-index component
            ++p;
            if (i >= depth_) return false;
            const int idx = comp_[i].index;
            if (idx < 0) return false;         // this component is a key
            if (*p == '*') {
                ++p;
            } else {
                int want = 0;
                if (*p < '0' || *p > '9') return false;
                while (*p >= '0' && *p <= '9') {
                    want = want * 10 + (*p - '0');
                    ++p;
                }
                if (want != idx) return false;
            }
            if (*p != ']') return false;
            ++p;
            ++i;
            continue;
        }

        // a key component, running to the next '.' or '['
        const char* start = p;
        while (*p != '\0' && *p != '.' && *p != '[') ++p;
        const size_t n = static_cast<size_t>(p - start);
        if (i >= depth_) return false;
        if (comp_[i].index >= 0) return false;         // this component is an index
        if (std::strncmp(comp_[i].key, start, n) != 0) return false;
        if (comp_[i].key[n] != '\0') return false;     // key is longer than the pattern
        ++i;
    }

    return prefixOnly ? true : (i == depth_);
}

// -------------------------------------------------------------- Reader -----

void Reader::reset() {
    path_ = Path();
    err_ = nullptr;
    st_ = St::ValueStart;
    inKey_ = false;
    stackTop_ = 0;
    val_[0] = '\0'; valLen_ = 0; valTrunc_ = false;
    key_[0] = '\0'; keyLen_ = 0;
    uhexLen_ = 0; highSurrogate_ = 0;
    lit_ = nullptr; litType_ = Type::Null;
}

bool Reader::fail(const char* why) {
    if (err_ == nullptr) err_ = why;
    return false;
}

void Reader::pushKeyComp() {
    if (path_.depth_ < kMaxDepth) {
        std::memcpy(path_.comp_[path_.depth_].key, key_, keyLen_ + 1);
        path_.comp_[path_.depth_].index = -1;
        ++path_.depth_;
    } else {
        ++path_.buried_;
        path_.overflow_ = true;
    }
}

void Reader::pushIndexComp(int idx) {
    if (path_.depth_ < kMaxDepth) {
        path_.comp_[path_.depth_].key[0] = '\0';
        path_.comp_[path_.depth_].index = idx;
        ++path_.depth_;
    } else {
        ++path_.buried_;
        path_.overflow_ = true;
    }
}

void Reader::popComp() {
    if (path_.buried_ > 0) --path_.buried_;
    else if (path_.depth_ > 0) --path_.depth_;
}

void Reader::valueDone() {
    st_ = (stackTop_ == 0) ? St::Done : St::AfterValue;
}

bool Reader::beginContainer(bool isArray) {
    if (stackTop_ >= kMaxContainers) return fail("nesting too deep");
    if (path_.buried_ == 0) h_->onEnter(path_, isArray);
    stackIsArray_[stackTop_] = isArray;
    arrayIndex_[stackTop_] = 0;
    ++stackTop_;
    st_ = isArray ? St::ArrStart : St::KeyStart;
    return true;
}

bool Reader::endContainer(bool isArray) {
    if (stackTop_ == 0) return fail("unbalanced close");
    if (stackIsArray_[stackTop_ - 1] != isArray) return fail("mismatched close");
    --stackTop_;
    if (path_.buried_ == 0) h_->onLeave(path_, isArray);
    popComp();
    valueDone();
    return true;
}

void Reader::emitScalar(Type t) {
    val_[valLen_] = '\0';
    if (path_.buried_ == 0) h_->onScalar(path_, t, val_, valLen_, valTrunc_);
    popComp();
    valueDone();
}

void Reader::appendValue(char c) {
    if (valLen_ < valCap_) val_[valLen_++] = c;
    else valTrunc_ = true;
}

void Reader::appendUtf8(uint32_t cp) {
    // Encode as UTF-8. A lone surrogate or an embedded NUL is replaced with
    // U+FFFD: a NUL inside a C string would silently truncate a stop name, and
    // is never legitimate content here.
    if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;

    if (cp < 0x80) {
        appendValue(static_cast<char>(cp));
    } else if (cp < 0x800) {
        appendValue(static_cast<char>(0xC0 | (cp >> 6)));
        appendValue(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        appendValue(static_cast<char>(0xE0 | (cp >> 12)));
        appendValue(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        appendValue(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        appendValue(static_cast<char>(0xF0 | (cp >> 18)));
        appendValue(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        appendValue(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        appendValue(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

static bool isWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static bool isDigit(char c) { return c >= '0' && c <= '9'; }

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool Reader::valueStartChar(char c) {
    if (isWs(c)) return true;
    switch (c) {
        case '{': return beginContainer(false);
        case '[': return beginContainer(true);
        case '"':
            inKey_ = false; valLen_ = 0; valTrunc_ = false; highSurrogate_ = 0;
            st_ = St::Str;
            return true;
        case 't': lit_ = "rue";  litType_ = Type::Bool; valLen_ = 0; valTrunc_ = false;
                  appendValue('t'); st_ = St::Lit; return true;
        case 'f': lit_ = "alse"; litType_ = Type::Bool; valLen_ = 0; valTrunc_ = false;
                  appendValue('f'); st_ = St::Lit; return true;
        case 'n': lit_ = "ull";  litType_ = Type::Null; valLen_ = 0; valTrunc_ = false;
                  appendValue('n'); st_ = St::Lit; return true;
        default: break;
    }
    if (c == '-' || isDigit(c)) {
        valLen_ = 0; valTrunc_ = false;
        appendValue(c);
        st_ = St::Num;
        return true;
    }
    return fail("unexpected character where a value was expected");
}

bool Reader::step(char c) {
    switch (st_) {
    case St::ValueStart:
        return valueStartChar(c);

    case St::ArrStart:
        if (isWs(c)) return true;
        if (c == ']') return endContainer(true);
        pushIndexComp(arrayIndex_[stackTop_ - 1]);
        st_ = St::ValueStart;
        return valueStartChar(c);

    case St::KeyStart:
    case St::KeyNext:
        if (isWs(c)) return true;
        // '}' closes an empty object, but never one that has just seen a
        // comma: {"a":1,} is not JSON, and accepting it would hide a
        // truncated or mis-assembled body.
        if (c == '}') {
            if (st_ == St::KeyNext) return fail("trailing comma in object");
            return endContainer(false);
        }
        if (c == '"') {
            inKey_ = true; keyLen_ = 0; highSurrogate_ = 0;
            valLen_ = 0; valTrunc_ = false;
            st_ = St::Str;
            return true;
        }
        return fail("expected a member name");

    case St::Colon:
        if (isWs(c)) return true;
        if (c == ':') { st_ = St::ValueStart; return true; }
        return fail("expected ':' after a member name");

    case St::Str:
        if (highSurrogate_ != 0 && c != '\\') { appendUtf8(0xFFFD); highSurrogate_ = 0; }
        if (c == '"') {
            if (highSurrogate_ != 0) { appendUtf8(0xFFFD); highSurrogate_ = 0; }
            if (inKey_) {
                keyLen_ = valLen_ < kMaxKeyLen ? valLen_ : kMaxKeyLen;
                std::memcpy(key_, val_, keyLen_);
                key_[keyLen_] = '\0';
                pushKeyComp();
                st_ = St::Colon;
            } else {
                emitScalar(Type::String);
            }
            return true;
        }
        if (c == '\\') { st_ = St::StrEsc; return true; }
        if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
        appendValue(c);
        return true;

    case St::StrEsc: {
        char out = 0;
        switch (c) {
            case '"':  out = '"';  break;
            case '\\': out = '\\'; break;
            case '/':  out = '/';  break;
            case 'b':  out = '\b'; break;
            case 'f':  out = '\f'; break;
            case 'n':  out = '\n'; break;
            case 'r':  out = '\r'; break;
            case 't':  out = '\t'; break;
            case 'u':  uhexLen_ = 0; st_ = St::StrU; return true;
            default:   return fail("bad escape in string");
        }
        if (highSurrogate_ != 0) { appendUtf8(0xFFFD); highSurrogate_ = 0; }
        appendValue(out);
        st_ = St::Str;
        return true;
    }

    case St::StrU: {
        if (hexVal(c) < 0) return fail("bad \\u escape");
        uhex_[uhexLen_++] = c;
        if (uhexLen_ < 4) return true;
        uint32_t cp = 0;
        for (int i = 0; i < 4; ++i)
            cp = (cp << 4) | static_cast<uint32_t>(hexVal(uhex_[i]));
        uhexLen_ = 0;
        st_ = St::Str;

        const bool isHigh = (cp >= 0xD800 && cp <= 0xDBFF);
        const bool isLow  = (cp >= 0xDC00 && cp <= 0xDFFF);
        if (highSurrogate_ != 0) {
            if (isLow) {
                appendUtf8(0x10000u + ((highSurrogate_ - 0xD800u) << 10) + (cp - 0xDC00u));
                highSurrogate_ = 0;
            } else {
                appendUtf8(0xFFFD);              // orphaned high surrogate
                highSurrogate_ = isHigh ? cp : 0;
                if (!isHigh) appendUtf8(cp);
            }
        } else if (isHigh) {
            highSurrogate_ = cp;
        } else {
            appendUtf8(cp);                       // a lone low surrogate folds to U+FFFD
        }
        return true;
    }

    case St::Num:
        if (isDigit(c) || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
            appendValue(c);
            return true;
        }
        emitScalar(Type::Number);
        return step(c);                            // the terminator belongs to the next state

    case St::Lit:
        if (*lit_ == '\0' || c != *lit_) return fail("bad literal");
        appendValue(c);
        ++lit_;
        if (*lit_ == '\0') emitScalar(litType_);
        return true;

    case St::AfterValue:
        if (isWs(c)) return true;
        if (c == ',') {
            if (stackTop_ == 0) return fail("',' outside a container");
            if (stackIsArray_[stackTop_ - 1]) {
                pushIndexComp(++arrayIndex_[stackTop_ - 1]);
                st_ = St::ValueStart;
            } else {
                st_ = St::KeyNext;
            }
            return true;
        }
        if (c == '}') return endContainer(false);
        if (c == ']') return endContainer(true);
        return fail("expected ',' or a closing bracket");

    case St::Done:
        if (isWs(c)) return true;
        return fail("trailing data after the document");
    }
    return fail("internal state error");
}

bool Reader::feed(const char* data, size_t n) {
    if (err_ != nullptr) return false;
    if (data == nullptr) return n == 0;
    for (size_t i = 0; i < n; ++i) {
        if (!step(data[i])) return false;
    }
    return true;
}

bool Reader::finish() {
    if (err_ != nullptr) return false;
    if (st_ == St::Num) emitScalar(Type::Number);   // a number ends at EOF
    if (st_ != St::Done) return fail("document ended early");
    return true;
}

// ------------------------------------------------------------- helpers -----

long toLong(const char* text, bool* ok) {
    if (ok) *ok = false;
    if (text == nullptr || *text == '\0') return 0;
    const char* p = text;
    bool neg = false;
    if (*p == '-') { neg = true; ++p; }
    else if (*p == '+') { ++p; }
    if (!isDigit(*p)) return 0;
    // Saturating, because `long` is 32 bits on the ESP32 and a wrapped value is
    // the worst kind of wrong: "expires_in": 4294967596 would wrap to 300 and
    // look entirely plausible.
    long v = 0;
    bool overflowed = false;
    while (isDigit(*p)) {
        const int d = *p - '0';
        if (v > (LONG_MAX - d) / 10) { overflowed = true; }
        else { v = v * 10 + d; }
        ++p;
    }
    if (overflowed) {
        if (ok) *ok = false;
        return neg ? LONG_MIN : LONG_MAX;
    }
    // Anything trailing (a fractional part, an exponent) means this was not an
    // integer; report the integral part but do not claim success.
    if (ok) *ok = (*p == '\0');
    return neg ? -v : v;
}

double toDouble(const char* text, bool* ok) {
    if (ok) *ok = false;
    if (text == nullptr || *text == '\0') return 0.0;

    const char* p = text;
    bool neg = false;
    if (*p == '-') { neg = true; ++p; }
    else if (*p == '+') { ++p; }
    if (!isDigit(*p) && *p != '.') return 0.0;

    double v = 0.0;
    while (isDigit(*p)) { v = v * 10.0 + (*p - '0'); ++p; }

    if (*p == '.') {
        ++p;
        double scale = 0.1;
        while (isDigit(*p)) { v += (*p - '0') * scale; scale *= 0.1; ++p; }
    }

    if (*p == 'e' || *p == 'E') {
        ++p;
        bool expNeg = false;
        if (*p == '-') { expNeg = true; ++p; }
        else if (*p == '+') { ++p; }
        if (!isDigit(*p)) return 0.0;
        int e = 0;
        while (isDigit(*p) && e < 400) { e = e * 10 + (*p - '0'); ++p; }
        double f = 1.0;
        for (int i = 0; i < e; ++i) f *= 10.0;
        if (expNeg) v /= f; else v *= f;
    }

    if (*p != '\0') return 0.0;          // trailing junk: not a number
    if (ok) *ok = true;
    return neg ? -v : v;
}

bool toBool(const char* text) {
    return text != nullptr && text[0] == 't';
}

} // namespace json
