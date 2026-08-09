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
// URL contract (verified against the live web surface 2026-08-01):
//   /research?source=record&study=investigate&moment={symbol},{iso}
// where symbol matches ^[a-z0-9]{2,32}$ ("btcusdt", never "BTC/USDT") and
// iso is RFC3339 UTC at seconds precision. `moment={symbol},now` is the live
// variant. Optional &mfields=feature.a,feature.b pre-checks reading rows; the
// URL never carries thresholds, so it cannot bypass the frozen validator or
// the confirm gate on the web side.
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
    std::string url = std::string(base) + "/research?source=record&study=investigate&moment=" +
                      normalize_symbol(symbol_raw) + "," + iso_utc(floor_minute_ms(t_ms));
    if (!mfields.empty()) url += "&mfields=" + mfields;
    return url;
}

// The live variant: the newest closed minute, not a pointed one.
inline std::string moment_live_url(const std::string& symbol_raw,
                                   const std::string& mfields = std::string(),
                                   const char* base = "https://edgedepth.com") {
    std::string url = std::string(base) + "/research?source=record&study=investigate&moment=" +
                      normalize_symbol(symbol_raw) + ",now";
    if (!mfields.empty()) url += "&mfields=" + mfields;
    return url;
}

}  // namespace research_url
