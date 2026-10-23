// json_stream.h - an incremental, allocation-free JSON reader.
//
// WHY THIS EXISTS
// ---------------
// A trip response from AVV is 67 KB on the wire, and the ESP32's largest
// contiguous allocation is 110,580 bytes (measured -- see band_renderer.h).
// Buffering the whole body would make the JSON document the single biggest
// allocation in the firmware, competing with TLS and the JPEG buffer for the
// one big DRAM region. So the body is never held whole: bytes are pushed into
// this reader as they arrive off the socket, in whatever chunks the transport
// hands over, and only the handful of fields we keep survive the pass.
//
// Fixed cost, whatever the document. Measured with sizeof on the host build:
//
//     json::Reader        1,440 B    (876 of it the path stack)
//     hafas::TripParser   2,792 B    reader + one leg being assembled
//     hafas::TripResult   5,968 B    six itineraries of six legs
//     ------------------------------
//     total               8,760 B    to parse a 36 KB response
//
// No heap, no <string>, no recursion, and the same 8.7 KB whether the response
// is 3 itineraries or 300.
//
// WHY PATHS RATHER THAN A HAND-ROLLED STATE MACHINE
// -------------------------------------------------
// The obvious cheap alternative -- scan for `"time":` and take what follows --
// is wrong on this data, and quietly so: a HAFAS leg carries Origin.time,
// Destination.time, and a whole JourneyDetail.Stops.Stop[].time list. The
// reader therefore tracks where it is, and the handler asks:
//
//     p.is("Trip[*].LegList.Leg[*].Origin.time")
//
// Unknown fields match nothing and cost nothing, which is what makes this
// tolerant of the schema drift we know about: the XSD we were given is 2.49.0
// while the server reports 2.51.0.
#pragma once
#include <cstddef>
#include <cstdint>

namespace json {

// Deepest observed path in a real AVV trip response is 9 components
// (Trip[0].LegList.Leg[1].Product[0].icon.foregroundColor.r). 24 is headroom,
// and going past it is survivable rather than fatal -- see Path::overflowed().
constexpr int    kMaxDepth    = 24;
constexpr size_t kMaxKeyLen   = 31;   // longest HAFAS key seen: minimumChangeDuration (21)
constexpr size_t kMaxValueLen = 127;  // stop names are <= 40; ctxRecon truncates, and may

enum class Type : uint8_t { Null, Bool, Number, String };

// Where the reader currently is, as the list of steps from the document root.
// Each step is either an object member (a key) or an array element (an index).
//
//   Trip[0].LegList.Leg[1].Origin.time
//   -> "Trip", [0], "LegList", "Leg", [1], "Origin", "time"   (7 components)
class Path {
public:
    int depth() const { return depth_; }

    // True if the document nested deeper than kMaxDepth at some point. Values
    // below that point are not delivered; everything shallower is unaffected.
    bool overflowed() const { return overflow_; }

    // Exactly one of these is meaningful per component: key() is "" for an
    // array element, index() is -1 for an object member.
    const char* key(int i) const {
        return (i >= 0 && i < depth_) ? comp_[i].key : "";
    }
    int index(int i) const {
        return (i >= 0 && i < depth_) ? comp_[i].index : -1;
    }

    // Match the whole path against a pattern:
    //   "Trip[*].LegList.Leg[*].Origin.time"
    // '*' matches any array index; a literal number matches only that index.
    // The pattern must account for every component -- a path one level deeper
    // does not match.
    bool is(const char* pattern) const { return match(pattern, false); }

    // Match only the leading components, so the rest of the path may be
    // anything. Use it to ask "am I somewhere inside this subtree?".
    bool startsWith(const char* pattern) const { return match(pattern, true); }

    // The index of the n-th array component, counting from the root, or -1.
    // For Trip[2]...Leg[4]..., nthIndex(0) == 2 and nthIndex(1) == 4.
    int nthIndex(int n) const;

    // The last component's key, or "" if the last component is an array index.
    const char* leaf() const { return key(depth_ - 1); }

private:
    friend class Reader;
    struct Comp {
        char key[kMaxKeyLen + 1];
        int  index;               // -1 for object members
    };

    bool match(const char* pattern, bool prefixOnly) const;

    Comp comp_[kMaxDepth] = {};
    int  depth_ = 0;
    int  buried_ = 0;             // levels entered past kMaxDepth
    bool overflow_ = false;
};

struct Handler {
    virtual ~Handler() = default;

    // A scalar value. `text` is NUL-terminated; for strings it is the decoded
    // content (escapes resolved, \uXXXX re-encoded as UTF-8). `truncated` is
    // set when the value was longer than kMaxValueLen -- the parse continues
    // correctly either way, but the text is short.
    virtual void onScalar(const Path& p, Type t, const char* text, size_t len,
                          bool truncated) = 0;

