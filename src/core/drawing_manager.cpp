#include "core/drawing_manager.h"

#include <algorithm>
#include <unordered_map>

#include <nlohmann/json.hpp>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// localStorage bridge - menu.cpp recents pattern, but with a DYNAMIC-length
// read (drawing blobs can exceed any fixed buffer; caller frees).
EM_JS(char*, eddraw_read_blob, (const char* key), {
    try {
        const raw = localStorage.getItem(UTF8ToString(key));
        if (!raw) return 0;
        const len = lengthBytesUTF8(raw) + 1;
        const p = _malloc(len);
        stringToUTF8(raw, p, len);
        return p;
    } catch (e) { return 0; }
});

EM_JS(void, eddraw_write_blob, (const char* key, const char* val), {
    try { localStorage.setItem(UTF8ToString(key), UTF8ToString(val)); }
    catch (e) { /* quota/blocked: drawings just don't persist */ }
});

// pagehide/visibility flush so a tab close within the 1s debounce window
// doesn't lose the last mutation (display_time_zone bootstrap pattern).
EM_JS(void, eddraw_register_flush, (const char* key), {
    if (Module.__eddraw_flush_registered) return;
    Module.__eddraw_flush_registered = true;
    const kick = () => { try { Module._edgedepth_drawings_flush(); } catch (e) {} };
    addEventListener('pagehide', kick);
    document.addEventListener('visibilitychange', () => {
        if (document.visibilityState === 'hidden') kick();
    });
    // Another terminal window wrote this symbol's drawings: merge, never clobber.
    const mine = UTF8ToString(key);
    addEventListener('storage', (event) => {
        if (event.key === mine) { try { Module._edgedepth_drawings_changed(); } catch (e) {} }
    });
});
#endif

namespace {
constexpr double kPersistDebounceSec = 1.0;
constexpr size_t kUndoDepth = 64;
DrawingManager* g_drawing_mgr_instance = nullptr;
}  // namespace

#ifdef __EMSCRIPTEN__
extern "C" {
EMSCRIPTEN_KEEPALIVE
void edgedepth_drawings_flush() {
    if (g_drawing_mgr_instance) g_drawing_mgr_instance->flush();
}
EMSCRIPTEN_KEEPALIVE
void edgedepth_drawings_changed() {
    if (g_drawing_mgr_instance) g_drawing_mgr_instance->note_external_change();
}
}
#endif

#ifdef __EMSCRIPTEN__
namespace {
int64_t wall_clock_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
}  // namespace
#endif

void DrawingManager::init(const std::string& exchange, const std::string& symbol) {
    storage_key_ = "edgedepth.drawings.v1:" + exchange + ":" + symbol;
    // Seed the id counter from wall-clock ms so ids stay unique across sessions
    // (ids also key undo ops; collisions would silently cross-wire restores).
    next_id_ = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) * 1000ull;
    g_drawing_mgr_instance = this;

#ifdef __EMSCRIPTEN__
    eddraw_register_flush(storage_key_.c_str());
    if (char* raw = eddraw_read_blob(storage_key_.c_str())) {
        load_json(raw);
        free(raw);
    }
#endif
    remember_flushed(items_);
    loaded_ = true;
}

