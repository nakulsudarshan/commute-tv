#include "hafas_parse.h"

#include <cstring>

#include "../core/textutil.h"
#include "../core/timeutil.h"

namespace hafas {
namespace {

using core::LegMode;

bool ieqz(const char* a, const char* b) {
    for (;; ++a, ++b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = static_cast<char>(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = static_cast<char>(cb - 32);
        if (ca != cb) return false;
        if (ca == '\0') return true;
    }
}

bool istarts(const char* s, const char* prefix) {
    for (;; ++s, ++prefix) {
        if (*prefix == '\0') return true;
        char cs = *s, cp = *prefix;
        if (cs >= 'a' && cs <= 'z') cs = static_cast<char>(cs - 32);
        if (cp >= 'a' && cp <= 'z') cp = static_cast<char>(cp - 32);
        if (cs != cp) return false;
    }
}

// Copy a scalar into a fixed field, folding to ASCII, and note truncation.
template <size_t N>
void setFolded(char (&dst)[N], const char* text, size_t len, bool* truncFlag) {
    if (!core::foldInto(dst, N, text, len) && truncFlag) *truncFlag = true;
}

template <size_t N>
void setRaw(char (&dst)[N], const char* text, size_t len) {
    core::copyStr(dst, N, text, len);
}

} // namespace

// The mapping is deliberately small. LegMode drives icon choice and the
// walk/transit split -- nothing else -- so an unrecognised category landing on
// Other is harmless, while mistaking a transit leg for a walk would not be.
// Every code below was observed in live AVV data for Aachen except where
// marked; the rest are the neighbouring categories the same feed can emit.
core::LegMode legModeFor(const char* catOut, const char* catOutS) {
    struct Row { const char* code; LegMode mode; };
    static const Row kShortCodes[] = {
        {"NFB", LegMode::Bus},    // observed: low-floor bus
        {"BUS", LegMode::Bus},
        {"NB",  LegMode::Bus},
        {"SEV", LegMode::Bus},    // rail replacement
        {"AST", LegMode::Bus},    // dial-a-ride
        {"NET", LegMode::Bus},    // observed: ASEAG Netliner, an on-demand shuttle
        {"STR", LegMode::Tram},
        {"NST", LegMode::Tram},
        {"TRM", LegMode::Tram},
        {"U",   LegMode::Tram},
        {"NU",  LegMode::Tram},
        {"NRB", LegMode::Train},  // observed: Regionalbahn
        {"RB",  LegMode::Train},
        {"RE",  LegMode::Train},
        {"NRE", LegMode::Train},
        {"S",   LegMode::Train},
        {"NS",  LegMode::Train},
        {"IC",  LegMode::Train},
        {"ICE", LegMode::Train},
        {"EC",  LegMode::Train},
    };

    if (catOutS != nullptr && catOutS[0] != '\0') {
        for (const Row& r : kShortCodes)
            if (ieqz(catOutS, r.code)) return r.mode;
    }

    if (catOut != nullptr && catOut[0] != '\0') {
        // Long names, matched on their leading word. "Stra" catches
        // Strassenbahn whether or not the sharp s survived folding.
        if (istarts(catOut, "bus") || istarts(catOut, "netliner")) return LegMode::Bus;
        if (istarts(catOut, "stra") || istarts(catOut, "tram")) return LegMode::Tram;
        if (istarts(catOut, "rb") || istarts(catOut, "re") || istarts(catOut, "rrx") ||
            istarts(catOut, "ic") || istarts(catOut, "ec") || istarts(catOut, "zug") ||
            istarts(catOut, "bahn") || istarts(catOut, "train")) return LegMode::Train;
    }

    return LegMode::Other;
}

// ---------------------------------------------------------- TripParser -----

void TripParser::beginTrip(int idx) {
    out_->tripsSeen = static_cast<uint8_t>(out_->tripsSeen + 1);
    if (idx < 0 || idx >= core::kMaxCandidates) { inTrip_ = false; return; }
    cur_ = core::Itinerary{};
    inTrip_ = true;
    legOverflow_ = false;
    anyLegCancelled_ = false;
    allReachable_ = true;
}

void TripParser::endTrip() {
    if (!inTrip_) return;
    inTrip_ = false;

    if (legOverflow_ || cur_.legCount == 0) {
        // A truncated itinerary is worse than no itinerary: the arrival time
        // comes from the last leg, so keeping the first kMaxLegs would show a
        // journey that arrives earlier than it really does.
        out_->tripsDropped = static_cast<uint8_t>(out_->tripsDropped + 1);
        return;
    }

    cur_.cancelled = cur_.cancelled || anyLegCancelled_;
    cur_.reachable = allReachable_;

    if (out_->tripCount < core::kMaxCandidates) {
        out_->trips[out_->tripCount] = cur_;
        out_->tripCount = static_cast<uint8_t>(out_->tripCount + 1);
    }
}

void TripParser::beginLeg(int idx) {
    if (!inTrip_) return;
    if (idx < 0 || idx >= core::kMaxLegs) { legOverflow_ = true; inLeg_ = false; return; }

    leg_ = core::Leg{};
    org_ = StopTimes{};
    dst_ = StopTimes{};
    catOut_[0] = catOutS_[0] = prodLine_[0] = prodDisplayNum_[0] = legName_[0] = '\0';
    isWalk_ = false;
    inLeg_ = true;
}

void TripParser::endLeg() {
    if (!inLeg_) return;
    inLeg_ = false;

    leg_.plannedDep = core::parseDateTime(org_.date, org_.time);
    leg_.plannedArr = core::parseDateTime(dst_.date, dst_.time);
    // A realtime time may fall on a different date from the planned one when a
    // delay pushes a departure past midnight, so rtDate wins when it is there.
    leg_.rtDep = core::parseDateTime(org_.rtDate[0] ? org_.rtDate : org_.date, org_.rtTime);
    leg_.rtArr = core::parseDateTime(dst_.rtDate[0] ? dst_.rtDate : dst_.date, dst_.rtTime);

    leg_.mode = isWalk_ ? LegMode::Walk : legModeFor(catOut_, catOutS_);

    // "47" beats "47" spelled as a display number beats "Bus 47": the screen
    // has 12 characters for this and the number is the part that identifies the
    // vehicle at the stop.
    const char* line = prodLine_[0] ? prodLine_
                     : (prodDisplayNum_[0] ? prodDisplayNum_ : legName_);
    if (!isWalk_) setFolded(leg_.line, line, std::strlen(line), &out_->textTruncated);

    if (leg_.cancelled) anyLegCancelled_ = true;

    if (cur_.legCount < core::kMaxLegs) {
        cur_.legs[cur_.legCount] = leg_;
        cur_.legCount = static_cast<uint8_t>(cur_.legCount + 1);
    } else {
        legOverflow_ = true;
    }
}

void TripParser::onEnter(const json::Path& p, bool isArray) {
    if (isArray) return;
    if (p.depth() == 2 && p.is("Trip[*]")) beginTrip(p.index(1));
    else if (p.depth() == 5 && p.is("Trip[*].LegList.Leg[*]")) beginLeg(p.index(4));
}

void TripParser::onLeave(const json::Path& p, bool isArray) {
    if (isArray) return;
    if (p.depth() == 5 && p.is("Trip[*].LegList.Leg[*]")) endLeg();
    else if (p.depth() == 2 && p.is("Trip[*]")) endTrip();
}

void TripParser::envelopeField(const char* key, const char* text, size_t len) {
    if (std::strcmp(key, "serverVersion") == 0)       setRaw(out_->env.serverVersion, text, len);
    else if (std::strcmp(key, "dialectVersion") == 0) setRaw(out_->env.dialectVersion, text, len);
    else if (std::strcmp(key, "planRtTs") == 0)       out_->env.planRtTs = core::parseIso8601Local(text);
    else if (std::strcmp(key, "errorCode") == 0)      setRaw(out_->env.errorCode, text, len);
    else if (std::strcmp(key, "errorText") == 0)      setFolded(out_->env.errorText, text, len, nullptr);
}

void TripParser::tripField(const char* key, json::Type t, const char* text) {
    if (!inTrip_) return;
    if (std::strcmp(key, "transferCount") == 0) {
        cur_.transfers = static_cast<int32_t>(json::toLong(text));
    } else if (std::strcmp(key, "cancelled") == 0 && t == json::Type::Bool) {
        cur_.cancelled = json::toBool(text);
    }
}

void TripParser::legField(const json::Path& p, json::Type t, const char* text, size_t len) {
    if (!inLeg_) return;

    const char* a = p.key(5);            // the member directly under the leg
    const int   d = p.depth();

    if (d == 6) {
        if (std::strcmp(a, "type") == 0) {
            isWalk_ = (std::strcmp(text, "WALK") == 0);
        } else if (std::strcmp(a, "direction") == 0) {
            // Not flagged as truncation: a direction is a route description
            // ("Ponttor - Hauptbahnhof - Schanz - Uniklinik", 43 characters in
            // a recorded response) and routinely exceeds the field. Flagging
            // it would leave the flag permanently set and therefore useless.
            setFolded(leg_.direction, text, len, nullptr);
        } else if (std::strcmp(a, "name") == 0) {
            setRaw(legName_, text, len);
        } else if (std::strcmp(a, "cancelled") == 0 && t == json::Type::Bool) {
            leg_.cancelled = json::toBool(text);
        } else if (std::strcmp(a, "reachable") == 0 && t == json::Type::Bool) {
            // Absent on walk legs and on the first leg of a journey; only a
            // present-and-false is a reason to distrust the itinerary.
            if (!json::toBool(text)) allReachable_ = false;
        }
        return;
    }

    if (d == 7 && (std::strcmp(a, "Origin") == 0 || std::strcmp(a, "Destination") == 0)) {
        const bool origin = (a[0] == 'O');
        StopTimes& st = origin ? org_ : dst_;
        const char* f = p.key(6);
        if      (std::strcmp(f, "date")   == 0) setRaw(st.date,   text, len);
        else if (std::strcmp(f, "time")   == 0) setRaw(st.time,   text, len);
        else if (std::strcmp(f, "rtDate") == 0) setRaw(st.rtDate, text, len);
        else if (std::strcmp(f, "rtTime") == 0) setRaw(st.rtTime, text, len);
        else if (std::strcmp(f, "name")   == 0) {
            setFolded(origin ? leg_.fromName : leg_.toName, text, len, &out_->textTruncated);
        }
        return;
    }

    // Product is an array of one in every response seen; the object form is
    // accepted as well so a flattened dialect would not silently lose the line
    // number.
    if (std::strcmp(a, "Product") != 0) return;
    const char* f = nullptr;
    if (d == 7)                            f = p.key(6);   // Product.line
    else if (d == 8 && p.index(6) == 0)    f = p.key(7);   // Product[0].line
    if (f == nullptr) return;

    if      (std::strcmp(f, "catOut")        == 0) setRaw(catOut_, text, len);
    else if (std::strcmp(f, "catOutS")       == 0) setRaw(catOutS_, text, len);
    else if (std::strcmp(f, "line")          == 0) setRaw(prodLine_, text, len);
    else if (std::strcmp(f, "displayNumber") == 0) setRaw(prodDisplayNum_, text, len);
}

void TripParser::onScalar(const json::Path& p, json::Type t, const char* text,
                          size_t len, bool truncated) {
    (void)truncated;   // only flagged for fields we keep; ctxRecon always overflows

    const int d = p.depth();
    if (d == 1 && p.index(0) < 0) { envelopeField(p.key(0), text, len); return; }
    if (d < 3 || !p.startsWith("Trip[*]")) return;
    if (d == 3) { tripField(p.key(2), t, text); return; }
    if (d >= 6 && p.startsWith("Trip[*].LegList.Leg[*]")) legField(p, t, text, len);
}

bool TripParser::finish() {
    if (!rd_.finish()) return false;
    return true;
}

bool parseTripResponse(const char* body, size_t len, TripResult& out,
                       const char** err) {
    TripParser tp(out);
    const bool okFeed = tp.feed(body, len);
    const bool okDone = okFeed && tp.finish();
    if (err) *err = okDone ? "" : tp.error();
    return okDone;
}

} // namespace hafas
