#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// entitlements.h - client-side plan/tier gating for replay (UX layer only).
//
// The web host (Next/Payload) knows the signed-in user's tier; it hands it to the
// WASM client via a window global set BEFORE the glue loads (mirrors how lessons
// are passed in via __EDGEDEPTH_LESSON__):
//
//     window.__EDGEDEPTH_TIER__ = "free" | "trader" | "pro" | "research" | "admin";
//
// We read it once at boot. This drives only the UX (locked menu items, the
// scrubber's advertised archive depth, the upgrade nudge). It is NOT a security
// boundary - the Go backend still enforces the real cap on every replay-session
// request (a tampered global must not unlock data). Standalone / dev (no global)
// defaults to Pro so local work is never gated.
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstdio>
#include <string>
#include <array>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <chrono>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace Entitlements {

// Research is a SUPERSET of Pro (every Pro entitlement, plus a deeper replay
// reach). Admin is a superset of Research for terminal display gates. Actual
// replay reach still comes from the
// backend-injected lookback below. NOTE: the web deliberately tokenizes research
// subscribers as `pro` (the backend TierConfig knows free/pro/admin only) and
// carries the extra reach as a signed maxLookbackDays claim; do not "fix" that by
// inventing a research TokenTier.
enum class Tier : uint8_t { Free, Trader, Pro, Research, Admin };

// ── Free-tier replay limits (tunable in ONE place - MIRROR the backend's
//    replay-session TierConfigs["free"]) ───────────────────────────────────
// D6 (amended 2026-07-11): the free replay window is the most recent fully-
// archived UTC CALENDAR DAY (e.g. [9 Jul 00:00, 10 Jul 00:00) UTC), which only
// advances when a new day's parquet archive lands on the box. The backend is the
// single source of truth: /replay/availability injects the exact bounds via
// window.__EDGEDEPTH_FREE_WINDOW__ (read in detect()), and free_window_range()
// prefers them. When the fetch fails, the client computes the SAME day itself
// (today-2, mirroring freeFloorDays/coldFloorDays) - see free_window_range.
//
// This REPLACES the older rolling [72h,48h]-ago band. The AGE constants below are
// retained only as documentation of that band + the 2-day-back intuition; they
// are NO LONGER the enforced window (a fixed offset can't equal a calendar day).
inline constexpr int64_t FREE_WINDOW_START_AGE_MS = 72LL * 3600LL * 1000LL;         // legacy band: earliest start
inline constexpr int64_t FREE_WINDOW_END_AGE_MS   = 48LL * 3600LL * 1000LL;         // legacy band: latest end
inline constexpr int64_t FREE_WINDOW_MAX_SPAN_MS  = 24LL * 3600LL * 1000LL;         // one day
inline constexpr int64_t PRO_REPLAY_LOOKBACK_MS   = 30LL * 24LL * 3600LL * 1000LL;  // 30d (DEFAULT, not a ceiling)

// Pro lookback is a DEFAULT the backend can raise. Accounts whose replay token
// carries a signed maxLookbackDays claim get more (staff + research subscribers
// are minted at 90d; internal/replay_session/handler.go bounds any claim by
// maxReplayLookback = 90d). Hard-coding 30d here made the client refuse clicks the
// backend would have served: the cryptohftdata backfill reaches 2026-05-15, ~71
// days back, and was unreachable from the UI ("Replay locked - beyond the 30-day
// archive") even for admin.
//
// The host injects window.__EDGEDEPTH_REPLAY_LOOKBACK_DAYS__ from the SAME number
// it mints into the token, so the client affordance and server enforcement cannot
// disagree - exactly how the free window is already resolved. Absent or invalid
// leaves 30d, so an ordinary Pro user sees their real reach and never fires a
// doomed POST.
inline int64_t& pro_lookback_ms()   { static int64_t v = PRO_REPLAY_LOOKBACK_MS; return v; }
inline int      pro_lookback_days() { return static_cast<int>(pro_lookback_ms() / (24LL * 3600LL * 1000LL)); }

// Free replay speed cap. The server CLAMPS silently to this (never errors, D7); the
// client locks the >2× buttons and funnels a tap into the upsell modal.
inline constexpr double  FREE_MAX_SPEED = 2.0;

// Free replay sessions per UTC day (server enforces via TIER_DAILY → 429; the client
// mirrors it as a "N of 6 today" chip). Design P2.
inline constexpr int     FREE_MAX_SESSIONS_PER_DAY = 6;

