#include "ui/replay_library_widget.h"

#include "rendering/theme.h"
#include "replayer/replay_manager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "imgui.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

using json = nlohmann::json;

namespace {

constexpr const char* kDefaultManifestUrl =
    "https://replays.edgedepth.com/library/v1/manifest.json";
constexpr size_t kMaxPacks = 64;

#ifdef __EMSCRIPTEN__

// Resolve once at widget creation. The query parameter wins so a developer can
// test a private/local catalog without rebuilding the terminal.
EM_JS(char*, edrl_resolve_manifest_url, (), {
    var fallback = 'https://replays.edgedepth.com/library/v1/manifest.json';
    var candidate = String();
    try {
        var query = new URLSearchParams(window.location.search).get('replayLibrary');
        candidate = query || window['__EDGEDEPTH_REPLAY_LIBRARY_URL__'] || fallback;
        var resolved = new URL(candidate, window.location.href);
        if (resolved.protocol !== 'https:' && resolved.protocol !== 'http:') {
            resolved = new URL(fallback);
        }
        candidate = resolved.href;
    } catch (_) {
        candidate = fallback;
    }
    var n = lengthBytesUTF8(candidate) + 1;
    var p = _malloc(n);
    stringToUTF8(candidate, p, n);
    return p;
});

// One browser-side catalog state is shared across widget reopenings. A cached
// manifest paints immediately, while every normal open still refreshes it.
EM_JS(void, edrl_fetch_begin, (const char* url_ptr, int force), {
    var url = UTF8ToString(url_ptr);
    var state = Module['__edReplayLibrary'];
    if (!state || state.url !== url) {
        state = {
            url: url,
            payload: String(),
            revision: 0,
            loading: false,
            error: String(),
            cacheTried: false
        };
        Module['__edReplayLibrary'] = state;
    }
    if (state.loading) return;

    var validate = function(text) {
        if (!text || text.length > 1024 * 1024) {
            throw new Error('Manifest is empty or exceeds 1 MB');
        }
        var parsed = JSON.parse(text);
        if (!parsed || parsed.schema !== 'edgedepth.replay-library.v1' ||
            !Array.isArray(parsed.packs)) {
            throw new Error('Unsupported replay-library manifest');
        }
        return text;
    };

    var cacheKey = 'edgedepth.replay_library.v1:' + url;
    if (!force && !state.cacheTried) {
        state.cacheTried = true;
        try {
            var cached = localStorage.getItem(cacheKey);
            if (cached) {
                state.payload = validate(cached);
                state.revision += 1;
            }
        } catch (_) {
            // Cache failures never prevent a network request.
        }
    }

    state.loading = true;
    state.error = String();
    // Cloudflare installations often give all public R2 objects one long edge
    // TTL. A five-minute query bucket keeps the mutable catalog fresh even if
    // that broad rule also catches /library/*. Manual Refresh is immediate.
    var requestUrl = new URL(url);
    requestUrl.searchParams.set(
        '_edrl', force ? String(Date.now()) : String(Math.floor(Date.now() / 300000)));
    fetch(requestUrl.href, {
        method: 'GET', cache: force ? 'reload' : 'no-cache', credentials: 'omit'
    })
        .then(function(response) {
            if (!response.ok) throw new Error('HTTP ' + response.status);
            return response.text();
        })
        .then(function(text) {
            state.payload = validate(text);
            state.revision += 1;
            state.loading = false;
            try { localStorage.setItem(cacheKey, text); } catch (_) {}
        })
        .catch(function(error) {
            state.loading = false;
            state.error = error && error.message ? error.message : 'Catalog request failed';
        });
});

EM_JS(int, edrl_status, (), {
    var state = Module['__edReplayLibrary'];
    if (!state) return 0;
    if (state.payload) return 2;
    if (state.loading) return 1;
    if (state.error) return 3;
    return 0;
});

EM_JS(int, edrl_loading, (), {
    var state = Module['__edReplayLibrary'];
    return state && state.loading ? 1 : 0;
});

EM_JS(int, edrl_revision, (), {
    var state = Module['__edReplayLibrary'];
    return state ? state.revision : 0;
});

EM_JS(char*, edrl_copy_payload, (), {
    var state = Module['__edReplayLibrary'];
    var value = state && state.payload ? state.payload : String();
    var n = lengthBytesUTF8(value) + 1;
    var p = _malloc(n);
    stringToUTF8(value, p, n);
    return p;
});

EM_JS(char*, edrl_copy_error, (), {
    var state = Module['__edReplayLibrary'];
    var value = state && state.error ? state.error : String();
    var n = lengthBytesUTF8(value) + 1;
    var p = _malloc(n);
    stringToUTF8(value, p, n);
    return p;
});

EM_JS(void, edrl_copy_url, (const char* url_ptr), {
    var value = UTF8ToString(url_ptr);
    var fallback = function() {
        var area = document.createElement('textarea');
        area.value = value;
        area.style.position = 'fixed';
        area.style.opacity = '0';
        document.body.appendChild(area);
        area.select();
        try { document.execCommand('copy'); } catch (_) {}
        document.body.removeChild(area);
    };
    if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(value).catch(fallback);
    } else {
        fallback();
    }
});

