#include "workspace_manager.h"
#include "imgui_internal.h"
#include "workspace_document.h"
#include "core/symbol_metadata.h"
#include "core/education_boot.h"
#include "core/recorder_glue.h"
#include "replayer/replay_manager.h"
#include "rendering/layout.h"
#include "rendering/theme.h"
#include "ui/chart_widget.h"
#include "ui/dom_widget.h"
#include "ui/trades_widget.h"
#include "ui/orderbook_widget.h"
#include "ui/stats_widget.h"
#include "ui/watchlist_widget.h"
#include "ui/replay_library_widget.h"
#include "ui/positions_panel.h"
#include <cstring>
#include <cstdlib>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(char*, workspace_read, (), {
    try { const value = localStorage.getItem('edgedepth.workspaces.v1') || '';
          if (value.length > 4194304) return 0;
          return stringToNewUTF8(value); } catch (_) { return 0; }
});
EM_JS(int, workspace_write, (const char* text), {
    try { localStorage.setItem('edgedepth.workspaces.v1', UTF8ToString(text)); return 1; }
    catch (_) { return 0; }
});
EM_JS(void, workspace_export, (const char* text), {
    const url = URL.createObjectURL(new Blob([UTF8ToString(text)], {type:'application/json'}));
    const a = document.createElement('a'); a.href = url; a.download = 'edgedepth-workspace.json';
    a.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
});
EM_JS(void, workspace_import, (), {
    const input = document.createElement('input'); input.type = 'file'; input.accept = '.json';
    input.onchange = async () => {
        const file = input.files && input.files[0]; if (!file) return;
        let text = '';
        try { if (file.size <= 262144) text = await file.text(); } catch (_) {}
        const p = stringToNewUTF8(text); _workspace_import_text(p); _free(p);
    };
    input.click();
});
#endif
namespace workspace {
namespace {
Json library = {{"version",1},{"named",Json::object()}};
Json pending, current;
std::vector<std::unique_ptr<Widget>>* panels = nullptr;
const AppContext* context = nullptr;
Terminal::Pair active_pair;
bool ready = false, enabled_now = false, was_enabled = false;
bool save_requested = false, export_requested = false;
int capture_after_frame = 0;
double last_capture = -1;
char name[49] = {};
std::string notice;
std::string last_written;
const char* key(WidgetType type) {
    switch(type) {
    case WidgetType::Chart:return "chart"; case WidgetType::DOM:return "dom";
    case WidgetType::Trades:return "trades"; case WidgetType::Orderbook:return "depth";
    case WidgetType::Stats:return "stats"; case WidgetType::Watchlist:return "watchlist";
    case WidgetType::ReplayLibrary:return "library"; case WidgetType::PaperTrading:return "paper";
    default:return nullptr;
    }
}
Json capture() {
    Json doc = {{"version",1},{"widgets",Json::array()},{"layout",ImGui::SaveIniSettingsToMemory()}};
    std::set<std::string> seen;
    for (const auto& w : *panels) {
        if (!w || !w->is_open || w->is_replay_widget) continue;
        const auto* type = key(w->type()); if (!type) continue;
        if (!seen.insert(type).second) {
            notice = "Use one panel of each type before saving a workspace."; return {};
        }
        doc["widgets"].push_back({{"type",type},{"title",w->title()},{"settings",w->save_settings()}});
    }
    return valid_document(doc) ? doc : Json{};
}
bool persist() {
    const std::string bytes = library.dump();
    if (bytes.size() > 4194304) {
        notice = "Workspace library is full. Delete a saved copy or export this setup."; return false;
    }
    if (bytes == last_written) return true;
#ifdef __EMSCRIPTEN__
    if (!workspace_write(bytes.c_str())) {
        notice = "Browser storage unavailable or full. Export your workspace to keep it."; return false;
    }
#endif
    last_written = bytes;
    return true;
}
void replace_all(std::string& text, const std::string& from, const std::string& to) {
    if (from.empty() || from == to) return;
    for (size_t at = 0; (at = text.find(from, at)) != std::string::npos; at += to.size())
        text.replace(at, from.size(), to);
}
void apply(const Json& doc) {
    if (!valid_document(doc)) return;
    const auto fmt = SymbolRegistry::instance().get_formatter(active_pair.exchange, active_pair.symbol);
    const auto tick_size = SymbolRegistry::instance().tick_or_zero(active_pair.exchange, active_pair.symbol);
    // Retire subscribers before recreating the same keys. The context is still
    // live and stable here, and no widget iteration is in progress.
    panels->clear();
    std::string ini = doc["layout"].get<std::string>();
    for (const auto& row : doc["widgets"]) {
        const auto type = row["type"].get<std::string>();
        std::unique_ptr<Widget> w;
        if (type == "chart") w = std::make_unique<ChartWidget>(active_pair,*context,tick_size);
        else if (type == "dom") w = std::make_unique<DOMWidget>(active_pair,*context,tick_size,20);
        else if (type == "trades") w = std::make_unique<TradesWidget>(active_pair,*context,fmt);
        else if (type == "depth") w = std::make_unique<OrderbookWidget>(active_pair,*context,fmt);
        else if (type == "stats") w = std::make_unique<StatsWidget>(active_pair,*context,fmt);
        else if (type == "watchlist") w = std::make_unique<WatchlistWidget>(active_pair,*context);
        else if (type == "library") w = std::make_unique<ReplayLibraryWidget>(*context);
        else if (type == "paper") w = std::make_unique<PositionsPanel>(*context);
        if (!w) continue;
        w->load_settings(row["settings"]);
        replace_all(ini, row["title"].get<std::string>(), w->title());
        // Chart's ### ID is stored without its changing visible title by ImGui.
        const auto old = row["title"].get<std::string>();
        const std::string fresh = w->title();
        if (old.find("###") != std::string::npos && fresh.find("###") != std::string::npos)
            replace_all(ini, old.substr(old.find("###")), fresh.substr(fresh.find("###")));
        panels->push_back(std::move(w));
    }
    ImGui::ClearIniSettings();
    if (!ini.empty()) {
        ImGui::LoadIniSettingsFromMemory(ini.c_str(),ini.size());
        LayoutManager::restore_layout_for(active_pair.exchange,active_pair.symbol);
    }
    else LayoutManager::reset_layout_for(active_pair.exchange,active_pair.symbol);
}
Json preset(int index) {
    Json settings = {{"chart_type",index == 0 ? 1 : 0},{"timeframe",60},
        {"rt_mode",index == 1},{"liq_dense_field",index == 3},{"liq_profile_enabled",false},
        {"heatmap_enabled",index == 1},{"indicators",Json::array()}};
    if (index != 1) {
        settings["indicators"].push_back({{"name","Vol (USDT)"}});
        settings["indicators"].push_back({{"name","CVD"}});
    }
    if (index == 0) settings["footprint"] = {{"comparison",1},{"stacked_levels",3}};
    Json d = {{"version",1},{"layout",""},{"widgets",Json::array()}};
    for (const auto* type : {"chart","dom","trades","watchlist"})
        d["widgets"].push_back({{"type",type},{"title",""},
            {"settings",std::string(type)=="chart"?settings:Json::object()}});
    if (index == 2) d["widgets"].push_back({{"type","library"},{"title",""},{"settings",Json::object()}});
    return d;
}
}
void flush() {
    if (!ready || !enabled_now || !panels || !context || context->replay_mgr().is_active() ||
        ImGui::GetFrameCount() <= capture_after_frame) return;
    auto next = capture();
    if (next.is_null() || next.dump().size() > max_bytes) return;
    current = std::move(next); library["current"] = current; persist();
}
void tick(std::vector<std::unique_ptr<Widget>>& widgets, const AppContext& ctx,
          const Terminal::Pair& pair, bool enabled) {
    panels = &widgets; context = &ctx; active_pair = pair;
    enabled_now = enabled;
    if (!enabled) { was_enabled = false; return; }
    if (!ready) {
        ready = true;
#ifdef __EMSCRIPTEN__
        char* raw = workspace_read();
        if (raw && *raw) {
            auto stored = parse_bounded(raw,4194304); std::free(raw);
            if (stored.is_object() && stored.contains("version") && stored["version"] == 1 &&
                stored.contains("named") && stored["named"].is_object() && stored["named"].size() <= max_named) {
                library["named"] = Json::object();
                for (const auto& item : stored["named"].items())
                    if (item.key().size() <= 48 && valid_document(item.value())) library["named"][item.key()] = item.value();
                if (stored.contains("current") && valid_document(stored["current"])) {
                    pending = stored["current"];
                    if (!stored.contains("layout_revision") || stored["layout_revision"] != 2) {
                        bool rt = false;
                        for (const auto& row : pending["widgets"])
                            if (row["type"] == "chart")
                                rt = row["settings"].value("rt_mode", Json(false)) == true;
                        pending = preset(rt ? 1 : 3);
                        notice = "Default layout repaired. Your named workspaces are still available.";
                    }
                }
            } else if (!stored.is_null()) notice = "Saved workspace could not be read. Defaults are available.";
        } else std::free(raw);
        EM_ASM({
            window.addEventListener('pagehide', () => _workspace_flush());
            document.addEventListener('visibilitychange', () => { if (document.hidden) _workspace_flush(); });
        });
#endif
    } else if (!was_enabled && !current.is_null()) pending = current;
    was_enabled = true;
    if (!pending.is_null()) {
        apply(pending); pending = Json{}; last_capture = -1;
        library["layout_revision"] = 2;
        // DockBuilder and new windows need a completed frame before capture.
        capture_after_frame = ImGui::GetFrameCount() + 1;
        return;
    }
    if (ImGui::GetFrameCount() <= capture_after_frame) return;
    library["layout_revision"] = 2;
    if (save_requested || export_requested || ImGui::GetTime() - last_capture >= 1.0) {
        auto next = capture(); last_capture = ImGui::GetTime();
        if (next.is_null() || next.dump().size() > max_bytes) {
            save_requested = export_requested = false; return;
        }
        current = std::move(next); library["current"] = current; persist();
        if (save_requested && !current.is_null()) {
            library["named"][name] = current;
            if (persist()) notice = "Workspace saved in this browser.";
        }
#ifdef __EMSCRIPTEN__
        if (export_requested && !current.is_null()) workspace_export(current.dump(2).c_str());
#endif
        save_requested = export_requested = false;
    }
}
void reset_default() {
    if (!enabled_now || !panels) { LayoutManager::reset_layout(); return; }
    bool rt = false;
    for (const auto& w : *panels)
        if (w && w->type() == WidgetType::Chart) {
            rt = static_cast<const ChartWidget*>(w.get())->rt_mode(); break;
        }
    pending = preset(rt ? 1 : 3);
}
void menu() {
    ImGui::TextDisabled("Workspaces apply to the current market");
    ImGui::TextDisabled("Changes are saved in this browser");
    ImGui::BeginDisabled(!enabled_now);
    if (ImGui::BeginMenu("Presets")) {
        const char* labels[] = {"Order Flow", "Liquidity", "Replay Review", "Candles"};
        for (int i=0;i<4;++i) if (ImGui::MenuItem(labels[i])) pending = preset(i);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Saved workspaces")) {
        if (library["named"].empty()) ImGui::TextDisabled("No saved workspaces yet");
        std::string remove;
        for (const auto& item : library["named"].items()) {
            ImGui::PushID(item.key().c_str());
            if (ImGui::BeginMenu(item.key().c_str())) {
                if (ImGui::MenuItem("Load")) pending = item.value();
                if (ImGui::MenuItem("Delete saved copy")) remove = item.key();
                ImGui::EndMenu();
            }
            ImGui::PopID();
        }
        if (!remove.empty()) { library["named"].erase(remove); persist(); }
        ImGui::EndMenu();
    }
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##workspace_name","Workspace name",name,sizeof(name));
    const bool exists = library["named"].contains(name);
    ImGui::BeginDisabled(!name[0] || (!exists && library["named"].size() >= max_named));
    if (ImGui::Button(exists ? "Update saved workspace" : "Save workspace")) save_requested = true;
    ImGui::EndDisabled();
    if (ImGui::MenuItem("Export current workspace")) export_requested = true;
#ifdef __EMSCRIPTEN__
    if (ImGui::MenuItem("Import workspace...")) workspace_import();
#endif
    ImGui::EndDisabled();
    if (!enabled_now) ImGui::TextDisabled("Return to live to manage workspaces");
    if (!notice.empty()) { ImGui::Separator(); ImGui::TextWrapped("%s",notice.c_str()); }
}
void import_text(const char* text) {
    auto doc = parse_document(text ? text : "");
    if (doc.is_null()) { notice = "Invalid workspace. Use a version 1 export under 256 KiB."; return; }
    if (!enabled_now) { notice = "Return to live before importing a workspace."; return; }
    pending = std::move(doc); notice = "Imported workspace. Save a name to keep another copy.";
}
}
#ifdef __EMSCRIPTEN__
extern "C" {
EMSCRIPTEN_KEEPALIVE void workspace_flush() { workspace::flush(); }
EMSCRIPTEN_KEEPALIVE void workspace_import_text(const char* text) { workspace::import_text(text); }
}
#endif
