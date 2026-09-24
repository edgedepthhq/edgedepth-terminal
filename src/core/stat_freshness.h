#pragma once
// Per-field freshness for the Stats stream.
//
// Legacy publishers state no clocks: proto3 cannot distinguish "zero" from
// "absent", so widgets carry the previous value forward. A publisher that
// states per-field clocks is authoritative: a zero value with a zero clock is
// unavailable, and an old clock is stale. Nothing here invents a value.
#include <cstdint>
#include <cstdio>

namespace StatFreshness {

// Works on Terminal::Stat and on any struct carrying the same clock fields, so
// the native test does not drag the JSON-dependent types header in.
template <class Stat>
inline bool stated(const Stat& s) {
    return s.mark_price_ms > 0 || s.funding_ms > 0 || s.open_interest_ms > 0 || s.funding_interval_minutes > 0;
}

enum class State { Legacy, Fresh, Stale, Unavailable };

struct Field {
    State state = State::Legacy;
    int64_t age_ms = 0;
};

// clock_ms is the field's stated observation clock (0 = not stated). The
// message-level `stated` flag decides whether 0 means legacy or unavailable.
inline Field field(bool message_stated, int64_t clock_ms, int64_t now_ms, int64_t stale_after_ms) {
    Field f;
    if (!message_stated) return f;
    if (clock_ms <= 0) { f.state = State::Unavailable; return f; }
    f.age_ms = now_ms > clock_ms ? now_ms - clock_ms : 0;
    f.state = f.age_ms > stale_after_ms ? State::Stale : State::Fresh;
    return f;
}

// "8H", "4H", "1H", "30M"; empty when the cadence is not stated; "NONE" for a
// contract that states zero (dated futures without funding).
inline const char* cadence_label(bool message_stated, int32_t minutes, char* buf, size_t size) {
    if (!message_stated) { buf[0] = '\0'; return buf; }
    if (minutes <= 0) { std::snprintf(buf, size, "NONE"); return buf; }
    if (minutes % 60 == 0) std::snprintf(buf, size, "%dH", minutes / 60);
    else std::snprintf(buf, size, "%dM", minutes);
    return buf;
}

inline void age_label(int64_t age_ms, char* buf, size_t size) {
    const int64_t s = age_ms / 1000;
    if (s < 60) std::snprintf(buf, size, "%llds", static_cast<long long>(s));
    else if (s < 3600) std::snprintf(buf, size, "%lldm", static_cast<long long>(s / 60));
    else std::snprintf(buf, size, "%lldh", static_cast<long long>(s / 3600));
}

}  // namespace StatFreshness