EM_JS(void, edrl_open_url, (const char* url_ptr), {
    window.open(UTF8ToString(url_ptr), '_blank', 'noopener');
});

#else

static char* edrl_resolve_manifest_url() { return nullptr; }
static void edrl_fetch_begin(const char*, int) {}
static int edrl_status() { return 3; }
static int edrl_loading() { return 0; }
static int edrl_revision() { return 0; }
static char* edrl_copy_payload() { return nullptr; }
static char* edrl_copy_error() { return nullptr; }
static void edrl_copy_url(const char*) {}
static void edrl_open_url(const char*) {}

#endif

std::string upper_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

} // namespace

ReplayLibraryWidget::ReplayLibraryWidget(const AppContext& ctx)
    : ctx_(ctx), manifest_url_(kDefaultManifestUrl) {
#ifdef __EMSCRIPTEN__
    if (char* raw = edrl_resolve_manifest_url()) {
        if (raw[0] != '\0') manifest_url_ = raw;
        std::free(raw);
    }
#endif
    begin_fetch(false);
}

void ReplayLibraryWidget::begin_fetch(bool force) {
    error_.clear();
    edrl_fetch_begin(manifest_url_.c_str(), force ? 1 : 0);
    loading_ = true;
}

bool ReplayLibraryWidget::is_http_url(const std::string& value) {
    if (!(value.rfind("https://", 0) == 0 || value.rfind("http://", 0) == 0)) {
        return false;
    }
    return std::none_of(value.begin(), value.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    });
}

std::string ReplayLibraryWidget::format_duration(int64_t seconds) {
    char out[32];
    if (seconds < 60) {
        std::snprintf(out, sizeof(out), "%llds", static_cast<long long>(seconds));
    } else if (seconds < 3600) {
        std::snprintf(out, sizeof(out), "%lldm", static_cast<long long>(seconds / 60));
    } else {
        const auto hours = seconds / 3600;
        const auto minutes = (seconds % 3600) / 60;
        if (minutes == 0) {
            std::snprintf(out, sizeof(out), "%lldh", static_cast<long long>(hours));
        } else {
            std::snprintf(out, sizeof(out), "%lldh %lldm",
                          static_cast<long long>(hours), static_cast<long long>(minutes));
        }
    }
    return out;
}

