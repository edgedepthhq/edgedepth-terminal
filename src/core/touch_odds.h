#pragma once
// Heatmap V2 touch odds handed over by the web host as window.__EDGEDEPTH_REACH__ v1
// (TouchOddsBridge). The host decides currency with its access and freshness rules;
// this only checks the shape and answers what may be drawn at a clock. Emscripten-free.
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace touch_odds {

struct Band { int side = 0; int offset = 0; double low = 0, high = 0, p = 0; };
enum class Tier : uint8_t { None, Other, Top5, Top1 };

// Long-range touch odds: a host-supplied logistic model per
// horizon over the reflection logit, log distance, two volatility ratios and side, then a monotone
// calibration map. Evaluated here for any level; the calculation is tested natively with synthetic fixtures.
struct Horizon {
    std::string name;
    int bars = 0;
    double coef[6]{}, lo[5]{}, hi[5]{}, mean[5]{}, sd[5]{};
    std::vector<double> kp, kq;
};
struct LongRange {
    int64_t asof = 0, hide_at = 0, available_at = 0;
    double mark = 0, rv[3]{}, dmin = 0, dmax = 0;
    std::array<Horizon, 3> horizons{};
    bool valid = false;
    bool at(int64_t clock) const { return valid && clock >= available_at && clock < hide_at; }
};

// Probability that price touches `level` within the horizon, from reference price `from`; negative
// outside the model's distance range.
inline double probability(const LongRange& lr, const Horizon& h, double from, double level) {
    if (!(from > 0) || !(level > 0)) return -1;
    const double d = level > from ? level/from - 1.0 : 1.0 - level/from;
    if (d < lr.dmin*(1 - 1e-9) || d > lr.dmax*(1 + 1e-9)) return -1;  // tolerate price rounding at the ends
    const double side = level > from ? 1.0 : -1.0;
    const double delta = std::abs(std::log(level/from));
    const double x1 = std::log(delta/(lr.rv[1]*std::sqrt(static_cast<double>(h.bars))));
    const double g = std::clamp(std::erfc(std::exp(x1)/std::sqrt(2.0)), 1e-12, 1 - 1e-12);
    const double x[5] = {std::log(g/(1 - g)), x1, std::log(lr.rv[0]/lr.rv[1]), std::log(lr.rv[1]/lr.rv[2]), side};
    double z = h.coef[0];
    for (int i = 0; i < 5; ++i) z += h.coef[i + 1]*(std::clamp(x[i], h.lo[i], h.hi[i]) - h.mean[i])/h.sd[i];
    const double p = 1.0/(1.0 + std::exp(-std::clamp(z, -35.0, 35.0)));
    const auto& kp = h.kp; const auto& kq = h.kq;
    if (p <= kp.front()) return kq.front();
    if (p >= kp.back()) return kq.back();
    size_t i = 1;
    while (kp[i] < p) ++i;
    return kq[i - 1] + (p - kp[i - 1])/(kp[i] - kp[i - 1])*(kq[i] - kq[i - 1]);
}

struct Snapshot {
    bool history = false;
    int64_t available_at = 0;
    std::string symbol;  // research_url::normalize_symbol form (lower case)
    std::string status;  // scored | outside_frozen_universe | mark_window_unavailable | no_current_frame | not_covered
    int64_t asof = 0, expires_at = 0, hide_at = 0;
    double mark = 0;
    std::array<Band, 6> bands{};
    int band_count = 0;
    Tier tier = Tier::None;
    int64_t hour_start = 0, hour_end = 0;
    LongRange longrange;
    // The host already clears at hide_at on its clock; this is the local backstop.
    bool bands_at(int64_t clock) const { return band_count == 6 && clock >= available_at && clock < hide_at && clock < expires_at; }
    bool tier_at(int64_t clock) const { return tier != Tier::None && clock >= hour_start && clock < hour_end; }
};

// Why a covered chart has no bands, in words for the chart note. Null when bands exist.
inline const char* missing_reason(const std::string& status) {
    if (status == "outside_frozen_universe") return "not in the tested market set";
    if (status == "mark_window_unavailable") return "mark history incomplete";
    if (status == "no_current_frame") return "no current forecast";
    if (status == "not_covered") return "market not covered";
    return nullptr;
}

namespace detail {
inline bool integer(const nlohmann::json& j, const char* k, int64_t& out) {
    if (!j.contains(k) || !j[k].is_number_integer()) return false;
    out = j[k].get<int64_t>();
    return true;
}
inline bool number(const nlohmann::json& j, const char* k, double& out) {
    if (!j.contains(k) || !j[k].is_number()) return false;
    out = j[k].get<double>();
    return std::isfinite(out);
}
}

inline bool parse_longrange(const nlohmann::json& j, LongRange& out) {
    using namespace detail;
    LongRange lr;
    auto fill = [](const nlohmann::json& a, double* dst, size_t n) {
        if (!a.is_array() || a.size() != n) return false;
        for (size_t i = 0; i < n; ++i) {
            if (!a[i].is_number()) return false;
            dst[i] = a[i].get<double>();
            if (!std::isfinite(dst[i])) return false;
        }
        return true;
    };
    double range[2];
    if (!j.is_object() || !integer(j, "asof", lr.asof) || !integer(j, "hide_at", lr.hide_at) || lr.hide_at <= lr.asof
        || !number(j, "mark", lr.mark) || lr.mark <= 0 || !j.contains("rv") || !fill(j["rv"], lr.rv, 3)
        || !(lr.rv[0] > 0 && lr.rv[1] > 0 && lr.rv[2] > 0) || !j.contains("distance_range") || !fill(j["distance_range"], range, 2)
        || !(range[0] > 0 && range[1] > range[0]) || !j.contains("horizons") || !j["horizons"].is_array() || j["horizons"].size() != 3) return false;
    lr.available_at = lr.asof;
    if (j.contains("available_at") && (!integer(j,"available_at",lr.available_at) || lr.available_at < lr.asof || lr.available_at >= lr.hide_at)) return false;
    lr.dmin = range[0]; lr.dmax = range[1];
    for (size_t k = 0; k < 3; ++k) {
        const auto& h = j["horizons"][k];
        Horizon& o = lr.horizons[k];
        if (!h.is_object() || !h.contains("name") || !h["name"].is_string() || !h.contains("bars") || !h["bars"].is_number_integer()
            || !h.contains("coefficients") || !fill(h["coefficients"], o.coef, 6) || !h.contains("transform") || !h["transform"].is_object()
            || !h.contains("knots") || !h["knots"].is_object()) return false;
        o.name = h["name"].get<std::string>(); o.bars = h["bars"].get<int>();
        const auto& t = h["transform"];
        for (auto [key, dst] : {std::pair{"lo", o.lo}, std::pair{"hi", o.hi}, std::pair{"mean", o.mean}, std::pair{"sd", o.sd}})
            if (!t.contains(key) || !fill(t[key], dst, 5)) return false;
        for (double v : o.sd) if (!(v > 0)) return false;
        const auto& kn = h["knots"];
        if (!kn.contains("p") || !kn.contains("q") || !kn["p"].is_array() || !kn["q"].is_array() || kn["p"].size() < 2 || kn["p"].size() != kn["q"].size()) return false;
        for (size_t i = 0; i < kn["p"].size(); ++i) {
            if (!kn["p"][i].is_number() || !kn["q"][i].is_number()) return false;
            o.kp.push_back(kn["p"][i].get<double>()); o.kq.push_back(kn["q"][i].get<double>());
            if (!std::isfinite(o.kp.back()) || !std::isfinite(o.kq.back()) || (i > 0 && !(o.kp[i] > o.kp[i - 1]))) return false;
        }
        if (o.bars <= 0) return false;
    }
    lr.valid = true;
    out = std::move(lr);
    return true;
}

// False, with out untouched, for anything but a well-formed v1 payload.
inline bool parse(const nlohmann::json& j, Snapshot& out) {
    using namespace detail;
    if (!j.is_object() || !j.contains("v") || !j["v"].is_number_integer() || j["v"] != 1
        || !j.contains("venue") || j["venue"] != "binancef" || !j.contains("symbol") || !j["symbol"].is_string()
        || !j.contains("status") || !j["status"].is_string() || !j.contains("bands") || !j["bands"].is_array()) return false;
    Snapshot s;
    if (j.contains("history")) { if (!j["history"].is_boolean()) return false; s.history = j["history"].get<bool>(); }
    s.symbol = j["symbol"].get<std::string>();
    s.status = j["status"].get<std::string>();
    if (s.symbol.empty()) return false;
    for (char c : s.symbol) if (c >= 'A' && c <= 'Z') return false;
    if (s.status == "scored") {
        if (!integer(j, "asof", s.asof) || !integer(j, "expires_at", s.expires_at) || !integer(j, "hide_at", s.hide_at)
            || !number(j, "mark", s.mark) || s.mark <= 0 || !(s.asof < s.hide_at && s.hide_at <= s.expires_at)
            || j["bands"].size() != 6) return false;
        s.available_at = s.asof;
        if (j.contains("available_at") && !j["available_at"].is_null() && !integer(j,"available_at",s.available_at)) return false;
        if (s.available_at < s.asof || s.available_at >= s.hide_at) return false;
        bool seen[2][4] = {};
        for (const auto& b : j["bands"]) {
            if (!b.is_object() || !b.contains("side") || !b["side"].is_number_integer() || !b.contains("offset") || !b["offset"].is_number_integer()) return false;
            Band band;
            band.side = b["side"].get<int>(); band.offset = b["offset"].get<int>();
            if ((band.side != -1 && band.side != 1) || band.offset < 1 || band.offset > 3) return false;
            if (!number(b, "low", band.low) || !number(b, "high", band.high) || !number(b, "p", band.p)
                || band.low <= 0 || band.high <= band.low || band.p < 0 || band.p > 1) return false;
            bool& dup = seen[band.side > 0][band.offset];
            if (dup) return false;
            dup = true;
            s.bands[s.band_count++] = band;
        }
    } else if (!missing_reason(s.status) || !j["bands"].empty()) return false;
    if (j.contains("cascade") && !j["cascade"].is_null()) {
        const auto& c = j["cascade"];
        if (!c.is_object() || !c.contains("tier") || !c["tier"].is_string()
            || !integer(c, "hour_start", s.hour_start) || !integer(c, "hour_end", s.hour_end) || s.hour_end <= s.hour_start) return false;
        const auto tier = c["tier"].get<std::string>();
        s.tier = tier == "top1" ? Tier::Top1 : tier == "top5" ? Tier::Top5 : tier == "other" ? Tier::Other : Tier::None;
        if (s.tier == Tier::None) return false;
    }
    if (j.contains("longrange") && !j["longrange"].is_null() && !parse_longrange(j["longrange"], s.longrange)) return false;
    out = std::move(s);
    return true;
}

}  // namespace touch_odds
