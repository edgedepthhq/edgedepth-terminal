// ═══════════════════════════════════════════════════════════════════════════════
// entitlements_test.cpp - native pin on the client-side replay gating.
//
// Entitlements decides what the terminal LETS you click: how far back the
// scrubber reaches, which symbols replay, how fast, and whether a right-click
// offers "Replay from here" or an upgrade nudge. The Go backend enforces the
// real rule on every replay-session request, so nothing here is a security
// boundary. What it is instead is an agreement: the client must gate exactly
// what the server gates, because the two failure modes are both bad.
//
//   Client tighter than server -> the UI refuses clicks the backend would have
//   served. That already happened once: a hard-coded 30d reach made the
//   cryptohftdata backfill (~71 days back) unreachable even for admin, who had
//   a 90d claim minted into their token.
//
//   Client looser than server -> the user gets a doomed POST and an error modal
//   instead of a locked control that explains itself.
//
// Every window function takes now_ms as a PARAMETER rather than reading a clock,
// which is what makes this file deterministic. The tests below drive it from a
// fixed epoch instead of "now".
// ═══════════════════════════════════════════════════════════════════════════════

#include "core/entitlements.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void expect_true(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

void expect_i64(int64_t got, int64_t want, const char* what) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: got %lld, want %lld\n", what,
                     static_cast<long long>(got), static_cast<long long>(want));
        ++failures;
    }
}

void expect_eq(const std::string& got, const std::string& want, const char* what) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s\n  got:  '%s'\n  want: '%s'\n", what, got.c_str(),
                     want.c_str());
        ++failures;
    }
}

constexpr int64_t kDay = 86400000LL;
constexpr int64_t kHour = 3600000LL;

// 2026-08-23T13:45:30.123Z. Deliberately mid-day and not on any round boundary,
// so a window that accidentally tracks "now" instead of the calendar day shows.
constexpr int64_t kNow = 1787492730123LL;

// Every test owns its own tier + window state; nothing is inherited from the
// process defaults or from whichever test ran before.
void reset_state() {
    Entitlements::current() = Entitlements::Tier::Pro;
    Entitlements::pro_lookback_ms() = Entitlements::PRO_REPLAY_LOOKBACK_MS;
    Entitlements::free_window_dynamic() = false;
    Entitlements::free_window_start_ms() = 0;
    Entitlements::free_window_end_ms() = 0;
    Entitlements::user_email().clear();
    Entitlements::hosted() = true;
}

void test_tier_is_a_superset_chain() {
    using T = Entitlements::Tier;
    reset_state();

    // Research is a superset of Pro; Admin is a superset of Research. Any tier
    // that answers yes to is_research must also answer yes to is_pro, or a
    // research subscriber loses Pro affordances they have paid for.
    const T tiers[] = {T::Free, T::Trader, T::Pro, T::Research, T::Admin};
    for (const T t : tiers) {
        Entitlements::current() = t;
        if (Entitlements::is_admin()) {
            expect_true(Entitlements::is_research(), "admin implies research");
        }
        if (Entitlements::is_research()) {
            expect_true(Entitlements::is_pro(), "research implies pro");
        }
        expect_true(Entitlements::is_free() == (t == T::Free),
                    "is_free is true for exactly one tier");
    }

    Entitlements::current() = T::Free;
    expect_true(!Entitlements::is_pro(), "free is not pro");
    Entitlements::current() = T::Trader;
    expect_true(!Entitlements::is_pro(), "trader is gated like free for replay");
    Entitlements::current() = T::Pro;
    expect_true(Entitlements::is_pro() && !Entitlements::is_research(), "pro stops at pro");
    Entitlements::current() = T::Research;
    expect_true(Entitlements::is_research() && !Entitlements::is_admin(),
                "research stops below admin");
    Entitlements::current() = T::Admin;
    expect_true(Entitlements::is_admin(), "admin is admin");

    // Labels: one per tier, none of them shared.
    Entitlements::current() = T::Free;     expect_eq(Entitlements::tier_label(), "FREE", "free label");
    Entitlements::current() = T::Trader;   expect_eq(Entitlements::tier_label(), "TRADER", "trader label");
    Entitlements::current() = T::Pro;      expect_eq(Entitlements::tier_label(), "PRO", "pro label");
    Entitlements::current() = T::Research; expect_eq(Entitlements::tier_label(), "RESEARCH", "research label");
    Entitlements::current() = T::Admin;    expect_eq(Entitlements::tier_label(), "ADMIN", "admin label");
}

