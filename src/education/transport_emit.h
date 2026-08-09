#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// transport_emit.h — shared C++→React transport-emit mechanics.
//
// LessonRuntime::emit_state and StudioRuntime::emit_state both push a transport
// snapshot to the React chrome the same way: hash the DISCRETE fields (clock/progress
// excluded) to dirty-check, re-emit the clock on a fixed cadence while playing, hold a
// monotonic clock floor against catch-up jitter, and dispatch a CustomEvent carrying
// the JSON. Those mechanics live here ONCE; each runtime supplies only WHICH fields it
// hashes and WHAT JSON it builds (those genuinely differ — lesson carries steps/cards/
// spotlight; studio carries symbol/tf/window).
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace edu::transport {

// FNV-1a incremental hasher for the discrete-state emit signature. Mix each discrete
// field; value() is the signature compared against the last emit.
struct SigHasher {
    uint64_t h = 1469598103934665603ULL;  // FNV-1a 64-bit offset basis
    void mix(uint64_t v) {
        for (int i = 0; i < 8; ++i) { h ^= (v & 0xff); h *= 1099511628211ULL; v >>= 8; }
    }
    [[nodiscard]] uint64_t value() const { return h; }
};

// Clock re-anchor cadence (ms). The discrete dirty-check excludes the clock, so a long
// steady "playing" stretch would emit once and never re-anchor React — leaving it to
// dead-reckon clockMs + wall×speed open-loop and drift. Re-emit on this cadence while
// playing so React re-anchors to the self-correcting interpolated_time_ms().
inline constexpr double kClockEmitIntervalMs = 250.0;

// Monotonic clock floor. During a steady playing stretch, hold the REPORTED clock at
// its running max to absorb the small backward steps interpolated_time_ms() takes when
// the backend status anchor lags the wall-time extrapolation (notably the initial
// catch-up) — which otherwise snap the React clock 00:01→00:00 on the 250ms cadence. A
// LARGE backward jump (a real seek/rewind) passes through and re-baselines; the floor
// also re-baselines whenever not steadily playing (loading/seeking/paused/idle), so a
// backward scrub that routes through Seeking is honored. Returns the clamped clock and
// updates `floor` in place.
inline int64_t apply_clock_floor(int64_t now_ms, int64_t& floor,
                                 bool active, bool playing, bool paused, bool loading) {
    constexpr int64_t kBackwardTolMs = 2500;
    if (active && playing && !paused && !loading) {
        if (now_ms < floor && (floor - now_ms) <= kBackwardTolMs) now_ms = floor;
        floor = now_ms;
    } else {
        floor = now_ms;
    }
    return now_ms;
}

// Emit gate: returns true if a snapshot should be sent this frame — on a discrete
// change (sig != last_sig) OR on the clock cadence while actively playing. On a true
// return it commits last_sig + last_clock_emit_wall so the caller just builds + sends.
inline bool should_emit(uint64_t sig, uint64_t& last_sig,
                        bool active, bool playing, bool paused, bool loading,
                        double now_wall, double& last_clock_emit_wall) {
    const bool discrete_changed = (sig != last_sig);
    const bool clock_due = active && playing && !paused && !loading &&
                           (now_wall - last_clock_emit_wall) >= kClockEmitIntervalMs;
    if (!discrete_changed && !clock_due) return false;
    last_sig = sig;
    last_clock_emit_wall = now_wall;
    return true;
}

// Push a state snapshot to the React chrome: window[global_name] = JSON.parse(json),
// then dispatch CustomEvent(event_name, { detail }). No-op off-Emscripten.
inline void dispatch_state(const char* event_name, const char* global_name,
                           const std::string& json) {
#ifdef __EMSCRIPTEN__
    EM_ASM({
        try {
            var obj = JSON.parse(UTF8ToString($1));
            window[UTF8ToString($2)] = obj;
            window.dispatchEvent(new CustomEvent(UTF8ToString($0), { detail: obj }));
        } catch (e) {
            console.warn('transport dispatch_state: bad payload', e);
        }
    }, event_name, json.c_str(), global_name);
#else
    (void)event_name;
    (void)global_name;
    (void)json;
#endif
}

}  // namespace edu::transport