// Symbols a non-Pro user may REPLAY (live data stays all-symbols for everyone).
// The 6 most liquid majors - MIRROR handler.go FreeReplaySymbols (design P1: +DOGE,
// whose constant liquidation action makes the free Liq Field demo dramatic).
inline constexpr std::array<const char*, 6> FREE_REPLAY_SYMBOLS = {
    "btcusdt", "ethusdt", "solusdt", "xrpusdt", "bnbusdt", "dogeusdt"
};

// Current tier - read once at boot via detect(). Pro by default (dev/standalone).
inline Tier& current() { static Tier t = Tier::Pro; return t; }
inline bool  is_pro()  {
    return current() == Tier::Pro || current() == Tier::Research || current() == Tier::Admin;
}
inline bool  is_research() {
    return current() == Tier::Research || current() == Tier::Admin;
}
inline bool  is_admin() { return current() == Tier::Admin; }

// Whether this build is running behind the hosted product, i.e. whether there
// is an EdgeDepth account and a courses hub on the other side of the chrome.
//
// Defaults TRUE and only ever flips on an EXPLICIT window.__EDGEDEPTH_HOSTED__
// === false. app.edgedepth.com never sets that global, so the hosted deploy is
// byte-identical to before this existed; the bundled docker image sets it in
// docker/entrypoint.sh. Read via detect() at boot.
//
// This exists because the tier default above is Pro, which is right for dev but
// makes a self-hosted stack render a signed-in "PRO . FOUNDER RATE" account the
// user does not have, contradicting the Unlock CTA in the same window. Tier
// answers "what may this user do"; this answers "is there an account at all".
inline bool& hosted() { static bool b = true; return b; }

// Signed-in user's email (account menu header). Set by the host via
// window.__EDGEDEPTH_USER_EMAIL__; empty in standalone/dev.
inline std::string& user_email() { static std::string e; return e; }

// Subscription renewal date label for the Pro account menu (e.g. "12 Jul 2026").
// The host sets window.__EDGEDEPTH_RENEWS__ to a short pre-formatted string; empty
// in standalone/dev (the menu then just shows "Active", no date). Display-only.
inline std::string& renews_label() { static std::string s; return s; }

// ── Dynamic free-replay window (backend-authoritative) ───────────────────────
// The host injects window.__EDGEDEPTH_FREE_WINDOW__ (from GET /replay/availability,
// which the Go backend also ENFORCES) BEFORE the glue loads. When present it is the
// most recent fully-archived UTC calendar day as absolute epoch-ms bounds - the REAL
// replayable day, not the client's static [72h,48h] guess. detect() caches it here;
// the window helpers below PREFER these values and fall back to the static band when
// the fetch failed (or for Pro). free_window_dynamic() gates the override.
inline bool&        free_window_dynamic()  { static bool b = false;    return b; }
inline int64_t&     free_window_start_ms() { static int64_t v = 0;     return v; }
inline int64_t&     free_window_end_ms()   { static int64_t v = 0;     return v; }
inline std::string& free_window_day_utc()  { static std::string s;     return s; }

// Forward decl: the window helpers below (replay_start_floor_ms, range_replayable)
// resolve the free window through this single function. Defined lower, once the
// constants are in scope. Keeping ONE resolver means client display and backend
// enforcement can't disagree about the day.
inline void free_window_range(int64_t now_ms, int64_t& start_ms, int64_t& end_ms);

inline const char* tier_label() {
    switch (current()) { case Tier::Admin: return "ADMIN";
                         case Tier::Research: return "RESEARCH";
                         case Tier::Pro: return "PRO";
                         case Tier::Trader: return "TRADER";
                         default: return "FREE"; }
}

