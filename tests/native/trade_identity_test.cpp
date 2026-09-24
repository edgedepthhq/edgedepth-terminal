// trade_identity_test.cpp - bounded native-ID dedup for at-least-once feeds.
#include "core/trade_identity.h"
#include "core/stat_freshness.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {
int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); ++failures; }
}
}  // namespace

int main() {
    RecentTradeIdentities ids(4);
    expect(ids.admit("a"), "first identity admitted");
    expect(!ids.admit("a"), "repeated identity refused");
    expect(ids.admit(""), "trades without a native id always pass");
    expect(ids.admit("b") && ids.admit("c") && ids.admit("d"), "distinct identities admitted");
    expect(ids.size() == 4, "bounded to capacity");
    expect(ids.admit("e"), "new identity beyond capacity admitted");
    expect(ids.size() == 4 && ids.rejected() == 1, "oldest identity evicted, one rejection counted");
    expect(ids.admit("a"), "evicted identity is admissible again (bounded memory, not a promise)");
    expect(!ids.admit("e"), "recent identity still refused");

    struct Clocks { int64_t mark_price_ms = 0, funding_ms = 0, open_interest_ms = 0; int32_t funding_interval_minutes = 0; };
    Clocks legacy{};
    expect(!StatFreshness::stated(legacy), "legacy stat states no clocks");
    expect(StatFreshness::field(false, 0, 1000, 100).state == StatFreshness::State::Legacy, "legacy field");
    Clocks native{};
    native.mark_price_ms = 1000;
    native.funding_interval_minutes = 240;
    expect(StatFreshness::stated(native), "stated clocks");
    expect(StatFreshness::field(true, 0, 5000, 100).state == StatFreshness::State::Unavailable, "zero clock is unavailable");
    expect(StatFreshness::field(true, 1000, 1050, 100).state == StatFreshness::State::Fresh, "recent clock is fresh");
    const auto stale = StatFreshness::field(true, 1000, 5000, 100);
    expect(stale.state == StatFreshness::State::Stale && stale.age_ms == 4000, "old clock is stale with its age");
    char buf[8];
    expect(std::string(StatFreshness::cadence_label(true, 240, buf, sizeof(buf))) == "4H", "four hour cadence");
    expect(std::string(StatFreshness::cadence_label(true, 480, buf, sizeof(buf))) == "8H", "eight hour cadence");
    expect(std::string(StatFreshness::cadence_label(true, 60, buf, sizeof(buf))) == "1H", "hourly cadence");
    expect(std::string(StatFreshness::cadence_label(true, 0, buf, sizeof(buf))) == "NONE", "no funding for dated contracts");
    expect(std::string(StatFreshness::cadence_label(false, 480, buf, sizeof(buf))).empty(), "legacy cadence is not invented");
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("trade_identity_test: ok");
    return 0;
}