void test_static_free_window_is_one_whole_utc_calendar_day() {
    reset_state();
    Entitlements::current() = Entitlements::Tier::Free;

    int64_t start = 0, end = 0;
    Entitlements::free_window_range(kNow, start, end);

    // Exactly 24h, both ends on a UTC midnight. The old rolling [72h,48h] band
    // satisfied neither, and the backend's calendar-day check would 4xx it.
    expect_i64(end - start, kDay, "the free window is exactly one day long");
    expect_i64(start % kDay, 0, "the window starts at a UTC midnight");
    expect_i64(end % kDay, 0, "the window ends at a UTC midnight");
    expect_i64(start, (kNow / kDay) * kDay - 2 * kDay, "the window is the day before yesterday");
    expect_true(end <= kNow, "the window is entirely in the past");

    // The window must NOT drift with the time of day. A user scrubbing just
    // after midnight and one scrubbing just before it get the same day, which is
    // the entire point of anchoring to a calendar day instead of a rolling band.
    const int64_t day_start = (kNow / kDay) * kDay;
    for (const int64_t offset : {int64_t{0}, int64_t{1}, kHour, 12 * kHour, kDay - 1}) {
        int64_t s = 0, e = 0;
        Entitlements::free_window_range(day_start + offset, s, e);
        expect_i64(s, start, "the free window does not move within a UTC day");
        expect_i64(e, end, "the free window end does not move within a UTC day");
    }
    // ...and it advances by exactly one day at the boundary, not by some fraction.
    int64_t next_s = 0, next_e = 0;
    Entitlements::free_window_range(day_start + kDay, next_s, next_e);
    expect_i64(next_s - start, kDay, "the window advances one whole day at UTC midnight");
    expect_i64(next_e - end, kDay, "the window end advances one whole day too");
}

void test_dynamic_free_window_overrides_the_client_guess() {
    reset_state();
    Entitlements::current() = Entitlements::Tier::Free;

    // The backend told us which day is actually archived. That answer wins
    // verbatim, with no client-side inset: the server enforces the same bounds,
    // so a client that "helpfully" trimmed them would refuse its own valid range.
    const int64_t served_start = 1787184000000LL;  // 2026-08-18T00:00:00Z
    const int64_t served_end = served_start + kDay;
    Entitlements::free_window_dynamic() = true;
    Entitlements::free_window_start_ms() = served_start;
    Entitlements::free_window_end_ms() = served_end;

    int64_t start = 0, end = 0;
    Entitlements::free_window_range(kNow, start, end);
    expect_i64(start, served_start, "the served window start is used verbatim");
    expect_i64(end, served_end, "the served window end is used verbatim");

    // And it is used regardless of the client clock, which is the whole reason
    // the backend supplies it.
    Entitlements::free_window_range(kNow + 30 * kDay, start, end);
    expect_i64(start, served_start, "a skewed client clock does not move the served window");

    // Dropping back to the static fallback restores the computed day.
    Entitlements::free_window_dynamic() = false;
    Entitlements::free_window_range(kNow, start, end);
    expect_i64(start, (kNow / kDay) * kDay - 2 * kDay, "clearing the override restores the fallback");
}