    // Entering / leaving an object or array. `p` is the path *to that
    // container*, so an array element's own index is the last component.
    virtual void onEnter(const Path& p, bool isArray) { (void)p; (void)isArray; }
    virtual void onLeave(const Path& p, bool isArray) { (void)p; (void)isArray; }
};

// Push bytes in; events come out. Feed as little or as much at a time as the
// transport gives you -- the reader resumes mid-token, mid-string, even
// mid-\uXXXX escape, and the tests assert that every possible chunking of a
// document produces identical events.
class Reader {
public:
    explicit Reader(Handler& h) : h_(&h), val_(inlineVal_), valCap_(kMaxValueLen) {}

    // Most documents are read with the built-in 128-byte value buffer, which is
    // ample for stop names and times. A caller that knows one field is long -- a
    // Google OAuth access token runs to several hundred characters -- supplies
    // its own buffer instead of every parse in the firmware paying for the
    // largest field any parse might see.
    // A buffer smaller than kMaxKeyLen would truncate member NAMES as well as
    // values -- keys are accumulated in the same buffer -- and a parse whose
    // keys are truncated matches no patterns while reporting success. Rather
    // than trust the caller's arithmetic, anything too small (or null) falls
    // back to the built-in buffer.
    Reader(Handler& h, char* valueBuf, size_t valueCap)
        : h_(&h),
          val_((valueBuf != nullptr && valueCap > kMaxKeyLen + 1) ? valueBuf : inlineVal_),
          valCap_((valueBuf != nullptr && valueCap > kMaxKeyLen + 1) ? valueCap - 1
                                                                    : kMaxValueLen) {}

    // val_ may point at inlineVal_, so a copy would share the original's
    // buffer. Nothing copies a Reader; make that structural rather than lucky.
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    // The usable value length for this reader: values longer than this are
    // truncated and flagged.
    size_t valueCapacity() const { return valCap_; }

    void reset();

    // Returns false the moment the document is malformed; further feeds are
    // ignored. error() then says what went wrong.
    bool feed(const char* data, size_t n);

    // Call after the last chunk. Returns false if the document ended early
    // (unbalanced braces, a string left open) -- a truncated HTTP body is a
    // real failure mode and must not look like a successful parse.
    bool finish();

    bool failed() const { return err_ != nullptr; }
    const char* error() const { return err_ ? err_ : ""; }
    const Path& path() const { return path_; }

private:
    // Containers may nest deeper than the path is tracked: the path clamps
    // (Path::overflowed()) while the balance check stays exact up to here.
    static constexpr int kMaxContainers = 64;

    enum class St : uint8_t {
        ValueStart, ArrStart, KeyStart, KeyNext, Colon, Str, StrEsc, StrU, Num,
        Lit, AfterValue, Done
    };

    bool step(char c);
    bool valueStartChar(char c);
    bool fail(const char* why);

    void pushKeyComp();
    void pushIndexComp(int idx);
    void popComp();

    bool beginContainer(bool isArray);
    bool endContainer(bool isArray);
    void valueDone();
    void emitScalar(Type t);
    void appendValue(char c);
    void appendUtf8(uint32_t cp);

    Handler* h_;
    Path path_;
    const char* err_ = nullptr;

    St st_ = St::ValueStart;
    bool inKey_ = false;             // the string being read is a member name

    // Container stack, parallel to the path but tracking container kind and
    // array counters, which the path does not need to expose.
    bool  stackIsArray_[kMaxContainers] = {};
    int   arrayIndex_[kMaxContainers] = {};
    int   stackTop_ = 0;             // number of open containers

    char   inlineVal_[kMaxValueLen + 1] = {0};
    char*  val_;
    size_t valCap_;
    size_t valLen_ = 0;
    bool   valTrunc_ = false;

    char   key_[kMaxKeyLen + 1] = {0};
    size_t keyLen_ = 0;

    char     uhex_[4] = {0};
    uint8_t  uhexLen_ = 0;
    uint32_t highSurrogate_ = 0;

    const char* lit_ = nullptr;      // remaining chars of true/false/null
    Type litType_ = Type::Null;
};

// Small helpers so handlers do not each roll their own. Both accept the text
// exactly as onScalar delivers it.
long toLong(const char* text, bool* ok = nullptr);
bool toBool(const char* text);

// Parse a JSON number. Hand-rolled rather than strtod: strtod is
// locale-sensitive, and a device that came up in a locale using ',' as the
// decimal separator would read 3.86 km as 3 km without saying anything.
//
// `ok` is false for text that is not a number at all. JSON null arrives as the
// literal "null", which is NOT zero -- a missing rain probability must not read
// as "no rain".
double toDouble(const char* text, bool* ok = nullptr);

} // namespace json
