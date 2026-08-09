#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// DrawingManager — store + shared state for the chart drawing tools.
//
// Owned by AppState (NOT DataContext) so drawings survive live<->replay context
// swaps untouched; anchors are (epoch_ms, price) so they land correctly on
// historical candles. Main-thread only (no data-thread writes, no mutex).
//
// Split of responsibilities:
//   DrawingManager (here)      — items, armed tool, selection, undo, global
//                                toggles, per-symbol localStorage persistence.
//   drawing::DrawingLayer (ui) — placement state machine, hit-testing, drags,
//                                all in-plot rendering.
//   drawing_toolbar (ui)       — left icon rail + topbar dropdown.
// ═══════════════════════════════════════════════════════════════════════════════
#include "ui/drawing/drawing_types.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

class DrawingManager {
public:
    // Loads the per-symbol set from localStorage (key
    // edgedepth.drawings.v1:<exchange>:<symbol>). Symbol switches are full page
    // navigations, so one init at boot is the whole lifecycle.
    void init(const std::string& exchange, const std::string& symbol);

    // ── Armed tool (shared by rail / topbar dropdown / DrawingLayer) ─────────
    drawing::Tool armed() const { return armed_; }
    void arm(drawing::Tool t) { armed_ = t; }

    // ── Store ────────────────────────────────────────────────────────────────
    std::vector<drawing::Drawing>&       items()       { return items_; }
    const std::vector<drawing::Drawing>& items() const { return items_; }
    drawing::Drawing* find(uint64_t id);

    // Assigns an id when d.id == 0. Returns the id, or 0 when the store is at
    // capacity (kMaxDrawings). Pushes a Create undo op.
    uint64_t add(drawing::Drawing d);
    bool     remove(uint64_t id);      // pushes a Delete undo op
    void     clear_all();              // remove-all (bulk, single undo not kept)

    // Drag/edit lifecycle: snapshot at drag start, undo op at drag end (only
    // when something actually changed).
    void begin_modify(uint64_t id);
    void end_modify(uint64_t id);
    void mark_dirty();                 // any mutation → schedule a persist

    void undo();                       // Ctrl+Z — bounded stack (64)
    bool can_undo() const { return !undo_stack_.empty(); }

    // ── Selection (Cursor mode) ──────────────────────────────────────────────
    uint64_t selected() const { return selected_; }
    void select(uint64_t id) { selected_ = id; }

    // ── Global toggles ───────────────────────────────────────────────────────
    bool magnet() const { return magnet_; }
    void set_magnet(bool v) { magnet_ = v; mark_dirty(); }
    bool hidden_all() const { return hidden_all_; }
    void set_hidden_all(bool v) { hidden_all_ = v; mark_dirty(); }
    bool rail_collapsed() const { return rail_collapsed_; }
    void set_rail_collapsed(bool v) { rail_collapsed_ = v; mark_dirty(); }

    // Style applied to newly placed drawings (mutated by the style editor).
    drawing::Style& default_style() { return default_style_; }

    // ── Esc arbitration ──────────────────────────────────────────────────────
    // The layer stamps the frame when Esc cancels a placement/selection so
    // main.cpp can skip the replay Esc handler the same frame (widgets render
    // before replay shortcut processing).
    void mark_escape_consumed(int frame) { esc_frame_ = frame; }
    bool escape_consumed(int frame) const { return esc_frame_ == frame; }

    // ── Persistence (debounced) ──────────────────────────────────────────────
    // Called once per frame from main.cpp; flushes ~1s after the last mutation.
    void tick();
    void flush();                      // immediate write (pagehide export)

private:
    void load_json(const char* raw);       // parse + validate the stored blob
    std::string serialize() const;         // JSON v1 (see plan schema)

    struct UndoOp {
        enum class Kind : uint8_t { Create, Delete, Modify };
        Kind             kind;
        uint64_t         id;
        drawing::Drawing before;       // Delete/Modify: state to restore
    };
    void push_undo(UndoOp op);

    std::vector<drawing::Drawing> items_;
    std::deque<UndoOp>            undo_stack_;
    drawing::Drawing              modify_snapshot_{};   // begin_modify capture
    uint64_t                      modify_id_ = 0;

    drawing::Tool  armed_        = drawing::Tool::Cursor;
    uint64_t       selected_     = 0;
    drawing::Style default_style_{};
    bool           magnet_         = true;
    bool           hidden_all_     = false;
    bool           rail_collapsed_ = false;

    uint64_t next_id_ = 0;
    int      esc_frame_ = -1;

    // Persistence (Phase 5 wires the localStorage bridge; the debounce plumbing
    // is live from day one so mutations are already routed through mark_dirty).
    std::string storage_key_;
    bool        dirty_ = false;
    std::chrono::steady_clock::time_point dirty_at_{};
    bool        loaded_ = false;
};