void test_free_range_gate_matches_the_window_exactly() {
    reset_state();
    Entitlements::current() = Entitlements::Tier::Free;

    int64_t start = 0, end = 0;
    Entitlements::free_window_range(kNow, start, end);

    // The bounds are inclusive on both ends: the modal requests exactly the
    // advertised window, so an exclusive comparison here would reject the one
    // range the UI itself offers.
    expect_true(Entitlements::range_replayable(start, end, kNow),
                "the advertised window is replayable");
    expect_true(Entitlements::range_replayable(start, start, kNow), "a zero-width range at the start");
    expect_true(Entitlements::range_replayable(end, end, kNow), "a zero-width range at the end");
    expect_true(Entitlements::range_replayable(start + kHour, end - kHour, kNow),
                "an interior range is replayable");

    // One millisecond outside on either side is refused. The whole range has to
    // fit; a range that merely OVERLAPS the window is not enough.
    expect_true(!Entitlements::range_replayable(start - 1, end, kNow),
                "a range starting one ms early is refused");
    expect_true(!Entitlements::range_replayable(start, end + 1, kNow),
                "a range ending one ms late is refused");
    expect_true(!Entitlements::range_replayable(start - kDay, start + kHour, kNow),
                "a range straddling the start is refused");
    expect_true(!Entitlements::range_replayable(end - kHour, end + kDay, kNow),
                "a range straddling the end is refused");
    expect_true(!Entitlements::range_replayable(kNow - kHour, kNow, kNow),
                "free cannot replay the live edge");

    // A single timestamp gate agrees with the range gate at every boundary.
    expect_true(Entitlements::time_replayable(start, kNow), "the window start is replayable");
    expect_true(Entitlements::time_replayable(end, kNow), "the window end is replayable");
    expect_true(!Entitlements::time_replayable(start - 1, kNow), "one ms before the start is not");
    expect_true(!Entitlements::time_replayable(end + 1, kNow), "one ms after the end is not");
    expect_i64(Entitlements::replay_start_floor_ms(kNow), start, "the floor is the window start");
    expect_i64(Entitlements::replay_end_ceil_ms(kNow), end, "the ceiling is the window end");
    expect_i64(Entitlements::replay_floor_ms(kNow), Entitlements::replay_start_floor_ms(kNow),
               "the legacy floor alias agrees with the floor");
}

void test_pro_reach_follows_the_backend_injected_lookback() {
    reset_state();
    Entitlements::current() = Entitlements::Tier::Pro;

    // Default reach is 30 days back, up to the live edge.
    expect_i64(Entitlements::replay_start_floor_ms(kNow), kNow - 30 * kDay, "the default pro floor is 30d");
    expect_i64(Entitlements::replay_end_ceil_ms(kNow), kNow, "pro reaches the live edge");
    expect_true(Entitlements::range_replayable(kNow - 20 * kDay, kNow - 19 * kDay, kNow),
                "a range inside the 30d reach is replayable");
    expect_true(!Entitlements::range_replayable(kNow - 40 * kDay, kNow - 39 * kDay, kNow),
                "a range beyond the 30d reach is refused");
    // Nobody replays the future, whatever their tier.
    expect_true(!Entitlements::range_replayable(kNow - kHour, kNow + kHour, kNow),
                "a range ending in the future is refused");
    expect_true(!Entitlements::time_replayable(kNow + 1, kNow), "a future timestamp is not replayable");

    // THE regression this constant exists for: the archive backfill reaches ~71
    // days back and staff tokens are minted at 90d, but a hard-coded 30d refused
    // the click. Raising the injected lookback must open the reach immediately.
    const int64_t deep = kNow - 71 * kDay;
    expect_true(!Entitlements::time_replayable(deep, kNow),
                "a 71-day-old point is out of reach at the 30d default");
    Entitlements::pro_lookback_ms() = 90 * kDay;
    expect_true(Entitlements::time_replayable(deep, kNow),
                "the same point is in reach once the backend raises the lookback");
    expect_i64(Entitlements::replay_start_floor_ms(kNow), kNow - 90 * kDay, "the floor follows the lookback");
    expect_true(Entitlements::pro_lookback_days() == 90, "the day count follows the lookback");
    expect_eq(Entitlements::archive_label(), "90d archive", "the scrubber caption follows the lookback");

    Entitlements::pro_lookback_ms() = Entitlements::PRO_REPLAY_LOOKBACK_MS;
    expect_true(Entitlements::pro_lookback_days() == 30, "the default reach is 30 days");
    expect_eq(Entitlements::archive_label(), "30d archive", "the default caption reads 30d");
    Entitlements::current() = Entitlements::Tier::Free;
    expect_eq(Entitlements::archive_label(), "free window", "free advertises the window, not a depth");

    // Research and admin reach at least as far as pro at the same settings.
    reset_state();
    for (const auto t : {Entitlements::Tier::Pro, Entitlements::Tier::Research,
                         Entitlements::Tier::Admin}) {
        Entitlements::current() = t;
        expect_i64(Entitlements::replay_start_floor_ms(kNow), kNow - 30 * kDay,
                   "every pro-or-better tier shares the pro floor");
        expect_i64(Entitlements::replay_end_ceil_ms(kNow), kNow,
                   "every pro-or-better tier reaches the live edge");
    }
}