DrawingManager::StoredBlob DrawingManager::parse_blob(const char* raw) {
    StoredBlob out;
    if (!raw) return out;
    // Exceptions are disabled in this build: non-throwing parse + is_discarded.
    const nlohmann::json j = nlohmann::json::parse(raw, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("v", 0) != 1) return out;
    out.ok = true;
    out.magnet = j.value("magnet", true);
    out.hidden_all = j.value("hidden_all", false);
    out.rail_collapsed = j.value("rail_collapsed", false);
    const auto items = j.find("items");
    if (items == j.end() || !items->is_array()) return out;

    for (const auto& ji : *items) {
        if (out.items.size() >= drawing::kMaxDrawings) break;
        if (!ji.is_object()) continue;
        const drawing::Tool tool =
            drawing::tool_from_key(ji.value("tool", std::string()));
        if (tool == drawing::Tool::COUNT) continue;  // unknown / never-stored
        const auto anchors = ji.find("anchors");
        if (anchors == ji.end() || !anchors->is_array() || anchors->empty())
            continue;

        drawing::Drawing d;
        d.tool = tool;
        d.id = ji.value("id", 0ull);
        d.modified_ms = ji.value("m", 0ll);
        const size_t cap = tool == drawing::Tool::Brush ? drawing::kMaxBrushPoints
                         : tool == drawing::Tool::Polyline
                             ? drawing::kMaxPolylinePoints
                             : 8;
        for (const auto& ja : *anchors) {
            if (d.anchors.size() >= cap) break;
            if (!ja.is_array() || ja.size() < 2) continue;
            d.anchors.push_back(drawing::Anchor{
                ja[0].get<int64_t>(), ja[1].get<double>()});
        }
        if (d.anchors.empty()) continue;
        d.style.color = ji.value("color", drawing::kDefaultColor);
        d.style.width = ji.value("width", 1.5f);
        const int pat = ji.value("pattern", 0);
        d.style.pattern = pat >= 0 && pat <= 2
                              ? static_cast<drawing::LinePattern>(pat)
                              : drawing::LinePattern::Solid;
        d.style.fill = ji.value("fill", 0u);
        d.locked = ji.value("locked", false);
        d.hidden = ji.value("hidden", false);
        d.text = ji.value("text", std::string());
        d.font_size = ji.value("font_size", 14.0f);
        d.fib_mask = ji.value("fib_mask", drawing::kFibDefaultMask);
        d.stop = ji.value("stop", 0.0);
        d.target = ji.value("target", 0.0);
        d.chan_off = ji.value("chan_off", 0.0);
        out.items.push_back(std::move(d));
    }
    return out;
}

void DrawingManager::load_json(const char* raw) {
    StoredBlob blob = parse_blob(raw);
    if (!blob.ok) return;
    magnet_ = blob.magnet;
    hidden_all_ = blob.hidden_all;
    rail_collapsed_ = blob.rail_collapsed;
    for (drawing::Drawing& d : blob.items) {
        if (items_.size() >= drawing::kMaxDrawings) break;
        if (d.id == 0) d.id = ++next_id_;
        next_id_ = std::max(next_id_, d.id);
        items_.push_back(std::move(d));
    }
}

