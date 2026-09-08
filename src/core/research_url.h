#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// research_url.h - pure helpers for the terminal → /research handoff
// (the EdgeDepth web app's /research surface).
//
// THE TRAP this module exists to pin: the chart context menu's captured
// timestamp (context_menu_time_ms_) floors to the CHART TIMEFRAME so replay
// starts on a candle boundary - on a 4H chart that is a 4-hour boundary.
// Research reads a MINUTE. Everything here floors to 60000 itself and never
// touches the replay capture. Header-only and Emscripten-free on purpose so
// the native unit test (tests/native/research_url_test.cpp) compiles it with
// a plain host g++.
//
// URL contract (verified against the live web surface 2026-09-06, when the
// query workspace moved from /research to /research/workbench; the hub still
// 308s the old shape, so an older build keeps working):
//   /research/workbench?source=record&study=investigate&entry=terminal&moment={symbol},{iso}
// where symbol matches ^[a-z0-9]{2,32}$ ("btcusdt", never "BTC/USDT") and
// iso is RFC3339 UTC at seconds precision. `moment={symbol},now` is the live
// variant. Optional &mfields=feature.a,feature.b pre-checks reading rows; the
// URL never carries thresholds, so it cannot bypass the frozen validator or
// the confirm gate on the web side.
//
// TWO handoffs live in this file: the pointed minute above ("Find moments like
// this") and the dragged MOVE at the bottom ("Investigate this move"), which
// carries the outcome ladder and horizon list shared with the web door.
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace research_url {

inline constexpr int64_t kMinuteMs = 60'000;

// "BTC/USDT" → "btcusdt". Lowercase, '/' stripped. Anything else passes
// through untouched - the web validator is the authority on what a symbol
// is; this only undoes the terminal's display formatting.
inline std::string normalize_symbol(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (c == '/') continue;
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

// Floor an epoch-ms instant to its MINUTE (never the chart timeframe).
// Non-positive input floors to 0, which callers treat as "no minute".
inline int64_t floor_minute_ms(int64_t t_ms) {
    if (t_ms <= 0) return 0;
    return (t_ms / kMinuteMs) * kMinuteMs;
}

// "2026-08-01T13:57:00Z" - RFC3339 UTC, seconds precision.
inline std::string iso_utc(int64_t t_ms) {
    const time_t secs = static_cast<time_t>(t_ms / 1000);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &secs);
#else
    gmtime_r(&secs, &tm_utc);
#endif
    char buf[80];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm_utc.tm_year + 1900,
             tm_utc.tm_mon + 1, tm_utc.tm_mday, tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    return std::string(buf);
}

// "13:57" - the UTC wall minute, for the context-menu shortcut column.
inline std::string minute_label_utc(int64_t minute_ms) {
    const std::string iso = iso_utc(floor_minute_ms(minute_ms));
    return iso.substr(11, 5);
}

// "2026-08-01 13:57 UTC" - the panel header's minute line.
inline std::string minute_header_utc(int64_t minute_ms) {
    std::string iso = iso_utc(floor_minute_ms(minute_ms));
    iso[10] = ' ';
    return iso.substr(0, 16) + " UTC";
}

// feature ids joined for &mfields=. Ids come from the registry's closed
// grammar ([a-z0-9_.]), so no percent-encoding is needed.
inline std::string join_mfields(const std::vector<std::string>& ids) {
    std::string out;
    for (const auto& id : ids) {
        if (!out.empty()) out += ',';
        out += id;
    }
    return out;
}

// The Phase 1 handoff URL for a pointed minute. t_ms is floored to ITS
// minute here (the 60000 floor, not the chart timeframe).
inline std::string moment_url(const std::string& symbol_raw, int64_t t_ms,
                              const std::string& mfields = std::string(),
                              const char* base = "https://edgedepth.com") {
    std::string url = std::string(base) +
                      "/research/workbench?source=record&study=investigate&entry=terminal&moment=" +
                      normalize_symbol(symbol_raw) + "," + iso_utc(floor_minute_ms(t_ms));
    if (!mfields.empty()) url += "&mfields=" + mfields;
    return url;
}

// The live variant: the newest closed minute, not a pointed one.
inline std::string moment_live_url(const std::string& symbol_raw,
                                   const std::string& mfields = std::string(),
                                   const char* base = "https://edgedepth.com") {
    std::string url = std::string(base) +
                      "/research/workbench?source=record&study=investigate&entry=terminal&moment=" +
                      normalize_symbol(symbol_raw) + ",now";
    if (!mfields.empty()) url += "&mfields=" + mfields;
    return url;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Outcome-first handoff: shift-drag a move, ask what preceded moves like it.
//
// The ladder and the horizon list below are the SHARED contract with the web
// door and the engine route (outcome_first_query.v1). Two shapes that look
// like details and are not:
//   - magnitude is a FRACTION (0.10 for ten percent), never a percentage.
//   - horizon is a closed SUFFIX ("4h"), never ISO 8601 ("PT4H").
// Do not add a rung or a horizon here alone; all three sides move together.
// ═══════════════════════════════════════════════════════════════════════════════

// The outcome ladder, ascending. Down moves stop at 1.00 because a return
// cannot fall past total loss.
inline constexpr double kOutcomeLadder[] = {0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.10,
                                            0.15,  0.20,  0.30,  0.50, 1.00, 2.00, 4.00};
inline constexpr double kOutcomeLadderDownCap = 1.00;

struct OutcomeHorizon {
    const char* suffix;
    int64_t minutes;
};
inline constexpr OutcomeHorizon kOutcomeHorizons[] = {{"30m", 30},   {"1h", 60},    {"4h", 240},
                                                      {"24h", 1440}, {"72h", 4320}, {"7d", 10080}};

// The dragged range reduced to a target the engine already counts. When ok is
// false every other field is meaningless and the caller renders the menu item
// disabled; direction and horizon are always readable strings, never null.
struct MoveSnap {
    bool ok = false;
    const char* direction = "";
    double magnitude = 0.0;
    const char* horizon = "";
    int64_t start_minute_ms = 0;
};

// Snap a dragged range onto the outcome grammar. PURE: no ImGui, no chart, no
// Emscripten, so tests/native/research_url_test.cpp pins it with a host g++.
//
// kind is always "reached" (the extreme inside the range, not where it
// finished). Magnitude snaps DOWN to the rung at or below the excursion and
// the horizon snaps UP to the closed horizon at or above the span: that
// pairing is what guarantees the dragged move is a member of the population it
// opens, the same self-validation the commonality seeds rely on. Snapping the
// other way would open a population the move itself fails to join.
//
// start_minute_ms floors to 60000, NEVER to the chart timeframe (the trap at
// the top of this file: a 4H chart floors a click to a 4-hour boundary).
inline MoveSnap snap_move(int64_t start_ms, int64_t end_ms, double start_close, double end_close,
                          double max_high, double min_low) {
    MoveSnap none{};
    if (start_ms <= 0 || end_ms <= start_ms) return none;
    if (!(start_close > 0.0)) return none;         // zero, negative or NaN anchor
    if (end_ms - start_ms < kMinuteMs) return none;  // a sub-minute drag addresses no minute

    // Direction is where the range FINISHED (last close against first close);
    // the magnitude below is how far it REACHED in that direction.
    const bool up = end_close > start_close;

    const double excursion = up ? (max_high / start_close - 1.0) : (1.0 - min_low / start_close);
    double rung = 0.0;
    for (const double r : kOutcomeLadder) {
        if (!up && r > kOutcomeLadderDownCap) break;
        if (!(r <= excursion)) break;  // ladder ascends; NaN excursion falls out here
        rung = r;
    }
    if (rung <= 0.0) return none;  // under the smallest rung: not a move this door can count

    // The span is measured in MINUTES between the anchor minute and the last
    // minute the drag touched, because that is the grid the engine counts on:
    // the horizon clock starts at the anchor minute, and the extreme sits in
    // some minute at or before floor(end). Measuring raw milliseconds instead
    // would name a horizon that is short of the move whenever the drag starts
    // mid-minute, and cost it membership of its own population.
    const int64_t span_ms = floor_minute_ms(end_ms) - floor_minute_ms(start_ms);
    const char* horizon = nullptr;
    for (const OutcomeHorizon& h : kOutcomeHorizons) {
        if (h.minutes * kMinuteMs >= span_ms) {
            horizon = h.suffix;
            break;
        }
    }
    if (!horizon) return none;  // longer than the closed list's 7d

    return MoveSnap{true, up ? "up" : "down", rung, horizon, floor_minute_ms(start_ms)};
}

// "up 10% in 4h" - the menu shortcut column, so the snap rule is readable
// before the click rather than a surprise on the web side. Empty when !ok.
inline std::string move_label(const MoveSnap& snap) {
    if (!snap.ok) return std::string();
    char buf[48];
    snprintf(buf, sizeof(buf), "%s %g%% in %s", snap.direction, snap.magnitude * 100.0,
             snap.horizon);
    return std::string(buf);
}

// The outcome-first handoff URL, shared byte-for-byte with the web door
// (OF1 section 3b). Nothing here is executable: the page renders a
// confirmation row and waits for the user's run action.
inline std::string outcome_first_url(const std::string& symbol_raw, const MoveSnap& snap,
                                     const char* scope = "sector",
                                     const char* base = "https://edgedepth.com") {
    if (!snap.ok) return std::string();
    char target[64];
    // kind is always "reached"; magnitude goes over as the fraction it is.
    snprintf(target, sizeof(target), "reached,%s,%g,%s", snap.direction, snap.magnitude,
             snap.horizon);
    return std::string(base) + "/research/workbench?source=record&study=outcome-first&entry=terminal" +
           "&target=" + target + "&symbol=" + normalize_symbol(symbol_raw) +
           "&at=" + iso_utc(snap.start_minute_ms) + "&scope=" + scope;
}

}  // namespace research_url
