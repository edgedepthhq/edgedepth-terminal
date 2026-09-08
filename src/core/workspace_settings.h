#pragma once
#include <nlohmann/json.hpp>
#include <cmath>
#include <type_traits>

namespace workspace {
using Json = nlohmann::json;
// Imported settings are data. Ignore wrong types and out-of-range numbers;
// never invoke throwing conversions in the exception-free browser build.
inline void read(const Json& j, const char* key, bool& out) {
    auto it = j.find(key);
    if (it != j.end() && it->is_boolean()) out = it->get<bool>();
}
template<class T> void read(const Json& j, const char* key, T& out, double lo, double hi) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return;
    const double v = it->get<double>();
    if (!std::isfinite(v) || v < lo || v > hi) return;
    if constexpr (std::is_integral_v<T> || std::is_enum_v<T>)
        if (std::floor(v) != v) return;
    out = static_cast<T>(v);
}
inline const Json& object(const Json& j, const char* key) {
    static const Json empty = Json::object();
    auto it = j.find(key);
    return it != j.end() && it->is_object() ? *it : empty;
}
}