void test_symbol_and_speed_gates() {
    reset_state();
    Entitlements::current() = Entitlements::Tier::Free;

    // The six majors, matched case-insensitively so a link written in any case
    // resolves the same way the route parser resolves it.
    for (const char* sym : Entitlements::FREE_REPLAY_SYMBOLS) {
        expect_true(Entitlements::symbol_replay_allowed(sym), "a listed free symbol is allowed");
    }
    expect_true(Entitlements::symbol_replay_allowed("BTCUSDT"), "uppercase matches the free list");
    expect_true(Entitlements::symbol_replay_allowed("BtcUsdt"), "mixed case matches the free list");
    expect_true(!Entitlements::symbol_replay_allowed("linkusdt"), "an unlisted symbol is refused");
    expect_true(!Entitlements::symbol_replay_allowed(""), "an empty symbol is refused");
    expect_true(!Entitlements::symbol_replay_allowed("btc"), "a partial symbol is refused");
    expect_true(!Entitlements::symbol_replay_allowed("btcusdtx"), "a superstring symbol is refused");

    // Speed: the server clamps silently, the client locks the buttons. The
    // epsilon means the 2x button itself is never one ULP away from locked.
    expect_true(Entitlements::speed_allowed(1.0), "1x is allowed on free");
    expect_true(Entitlements::speed_allowed(Entitlements::FREE_MAX_SPEED), "the cap itself is allowed");
    expect_true(!Entitlements::speed_allowed(4.0), "4x is locked on free");
    expect_true(Entitlements::clamp_speed_for_tier(8.0) == Entitlements::FREE_MAX_SPEED,
                "a faster request clamps to the cap");
    expect_true(Entitlements::clamp_speed_for_tier(1.0) == 1.0, "a slower request is untouched");
    expect_true(Entitlements::clamp_speed_for_tier(0.5) == 0.5, "sub-1x is untouched");

    // Pro has no symbol or speed gate at all.
    Entitlements::current() = Entitlements::Tier::Pro;
    expect_true(Entitlements::symbol_replay_allowed("linkusdt"), "pro replays any symbol");
    expect_true(Entitlements::speed_allowed(64.0), "pro replays at any speed");
    expect_true(Entitlements::clamp_speed_for_tier(64.0) == 64.0, "pro speed is never clamped");
}

void test_authentication_is_not_the_same_question_as_tier() {
    reset_state();

    // Only a genuinely anonymous visitor should be told to log in. A logged-in
    // free user hitting an auth error has a token problem, not a login problem,
    // and sending them to the login page is a dead end.
    Entitlements::current() = Entitlements::Tier::Free;
    Entitlements::user_email().clear();
    expect_true(!Entitlements::is_authenticated(), "an anonymous free visitor is not authenticated");

    Entitlements::user_email() = "someone@example.com";
    expect_true(Entitlements::is_authenticated(), "a free user with an email is authenticated");
    expect_true(Entitlements::is_free(), "and is still on the free tier");

    Entitlements::user_email().clear();
    Entitlements::current() = Entitlements::Tier::Pro;
    expect_true(Entitlements::is_authenticated(), "a pro user is authenticated without an email");

    // Sessions-today is a UX mirror of a server rule; off-Emscripten it reports
    // a full allowance rather than a stale or negative one.
    expect_true(Entitlements::sessions_today() == 0, "the native session counter starts at zero");
    expect_true(Entitlements::sessions_remaining_today() == Entitlements::FREE_MAX_SESSIONS_PER_DAY,
                "the native session allowance is full");
    Entitlements::note_session_started();
    expect_true(Entitlements::sessions_remaining_today() >= 0,
                "the remaining count never goes negative");

    // detect() reads browser globals; natively it must leave the state alone
    // rather than resetting the tier a test (or a dev build) has set.
    Entitlements::current() = Entitlements::Tier::Admin;
    Entitlements::detect();
    expect_true(Entitlements::is_admin(), "detect() is inert off-Emscripten");
    reset_state();
}

}  // namespace

int main() {
    test_tier_is_a_superset_chain();
    test_static_free_window_is_one_whole_utc_calendar_day();
    test_dynamic_free_window_overrides_the_client_guess();
    test_free_range_gate_matches_the_window_exactly();
    test_pro_reach_follows_the_backend_injected_lookback();
    test_symbol_and_speed_gates();
    test_authentication_is_not_the_same_question_as_tier();

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("entitlements_test: all assertions passed\n");
    return 0;
}