// Read window.__EDGEDEPTH_TIER__ once. Safe to call when not embedded (leaves Pro).
inline void detect() {
#ifdef __EMSCRIPTEN__
    char buf[16] = {0};
    const int n = EM_ASM_INT({
        try {
            var t = (window.__EDGEDEPTH_TIER__ || "").toString().toLowerCase();
            if (!t) return 0;
            var s = (t === "free") ? "free" : (t === "trader") ? "trader"
                  : (t === "research") ? "research" : (t === "admin") ? "admin" : "pro";
            stringToUTF8(s, $0, 16);
            return s.length;
        } catch (e) { return 0; }
    }, buf);
    if (n > 0) {
        const std::string s(buf);
        current() = (s == "free")     ? Tier::Free
                  : (s == "trader")   ? Tier::Trader
                  : (s == "research") ? Tier::Research
                  : (s == "admin")    ? Tier::Admin
                                      : Tier::Pro;
    }
    // Strict === false, so an absent or malformed global leaves hosted() true
    // and nothing about app.edgedepth.com changes.
    hosted() = EM_ASM_INT({
        try { return window.__EDGEDEPTH_HOSTED__ === false ? 0 : 1; }
        catch (e) { return 1; }
    }) != 0;

    char ebuf[128] = {0};
    EM_ASM({
        try {
            var e = (window.__EDGEDEPTH_USER_EMAIL__ || "").toString();
            stringToUTF8(e.slice(0, 120), $0, 128);
        } catch (err) {}
    }, ebuf);
    user_email() = ebuf;

    // Optional pre-formatted renewal date (Pro account menu). Empty if unset.
    char rbuf[64] = {0};
    EM_ASM({
        try {
            var r = (window.__EDGEDEPTH_RENEWS__ || "").toString();
            stringToUTF8(r.slice(0, 56), $0, 64);
        } catch (err) {}
    }, rbuf);
    renews_label() = rbuf;

    // Backend-authoritative replay reach (window.__EDGEDEPTH_REPLAY_LOOKBACK_DAYS__),
    // set by the host from the SAME number it mints into the replay token. Clamped
    // to [1, 90] to mirror handler.go maxReplayLookback; anything absent or absurd
    // leaves the 30d default rather than advertising reach the backend will refuse.
    {
        const int d = EM_ASM_INT({
            try {
                var v = parseInt(window.__EDGEDEPTH_REPLAY_LOOKBACK_DAYS__, 10);
                return (isFinite(v) && v > 0) ? v : 0;
            } catch (e) { return 0; }
        });
        if (d > 0) {
            const int clamped = d > 90 ? 90 : d;
            pro_lookback_ms() = static_cast<int64_t>(clamped) * 24LL * 3600LL * 1000LL;
        }
    }

    // Dynamic free-replay window (window.__EDGEDEPTH_FREE_WINDOW__). Absolute
    // epoch-ms bounds cross as doubles via HEAPF64 (exact for ms timestamps up to
    // 2^53) to dodge the fragile i64 EM_ASM marshal; the day string comes back in
    // dbuf. ok=1 only when the global is present AND start<end.
    {
        double sd = 0.0, ed = 0.0;
        char dbuf[24] = {0};
        const int ok = EM_ASM_INT({
            try {
                var w = window.__EDGEDEPTH_FREE_WINDOW__;
                if (!w) return 0;
                var s = Number(w.startMs);
                var e = Number(w.endMs);
                if (!(s < e)) return 0;
                HEAPF64[$0 >> 3] = s;
                HEAPF64[$1 >> 3] = e;
                var d = (w.dayUtc || "").toString();
                stringToUTF8(d.slice(0, 20), $2, 24);
                return 1;
            } catch (err) { return 0; }
        }, &sd, &ed, dbuf);
        if (ok) {
            free_window_dynamic()  = true;
            free_window_start_ms() = static_cast<int64_t>(sd);
            free_window_end_ms()   = static_cast<int64_t>(ed);
            free_window_day_utc()  = dbuf;
        } else {
            free_window_dynamic() = false;
        }
    }
#endif
}

inline bool is_free() { return current() == Tier::Free; }

// True when the viewer is a signed-in account: Pro/admin, OR a logged-in free user
// whose email the host provided. Drives the AUTH_REQUIRED UX - only a genuinely
// anonymous visitor should be told to "log in" (§8.4); a logged-in user hitting an
// auth error has a token/session problem, not a login problem.
inline bool is_authenticated() { return is_pro() || !user_email().empty(); }

// ── Replay window for the current tier ───────────────────────────────────────
// Earliest replayable START. Pro = now−30d. Free = the resolved free-window start
// (backend archived-day when known, else the client's own today-2 calendar day).
inline int64_t replay_start_floor_ms(int64_t now_ms) {
    if (is_pro()) return now_ms - pro_lookback_ms();
    int64_t s, e; free_window_range(now_ms, s, e); return s;
}
// Latest replayable END. Pro = the live edge. Free = the resolved free-window end.
inline int64_t replay_end_ceil_ms(int64_t now_ms) {
    if (is_pro()) return now_ms;
    int64_t s, e; free_window_range(now_ms, s, e); return e;
}
// Back-compat alias for old call sites: the tier's earliest start timestamp.
inline int64_t replay_floor_ms(int64_t now_ms) { return replay_start_floor_ms(now_ms); }