std::string ReplayLibraryWidget::format_size(int64_t bytes) {
    char out[32];
    if (bytes >= 1024LL * 1024LL * 1024LL) {
        std::snprintf(out, sizeof(out), "%.1f GB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024LL * 1024LL) {
        std::snprintf(out, sizeof(out), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024LL) {
        std::snprintf(out, sizeof(out), "%.1f KB",
                      static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(out, sizeof(out), "%lld B", static_cast<long long>(bytes));
    }
    return out;
}

bool ReplayLibraryWidget::parse_manifest(
    const std::string& payload, std::string& parse_error) {
    const auto doc = json::parse(payload, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        parse_error = "Catalog returned invalid JSON";
        return false;
    }
    try {
    if (doc.value("schema", std::string()) != "edgedepth.replay-library.v1") {
        parse_error = "Catalog schema is not supported by this terminal build";
        return false;
    }
    const auto it = doc.find("packs");
    if (it == doc.end() || !it->is_array()) {
        parse_error = "Catalog has no packs array";
        return false;
    }

    std::vector<Pack> parsed;
    parsed.reserve(std::min(kMaxPacks, it->size()));
    for (const auto& item : *it) {
        if (parsed.size() >= kMaxPacks) break;
        if (!item.is_object()) continue;

        Pack pack;
        pack.id = item.value("id", std::string());
        pack.title = item.value("title", std::string());
        pack.summary = item.value("summary", std::string());
        pack.exchange = item.value("exchange", std::string());
        pack.symbol = item.value("symbol", std::string());
        pack.display_symbol = item.value("display_symbol", std::string());
        pack.captured_at = item.value("captured_at", std::string());
        pack.pack_url = item.value("pack_url", std::string());
        pack.duration_seconds = item.value("duration_seconds", int64_t{0});
        pack.size_bytes = item.value("size_bytes", int64_t{0});
        pack.featured = item.value("featured", false);

        if (pack.id.empty() || pack.title.empty() || !is_http_url(pack.pack_url)) {
            continue;
        }
        if (pack.display_symbol.empty()) pack.display_symbol = upper_ascii(pack.symbol);
        if (pack.exchange.empty()) pack.exchange = "Recorded market";

        std::string date = pack.captured_at;
        if (date.size() >= 10) date.resize(10);
        pack.metadata_line = pack.exchange;
        if (!date.empty()) pack.metadata_line += " | " + date;
        if (pack.duration_seconds > 0) {
            pack.metadata_line += " | " + format_duration(pack.duration_seconds);
        }
        if (pack.size_bytes > 0) {
            pack.metadata_line += " | " + format_size(pack.size_bytes);
        }

        if (const auto tags = item.find("tags"); tags != item.end() && tags->is_array()) {
            size_t count = 0;
            for (const auto& tag : *tags) {
                if (!tag.is_string() || count++ >= 6) continue;
                if (!pack.tags_line.empty()) pack.tags_line += "   ";
                pack.tags_line += "#" + tag.get<std::string>();
            }
        }
        parsed.push_back(std::move(pack));
    }

    packs_ = std::move(parsed);
    catalog_ready_ = true;
    parse_error.clear();
    return true;
    } catch (const json::exception&) {
        parse_error = "Catalog contains an invalid field type";
        return false;
    }
}

void ReplayLibraryWidget::update() {
    loading_ = edrl_loading() != 0;
    const int revision = edrl_revision();
    if (revision != manifest_revision_ && revision > 0) {
        manifest_revision_ = revision;
        if (char* payload = edrl_copy_payload()) {
            std::string parse_error;
            const bool ok = parse_manifest(payload, parse_error);
            std::free(payload);
            if (ok) error_.clear();
            else error_ = std::move(parse_error);
        }
    }

    if (edrl_status() == 3 && error_.empty()) {
        if (char* error = edrl_copy_error()) {
            error_ = error[0] ? error : "Could not load the replay catalog";
            std::free(error);
        }
    }
}

void ReplayLibraryWidget::play_pack(const Pack& pack) {
    ctx_.replay_mgr().request_pack_replay(pack.pack_url, pack.symbol, 1.0f);
    is_open = false;
}

void ReplayLibraryWidget::render_pack(const Pack& pack, int index) {
    ImGui::PushID(pack.id.c_str());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Tokens::PANEL);
    ImGui::PushStyleColor(ImGuiCol_Border, Theme::Tokens::BD1);
    ImGui::BeginChild("##pack", ImVec2(0.0f, 164.0f), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::PushFont(Theme::Fonts::ui_semibold());
    ImGui::TextColored(Theme::Tokens::BRAND_TX, "%s", pack.display_symbol.c_str());
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextUnformatted(pack.title.c_str());
    ImGui::PopFont();
    if (pack.featured) {
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::TextColored(Theme::Tokens::WARN, "PICK");
    }

    ImGui::PushFont(Theme::Fonts::mono_sm());
    ImGui::TextColored(Theme::Tokens::TX3, "%s", pack.metadata_line.c_str());
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(Theme::Tokens::TX2, "%s", pack.summary.c_str());
    ImGui::PopTextWrapPos();
    if (!pack.tags_line.empty()) {
        ImGui::TextColored(Theme::Tokens::TX3, "%s", pack.tags_line.c_str());
    }

    ImGui::SetCursorPosY(124.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, Theme::Tokens::BRAND);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Tokens::BRAND_TX);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Tokens::BRAND);
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::BRAND_INK);
    if (ImGui::Button("Replay", ImVec2(92.0f, 26.0f))) {
        if (ctx_.replay_mgr().is_active()) {
            pending_pack_ = index;
            replace_popup_requested_ = true;
        } else {
            play_pack(pack);
        }
    }
    ImGui::PopStyleColor(4);
    ImGui::SameLine();
    if (ImGui::Button("Download .edpack", ImVec2(138.0f, 26.0f))) {
        edrl_open_url(pack.pack_url.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy URL", ImVec2(92.0f, 26.0f))) {
        edrl_copy_url(pack.pack_url.c_str());
        notice_ = "Pack URL copied";
        notice_until_ = ImGui::GetTime() + 2.0;
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopID();
}

void ReplayLibraryWidget::render_replace_confirmation() {
    ImGui::SetNextWindowSize(ImVec2(390.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Replace current replay?", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextWrapped("Starting this recording will stop the replay that is currently open.");
    ImGui::Spacing();
    if (ImGui::Button("Cancel", ImVec2(100.0f, 28.0f))) {
        pending_pack_ = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, Theme::Tokens::BRAND);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Tokens::BRAND_TX);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Tokens::BRAND);
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::BRAND_INK);
    if (ImGui::Button("Replace and replay", ImVec2(160.0f, 28.0f))) {
        if (pending_pack_ >= 0 && pending_pack_ < static_cast<int>(packs_.size())) {
            play_pack(packs_[pending_pack_]);
        }
        pending_pack_ = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(4);
    ImGui::EndPopup();
}

void ReplayLibraryWidget::render() {
    if (!is_open) return;

    ImGui::SetNextWindowSize(ImVec2(610.0f, 560.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 360.0f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    if (!ImGui::Begin(title(), &is_open)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    ImGui::PushFont(Theme::Fonts::heading());
    ImGui::TextUnformatted("Replay Library");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextColored(Theme::Tokens::TX3, "PUBLIC TEST DATA");
    ImGui::SameLine(ImGui::GetWindowWidth() - 90.0f);
    if (ImGui::Button(loading_ ? "Loading" : "Refresh", ImVec2(72.0f, 24.0f)) && !loading_) {
        begin_fetch(true);
    }

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(
        Theme::Tokens::TX2,
        "Curated market recordings for deterministic local replay and testing. "
        "Each pack runs client-side with no replay server or account.");
    ImGui::PopTextWrapPos();
    if (ImGui::IsItemHovered()) {
        Theme::tooltip("Manifest: %s", manifest_url_.c_str());
    }
    ImGui::Separator();

    if (!notice_.empty() && ImGui::GetTime() < notice_until_) {
        ImGui::TextColored(Theme::Tokens::BRAND_TX, "%s", notice_.c_str());
    }

    if (!catalog_ready_ && loading_) {
        ImGui::Spacing();
        ImGui::TextColored(Theme::Tokens::TX2, "Loading curated recordings...");
    } else if (!catalog_ready_ && !error_.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(Theme::Tokens::DOWN, "Could not load the replay library");
        ImGui::TextWrapped("%s", error_.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Try again")) begin_fetch(true);
        ImGui::SameLine();
        if (ImGui::Button("Copy manifest URL")) {
            edrl_copy_url(manifest_url_.c_str());
            notice_ = "Manifest URL copied";
            notice_until_ = ImGui::GetTime() + 2.0;
        }
    } else if (packs_.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(Theme::Tokens::TX2, "No recordings are published yet.");
    } else {
        ImGui::TextColored(Theme::Tokens::TX3, "%zu recording%s available%s",
                           packs_.size(), packs_.size() == 1 ? "" : "s",
                           loading_ ? " | refreshing" : "");
        ImGui::Spacing();
        const float footer_height = 104.0f;
        const float list_height = std::max(
            120.0f, ImGui::GetContentRegionAvail().y - footer_height);
        ImGui::BeginChild("##replay_library_list", ImVec2(0.0f, list_height), false);
        for (int i = 0; i < static_cast<int>(packs_.size()); ++i) {
            render_pack(packs_[i], i);
            ImGui::Spacing();
        }
        ImGui::EndChild();
        if (replace_popup_requested_) {
            ImGui::OpenPopup("Replace current replay?");
            replace_popup_requested_ = false;
        }
        render_replace_confirmation();

        ImGui::Separator();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(
            Theme::Tokens::TX2,
            "Your local terminal starts recording today. EdgeDepth has already "
            "been recording 660+ markets for months.");
        ImGui::PopTextWrapPos();
        ImGui::TextColored(
            Theme::Tokens::TX3,
            "Pro lets you replay any moment from the last 30 days.");
        ImGui::PushStyleColor(ImGuiCol_Button, Theme::Tokens::BRAND);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Tokens::BRAND_TX);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Tokens::BRAND);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::BRAND_INK);
        if (ImGui::Button("Unlock 30-day replay", ImVec2(176.0f, 28.0f))) {
            edrl_open_url(
                "https://edgedepth.com/pricing?utm_source=terminal&"
                "utm_medium=replay_library&utm_campaign=oss_time_travel");
        }
        ImGui::PopStyleColor(4);
    }

    ImGui::End();
    ImGui::PopStyleVar();
}