std::string DrawingManager::serialize_item(const drawing::Drawing& d, bool with_stamp) {
    const char* key = drawing::tool_key(d.tool);
    if (key[0] == '\0') return {};  // Cursor/Measure are never stored
    nlohmann::json ji;
    ji["id"] = d.id;
    if (with_stamp) ji["m"] = d.modified_ms;
    ji["tool"] = key;
    nlohmann::json ja = nlohmann::json::array();
    for (const drawing::Anchor& a : d.anchors)
        ja.push_back(nlohmann::json::array({a.t_ms, a.price}));
    ji["anchors"] = std::move(ja);
    ji["color"] = d.style.color;
    ji["width"] = d.style.width;
    ji["pattern"] = static_cast<int>(d.style.pattern);
    if (d.style.fill) ji["fill"] = d.style.fill;
    if (d.locked) ji["locked"] = true;
    if (d.hidden) ji["hidden"] = true;
    switch (d.tool) {
        case drawing::Tool::Text:
            ji["text"] = d.text;
            ji["font_size"] = d.font_size;
            break;
        case drawing::Tool::Fib:
            ji["fib_mask"] = d.fib_mask;
            break;
        case drawing::Tool::LongPosition:
        case drawing::Tool::ShortPosition:
            ji["stop"] = d.stop;
            ji["target"] = d.target;
            break;
        case drawing::Tool::Channel:
            ji["chan_off"] = d.chan_off;
            break;
        default:
            break;
    }
    // error_handler_t::replace: invalid UTF-8 in user text must not throw
    // (exceptions are disabled) or lose the whole blob.
    return ji.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string DrawingManager::serialize() const {
    std::string out = "{\"v\":1,\"magnet\":";
    out += magnet_ ? "true" : "false";
    out += ",\"hidden_all\":";
    out += hidden_all_ ? "true" : "false";
    out += ",\"rail_collapsed\":";
    out += rail_collapsed_ ? "true" : "false";
    out += ",\"items\":[";
    bool first = true;
    for (const drawing::Drawing& d : items_) {
        const std::string item = serialize_item(d, true);
        if (item.empty()) continue;
        if (!first) out += ',';
        first = false;
        out += item;
    }
    out += "]}";
    return out;
}

void DrawingManager::remember_flushed(const std::vector<drawing::Drawing>& items) {
    last_flushed_.clear();
    for (const drawing::Drawing& d : items) {
        std::string body = serialize_item(d, false);
        if (!body.empty()) last_flushed_[d.id] = std::move(body);
    }
}

drawing::Drawing* DrawingManager::find(uint64_t id) {
    for (auto& d : items_)
        if (d.id == id) return &d;
    return nullptr;
}

uint64_t DrawingManager::add(drawing::Drawing d) {
    if (items_.size() >= drawing::kMaxDrawings) return 0;
    if (d.id == 0) d.id = ++next_id_;
    const uint64_t id = d.id;
    push_undo(UndoOp{UndoOp::Kind::Create, id, {}});
    items_.push_back(std::move(d));
    mark_dirty();
    return id;
}

bool DrawingManager::remove(uint64_t id) {
    auto it = std::find_if(items_.begin(), items_.end(),
                           [id](const drawing::Drawing& d) { return d.id == id; });
    if (it == items_.end()) return false;
    push_undo(UndoOp{UndoOp::Kind::Delete, id, *it});
    items_.erase(it);
    if (selected_ == id) selected_ = 0;
    mark_dirty();
    return true;
}

void DrawingManager::clear_all() {
    if (items_.empty()) return;
    items_.clear();
    selected_ = 0;
    undo_stack_.clear();  // bulk wipe is deliberate; no 200-op undo replay
    mark_dirty();
}

void DrawingManager::begin_modify(uint64_t id) {
    if (const drawing::Drawing* d = find(id)) {
        modify_snapshot_ = *d;
        modify_id_ = id;
    }
}

void DrawingManager::end_modify(uint64_t id) {
    if (modify_id_ != id || modify_id_ == 0) return;
    if (const drawing::Drawing* d = find(id)) {
        // Only a real change earns an undo op (a click that never dragged
        // should not eat an undo step).
        const bool changed =
            d->anchors.size() != modify_snapshot_.anchors.size() ||
            !std::equal(d->anchors.begin(), d->anchors.end(),
                        modify_snapshot_.anchors.begin(),
                        [](const drawing::Anchor& a, const drawing::Anchor& b) {
                            return a.t_ms == b.t_ms && a.price == b.price;
                        }) ||
            d->stop != modify_snapshot_.stop || d->target != modify_snapshot_.target ||
            d->chan_off != modify_snapshot_.chan_off || d->text != modify_snapshot_.text ||
            d->style.color != modify_snapshot_.style.color ||
            d->style.width != modify_snapshot_.style.width ||
            d->style.pattern != modify_snapshot_.style.pattern ||
            d->style.fill != modify_snapshot_.style.fill ||
            d->locked != modify_snapshot_.locked ||
            d->hidden != modify_snapshot_.hidden ||
            d->fib_mask != modify_snapshot_.fib_mask ||
            d->font_size != modify_snapshot_.font_size;
        if (changed) {
            push_undo(UndoOp{UndoOp::Kind::Modify, id, modify_snapshot_});
            mark_dirty();
        }
    }
    modify_id_ = 0;
}

void DrawingManager::undo() {
    if (undo_stack_.empty()) return;
    UndoOp op = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    switch (op.kind) {
        case UndoOp::Kind::Create: {
            auto it = std::find_if(items_.begin(), items_.end(),
                                   [&](const drawing::Drawing& d) { return d.id == op.id; });
            if (it != items_.end()) items_.erase(it);
            if (selected_ == op.id) selected_ = 0;
            break;
        }
        case UndoOp::Kind::Delete:
            if (items_.size() < drawing::kMaxDrawings) items_.push_back(std::move(op.before));
            break;
        case UndoOp::Kind::Modify:
            if (drawing::Drawing* d = find(op.id)) *d = std::move(op.before);
            break;
    }
    mark_dirty();
}

void DrawingManager::push_undo(UndoOp op) {
    undo_stack_.push_back(std::move(op));
    while (undo_stack_.size() > kUndoDepth) undo_stack_.pop_front();
}

void DrawingManager::mark_dirty() {
    dirty_ = true;
    dirty_at_ = std::chrono::steady_clock::now();
}

void DrawingManager::tick() {
    if (!loaded_) return;
    if (pull_pending_) {
        pull_pending_ = false;
        // Local unflushed edits merge on the way out; otherwise take theirs.
        if (dirty_) flush(); else pull();
    }
    if (!dirty_) return;
    const double since = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - dirty_at_).count();
    if (since >= kPersistDebounceSec) flush();
}