// Is a single timestamp replayable for the current tier? (used by the chart
// right-click "Replay from here" gate - a point must sit inside the window).
inline bool time_replayable(int64_t ts_ms, int64_t now_ms) {
    return ts_ms >= replay_start_floor_ms(now_ms) && ts_ms <= replay_end_ceil_ms(now_ms);
}
// Is a whole [start,end] range replayable for the current tier? (mirrors the
// backend validateNATSRequest window check so the client never fires a doomed POST).
inline bool range_replayable(int64_t start_ms, int64_t end_ms, int64_t now_ms) {
    if (is_pro()) return start_ms >= replay_start_floor_ms(now_ms) && end_ms <= now_ms;
    // Free: the whole [start,end] must sit inside the resolved free window (the
    // backend's archived day when known, else the client's today-2 calendar day).
    // Mirrors the backend's [dayStart,dayEnd] ±tol check on the exact bounds the
    // modal requests, so no separate client-clock span check is needed.
    int64_t s, e; free_window_range(now_ms, s, e);
    return start_ms >= s && end_ms <= e;
}

// ── Speed ────────────────────────────────────────────────────────────────────
inline bool   speed_allowed(double s)        { return is_pro() || s <= FREE_MAX_SPEED + 1e-9; }
inline double clamp_speed_for_tier(double s)  { return is_pro() ? s : (s > FREE_MAX_SPEED ? FREE_MAX_SPEED : s); }

// Short human label for the advertised archive depth (scrubber caption).
inline const char* archive_label() {
    if (!is_pro()) return "free window";
    static char buf[24];
    snprintf(buf, sizeof(buf), "%dd archive", pro_lookback_days());
    return buf;
}

// Free replay symbols, uppercased + de-suffixed + space-joined for the picker
// header ("BTC ETH SOL XRP BNB DOGE"). Cached on first use.
inline const char* free_symbols_label() {
    static std::string s;
    if (s.empty()) {
        for (const char* a : FREE_REPLAY_SYMBOLS) {
            std::string base(a);
            if (base.size() > 4 && base.compare(base.size() - 4, 4, "usdt") == 0)
                base.resize(base.size() - 4);
            std::transform(base.begin(), base.end(), base.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (!s.empty()) s += " ";
            s += base;
        }
    }
    return s.c_str();
}

// Wall-clock now (ms).
inline int64_t wall_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// The exact [start,end] the free-window replay requests. Dynamic: the backend's
// absolute archived-day bounds (server-authoritative, no client-clock inset needed
// - the backend enforces the same day ±5min). Static fallback: inset a few minutes
// inside the [72h,48h] band so a boundary request survives client/server clock drift.
inline void free_window_range(int64_t now_ms, int64_t& start_ms, int64_t& end_ms) {
    if (free_window_dynamic()) {
        start_ms = free_window_start_ms();
        end_ms   = free_window_end_ms();
        return;
    }
    // Static fallback (availability fetch failed): compute the SAME UTC calendar
    // day the backend defaults to - today-2 (mirrors freeFloorDays/coldFloorDays).
    // Epoch ms is UTC, so flooring to the day needs no tz math. This keeps the
    // fallback request inside the backend's enforced calendar-day window; the OLD
    // rolling [72h,48h] band did NOT (its start reached ~a day before the archived
    // day, so the new calendar-day enforcement would 4xx it).
    const int64_t DAY = 86400000LL;
    const int64_t day_floor = (now_ms / DAY) * DAY;   // 00:00 UTC today
    start_ms = day_floor - 2 * DAY;                   // 00:00 UTC, two days ago
    end_ms   = start_ms + DAY;                         // +24h → 00:00 UTC, one day ago
}

// Compact UTC day label for the account menu's "Replay window" row, from the
// dynamic window (e.g. "9 JUL \xC2\xB7 24H"). Falls back to a static hint when the
// backend window is unavailable. ASCII + the middot byte only (no atlas glyphs).
inline const char* free_window_day_label() {
    static std::string s;
    static const char* MON_UC[] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                    "JUL","AUG","SEP","OCT","NOV","DEC"};
    if (free_window_dynamic()) {
        time_t t = static_cast<time_t>(free_window_start_ms() / 1000);
        struct tm tmv;
#ifdef _WIN32
        gmtime_s(&tmv, &t);
#else
        gmtime_r(&t, &tmv);
#endif
        char buf[32];
        snprintf(buf, sizeof(buf), "%d %s \xC2\xB7 24H", tmv.tm_mday, MON_UC[tmv.tm_mon]);
        s = buf;
        return s.c_str();
    }
    return "2 DAYS AGO \xC2\xB7 24H";
}

