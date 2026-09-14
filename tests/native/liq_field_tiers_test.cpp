#include "core/liq_field_tiers.h"
#include <cstdio>
#include <cstdlib>
static void check(bool ok) { if (!ok) { std::fputs("liq field tier check failed\n", stderr); std::exit(1); } }
int main() {
    using namespace liq_field;
    check(max_leverage("binancef", "koruusdt") == 25);
    check(max_leverage("binancef", "synusdt") == 10);
    check(max_leverage("hl", "btcusdt") == 0);
    check(max_leverage("binancef", "unknownusdt") == 0);
    auto t = select_tiers(0x3f, 25); check(t.enabled == 0x07 && t.floor == 0x07);
    t = select_tiers(0x3f, 100); check(t.enabled == 0x3f && t.floor == 0x38);
    t = select_tiers(0x3c, 25); check(t.enabled == 0x04 && t.floor == 0x04);
    t = select_tiers(0x3f, 3); check(t.enabled == 0 && t.floor == 0);
    t = select_tiers(0, 100); check(t.enabled == 0 && t.floor == 0);
    for (const auto& cap : kLeverageCaps) {
        check(max_leverage("binancef", cap.symbol) == cap.leverage);
        for (uint8_t mask = 0; mask < 64; ++mask) {
            t = select_tiers(mask, cap.leverage);
            check((t.enabled & ~mask) == 0 && (t.floor & ~t.enabled) == 0);
            for (size_t i = 0; i < kLeverages.size(); ++i)
                if (t.enabled & (1u << i)) check(kLeverages[i] <= cap.leverage);
        }
    }
}
