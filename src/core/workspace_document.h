#pragma once
#include "workspace_settings.h"
#include <string>
#include <string_view>
#include <set>

namespace workspace {
inline constexpr size_t max_bytes = 262144;
inline constexpr size_t max_named = 20;

inline void remap_window_title(std::string& ini, const std::string& old_title,
                              const std::string& new_title) {
    if (old_title.empty()) return;
    const auto old_id = old_title.rfind("###"), new_id = new_title.rfind("###");
    const std::string target = "[Window][" + new_title.substr(
        new_id == std::string::npos ? 0 : new_id + 3) + "]";
    const auto replace_header = [&](const std::string& title) {
        const std::string header = "[Window][" + title + "]";
        if (header == target) return;
        for (size_t pos = 0; (pos = ini.find(header, pos)) != std::string::npos; pos += target.size())
            ini.replace(pos, header.size(), target);
    };
    // Current ImGui writes only the ID after ###. Also accept older exports
    // retaining the marker and verbose exports retaining the visible title.
    replace_header(old_title);
    if (old_id != std::string::npos) {
        replace_header(old_title.substr(old_id));
        replace_header(old_title.substr(old_id + 3));
    }
}

inline bool valid_document(const Json& j) {
    if (!j.is_object()) return false;
    auto version = j.find("version"), widgets = j.find("widgets"), ini = j.find("layout");
    if (version == j.end() || !version->is_number_integer() || *version != 1 ||
        widgets == j.end() || !widgets->is_array() || widgets->empty() || widgets->size() > 24 ||
        ini == j.end() || !ini->is_string() || ini->get_ref<const std::string&>().size() > 131072) return false;
    bool chart = false;
    std::set<std::string> types;
    for (const auto& w : *widgets) {
        if (!w.is_object()) return false;
        auto t = w.find("type"), s = w.find("settings"), title = w.find("title");
        if (t == w.end() || !t->is_string() || s == w.end() || !s->is_object() ||
            title == w.end() || !title->is_string()) return false;
        const auto& name = t->get_ref<const std::string&>();
        if (name != "chart" && name != "dom" && name != "trades" && name != "depth" &&
            name != "stats" && name != "watchlist" && name != "library" && name != "paper") return false;
        // One instance of each panel: the current terminal shares one candle context.
        if (!types.insert(name).second || title->get_ref<const std::string&>().size() > 256) return false;
        chart |= name == "chart";
        if (name == "chart") {
            auto inds = s->find("indicators");
            if (inds != s->end() && (!inds->is_array() || inds->size() > 16)) return false;
        }
    }
    return chart;
}
inline Json parse_bounded(std::string_view text, size_t limit = max_bytes) {
    if (text.size() > limit) return {};
    // Bound nesting before parsing, including malformed input. Strings can
    // contain braces; escaped quotes must not change the scanner's state.
    int depth = 0; bool quoted = false, escaped = false;
    for (char c : text) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            continue;
        }
        if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 16) return {}; }
        else if (c == '}' || c == ']') { if (--depth < 0) return {}; }
    }
    return Json::parse(text, nullptr, false);
}
inline Json parse_document(std::string_view text) {
    auto j = parse_bounded(text);
    return valid_document(j) ? j : Json{};
}
}