// Human copy of the free window in UTC, rounded to the hour, e.g.
// "2 Jul 11:00 → 3 Jul 11:00 UTC". Concrete "from … up until …" for the picker +
// upsell copy (§8.2). Reads cleanest once the window anchors to a calendar day.
inline std::string free_window_label() {
    int64_t s, e;
    free_window_range(wall_now_ms(), s, e);
    static const char* MON[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};
    auto fmt = [&](int64_t ms, char* buf, size_t n) {
        time_t t = static_cast<time_t>((ms + 1800000LL) / 3600000LL * 3600LL);  // round → hour
        struct tm tmv;
#ifdef _WIN32
        gmtime_s(&tmv, &t);
#else
        gmtime_r(&t, &tmv);
#endif
        snprintf(buf, n, "%d %s %02d:00", tmv.tm_mday, MON[tmv.tm_mon], tmv.tm_hour);
    };
    char sb[24], eb[24];
    fmt(s, sb, sizeof(sb));
    fmt(e, eb, sizeof(eb));
    // ASCII "to" (not a → glyph - the mono atlas has no U+2192, it renders as "?").
    return std::string(sb) + " to " + eb + " UTC";
}

// Short UTC day label for the resolved free window ("9 Jul") for the chart
// right-click "Replay <day> instead" rescue offer. Reflects the backend archived
// day when known (window.__EDGEDEPTH_FREE_WINDOW__), else the static fallback day.
// Rebuilt each call (cheap; only hit while the locked context menu is open).
inline std::string free_window_short_label() {
    int64_t s = 0, e = 0;
    free_window_range(wall_now_ms(), s, e);
    static const char* MON[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};
    time_t t = static_cast<time_t>(s / 1000);
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[16];
    snprintf(buf, sizeof(buf), "%d %s", tmv.tm_mday, MON[tmv.tm_mon]);
    return std::string(buf);
}

// May the current tier replay this symbol? (case-insensitive)
inline bool symbol_replay_allowed(const std::string& symbol) {
    if (is_pro()) return true;
    std::string s = symbol;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* a : FREE_REPLAY_SYMBOLS) if (s == a) return true;
    return false;
}

// Send the user to the upgrade page (web host). No-op off-Emscripten.
inline void open_upgrade() {
#ifdef __EMSCRIPTEN__
    EM_ASM({ window.location.href = '/pricing'; });
#endif
}

// Send the user straight into checkout for a payment rail ("card" | "btc"). The
// web /pricing gateway turns ?plan&rail into the Stripe/BTCPay redirect (see
// CheckoutStart.tsx). No-op off-Emscripten.
inline void open_upgrade_rail(const char* rail) {
#ifdef __EMSCRIPTEN__
    EM_ASM({
        window.location.href = '/pricing?plan=yearly&rail=' + UTF8ToString($0);
    }, rail);
#else
    (void)rail;
#endif
}

// Send the user to the login page (web host), preserving where they were so the
// login form can bounce them back. Drives the AUTH_REQUIRED/GRANT_REQUIRED prompt.
inline void open_login() {
#ifdef __EMSCRIPTEN__
    EM_ASM({
        try {
            var here = window.location.pathname + window.location.search;
            window.location.href = '/login?redirectTo=' + encodeURIComponent(here);
        } catch (e) { window.location.href = '/login'; }
    });
#endif
}

// ── Sessions-today mirror ("N of 6 today" chip) ──────────────────────────────
// The SERVER is the real enforcer (TIER_DAILY); this is a UX counter in
// localStorage keyed by the UTC date so it resets at 00:00 UTC. NOT a security
// boundary - a tampered client can reset it and the server still 429s.
inline int sessions_today() {
#ifdef __EMSCRIPTEN__
    return EM_ASM_INT({
        try {
            var d = new Date().toISOString().slice(0, 10);
            var raw = localStorage.getItem('edx_replay_day');
            var o = raw ? JSON.parse(raw) : null;
            if (!o || o.d !== d) return 0;
            return o.n | 0;
        } catch (e) { return 0; }
    });
#else
    return 0;
#endif
}
inline void note_session_started() {
#ifdef __EMSCRIPTEN__
    EM_ASM({
        try {
            var d = new Date().toISOString().slice(0, 10);
            var raw = localStorage.getItem('edx_replay_day');
            var o = raw ? JSON.parse(raw) : null;
            var n = (o && o.d === d) ? (o.n | 0) : 0;
            localStorage.setItem('edx_replay_day', JSON.stringify({ d: d, n: n + 1 }));
        } catch (e) {}
    });
#endif
}
inline int sessions_remaining_today() {
    int rem = FREE_MAX_SESSIONS_PER_DAY - sessions_today();
    return rem < 0 ? 0 : rem;
}

}  // namespace Entitlements