std::vector<drawing::Drawing> DrawingManager::merge_for_flush(
    std::vector<drawing::Drawing> stored, std::vector<drawing::Drawing> mine,
    const SyncedItems& synced, uint64_t editing_id, int64_t now) {
    std::unordered_map<uint64_t, size_t> here;
    for (size_t i = 0; i < mine.size(); ++i) {
        drawing::Drawing& d = mine[i];
        here[d.id] = i;
        const auto prev = synced.find(d.id);
        if (prev == synced.end() || prev->second != serialize_item(d, false))
            d.modified_ms = now;
    }
    std::vector<drawing::Drawing> merged;
    for (drawing::Drawing& theirs : stored) {
        const auto at = here.find(theirs.id);
        if (at == here.end()) {
            // Not in this window: the other window added it, or this window
            // deleted it since its last sync.
            if (synced.count(theirs.id) == 0 && merged.size() < drawing::kMaxDrawings)
                merged.push_back(std::move(theirs));
            continue;
        }
        drawing::Drawing& ours = mine[at->second];
        const bool take_theirs = theirs.modified_ms > ours.modified_ms && ours.id != editing_id;
        merged.push_back(take_theirs ? std::move(theirs) : std::move(ours));
        at->second = static_cast<size_t>(-1);
    }
    for (const auto& [id, index] : here)
        if (index != static_cast<size_t>(-1) && merged.size() < drawing::kMaxDrawings)
            merged.push_back(std::move(mine[index]));
    // This window's order first (mine still holds ids in order), arrivals after.
    std::vector<drawing::Drawing> ordered;
    ordered.reserve(merged.size());
    for (const drawing::Drawing& d : mine)
        for (drawing::Drawing& m : merged)
            if (m.id != 0 && m.id == d.id) { ordered.push_back(std::move(m)); m.id = 0; break; }
    for (drawing::Drawing& m : merged)
        if (m.id != 0) ordered.push_back(std::move(m));
    return ordered;
}

std::vector<drawing::Drawing> DrawingManager::merge_for_pull(
    std::vector<drawing::Drawing> stored, std::vector<drawing::Drawing> mine,
    const SyncedItems& synced, uint64_t editing_id) {
    std::unordered_map<uint64_t, size_t> theirs;
    for (size_t i = 0; i < stored.size(); ++i) theirs[stored[i].id] = i;
    std::vector<drawing::Drawing> next;
    next.reserve(stored.size());
    for (drawing::Drawing& d : mine) {
        const auto it = theirs.find(d.id);
        if (it == theirs.end()) {
            if (synced.count(d.id) == 0 || d.id == editing_id) next.push_back(std::move(d));
            continue;  // synced before and gone now: deleted elsewhere
        }
        drawing::Drawing& s = stored[it->second];
        const bool take_theirs = s.modified_ms > d.modified_ms && d.id != editing_id;
        next.push_back(take_theirs ? std::move(s) : std::move(d));
        it->second = static_cast<size_t>(-1);
    }
    for (auto& [id, index] : theirs)
        if (index != static_cast<size_t>(-1) && next.size() < drawing::kMaxDrawings)
            next.push_back(std::move(stored[index]));
    return next;
}

// Two terminal windows on one symbol share the blob, so a flush merges with
// what is stored instead of replacing it; see merge_for_flush.
void DrawingManager::flush() {
    if (!loaded_) return;
    dirty_ = false;
#ifdef __EMSCRIPTEN__
    std::vector<drawing::Drawing> stored;
    if (char* raw = eddraw_read_blob(storage_key_.c_str())) {
        stored = std::move(parse_blob(raw).items);
        free(raw);
    }
    items_ = merge_for_flush(std::move(stored), std::move(items_), last_flushed_, modify_id_, wall_clock_ms());
    for (const drawing::Drawing& d : items_) next_id_ = std::max(next_id_, d.id);
    if (selected_ != 0 && !find(selected_)) selected_ = 0;
    remember_flushed(items_);
    const std::string blob = serialize();
    eddraw_write_blob(storage_key_.c_str(), blob.c_str());
#endif
}

// The other window wrote and this window has nothing unflushed; see merge_for_pull.
void DrawingManager::pull() {
#ifdef __EMSCRIPTEN__
    char* raw = eddraw_read_blob(storage_key_.c_str());
    if (!raw) return;
    StoredBlob stored = parse_blob(raw);
    free(raw);
    if (!stored.ok) return;
    items_ = merge_for_pull(std::move(stored.items), std::move(items_), last_flushed_, modify_id_);
    for (const drawing::Drawing& d : items_) next_id_ = std::max(next_id_, d.id);
    if (selected_ != 0 && !find(selected_)) selected_ = 0;
    remember_flushed(items_);
#endif
}
