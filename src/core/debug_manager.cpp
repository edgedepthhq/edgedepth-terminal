#include "debug_manager.h"

// ── proto includes ─────────────────────────────────────────────────────────
// Requires debug.pb.h compiled from debug.proto (see bottom of this file).
// Add to messages.proto:   STREAM_DEBUG = 23;
// Add to CMakeLists:       debug.proto → pb/debug.pb.cc + pb/debug.pb.h
#include "pb/messages.pb.h"
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <cstring>
#include <cstdio>

using json = nlohmann::json;

// ── helpers ────────────────────────────────────────────────────────────────
static void safe_strcpy(char* dst, size_t dst_sz, const std::string& src) {
    size_t n = std::min(src.size(), dst_sz - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

void DebugManager::fmt_tf(int64_t tf_ms, char* out, size_t out_sz) {
    if (tf_ms <= 0) { std::snprintf(out, out_sz, "-"); return; }
    int64_t s = tf_ms / 1000;
    if      (s % 86400 == 0) std::snprintf(out, out_sz, "%lldd", (long long)(s/86400));
    else if (s % 3600  == 0) std::snprintf(out, out_sz, "%lldh", (long long)(s/3600));
    else if (s % 60    == 0) std::snprintf(out, out_sz, "%lldm", (long long)(s/60));
    else                     std::snprintf(out, out_sz, "%llds", (long long)s);
}

// ── DebugManager ──────────────────────────────────────────────────────────
DebugManager::DebugManager(StreamManager& stream_mgr, const Terminal::Pair& pair)
    : stream_mgr_(stream_mgr)
    , pair_(pair)
{}

void DebugManager::subscribe(const std::string& symbol) {
    if (subscribed_symbol_ == symbol) return;
    unsubscribe();
    subscribed_symbol_ = symbol;

    entry_count_ = 0;
    write_head_  = 0;
    new_entries_since_render_ = false;

    const json msg = {
        {"method", "subscribe_debug"},
        {"data", {
                {"pair", {
                    {"exchange", pair_.exchange},
                    {"symbol",   pair_.symbol}
                }}
        }}
    };
    stream_mgr_.send_message(msg.dump());
}

void DebugManager::unsubscribe() {
    if (subscribed_symbol_.empty()) return;
    stream_mgr_.send_message(R"({"method":"unsubscribe_debug"})");
    subscribed_symbol_.clear();
}

void DebugManager::push_entry(const DebugEntry& e) {
    ring_[write_head_ % MAX_ENTRIES] = e;
    ++write_head_;
    if (entry_count_ < MAX_ENTRIES) ++entry_count_;
    new_entries_since_render_ = true;
}

const DebugEntry& DebugManager::entry_at(size_t i) const {
    // i=0 is oldest surviving entry
    size_t oldest = (entry_count_ < MAX_ENTRIES) ? 0 : write_head_ % MAX_ENTRIES;
    return ring_[(oldest + i) % MAX_ENTRIES];
}

void DebugManager::handle_batch(const void* data, size_t size) {
    pb::DebugLogBatch batch;
    if (!batch.ParseFromArray(data, static_cast<int>(size))) return;
    // If we're in range-loading mode and this is backfill, append to range buffer
    if (range_loading_ && batch.is_backfill() && range_active_) {
        for (const auto& pb : batch.entries()) {
            DebugEntry e{};
            e.ts     = pb.ts();
            e.score  = static_cast<float>(pb.score());
            e.thresh = static_cast<float>(pb.thresh());
            e.price  = pb.price();
            safe_strcpy(e.actor,    sizeof(e.actor),    pb.actor());
            safe_strcpy(e.category, sizeof(e.category), pb.cat());
            safe_strcpy(e.detector, sizeof(e.detector), pb.det());
            std::snprintf(e.window, sizeof(e.window), "%d", pb.win());
            safe_strcpy(e.msg,      sizeof(e.msg),      pb.msg());
            fmt_tf(pb.tf(), e.tf_label, sizeof(e.tf_label));
            range_entries_.push_back(e);
        }
        new_entries_since_render_ = true;
        return;
    }
    // Normal live path — existing logic unchanged
    if (batch.is_backfill()) {
        entry_count_ = 0;
        write_head_  = 0;
    }
    for (const auto& pb : batch.entries()) {
        DebugEntry e{};
        e.ts     = pb.ts();
        e.score  = static_cast<float>(pb.score());
        e.thresh = static_cast<float>(pb.thresh());
        e.price  = pb.price();
        safe_strcpy(e.actor,    sizeof(e.actor),    pb.actor());
        safe_strcpy(e.category, sizeof(e.category), pb.cat());
        safe_strcpy(e.detector, sizeof(e.detector), pb.det());
        std::snprintf(e.window, sizeof(e.window), "%d", pb.win());
        safe_strcpy(e.msg,      sizeof(e.msg),      pb.msg());
        fmt_tf(pb.tf(), e.tf_label, sizeof(e.tf_label));
        push_entry(e);
    }
}

void DebugManager::load_range(int64_t start_ms, int64_t end_ms) {
    // Clear previous range data
    range_entries_.clear();
    range_entries_.reserve(4096);
    range_active_  = true;
    range_loading_ = true;
    range_start_ms_ = start_ms;
    range_end_ms_   = end_ms;

    const json msg = {
        {"method", "load_debug_range"},
        {"data", {
                {"pair", {
                    {"exchange", pair_.exchange},
                    {"symbol",   pair_.symbol}
                }},
                {"start_time", start_ms},
                {"end_time",   end_ms}
        }}
    };
    stream_mgr_.send_message(msg.dump());
}

void DebugManager::handle_range_start(int64_t start_ms, int64_t end_ms) {
    // Server confirmed it's sending — clear any stale data
    range_entries_.clear();
    range_loading_ = true;
}

void DebugManager::handle_range_complete(int64_t start_ms, int64_t end_ms,
                                          int msg_count, int entry_count) {
    range_loading_ = false;
}

void DebugManager::clear_range() {
    range_active_  = false;
    range_loading_ = false;
    range_entries_.clear();
    range_start_ms_ = 0;
    range_end_ms_   = 0;
}

void DebugManager::copy_all_to_clipboard() const {
    if (entry_count_ == 0) return;

    std::string buf;
    buf.reserve(entry_count_ * 200);
    buf += "Timestamp\tActor\tCategory\tTimeframe\tDetector\tWindow\tScore\tThreshold\tPrice\tMessage\n";

    for (size_t i = 0; i < count(); ++i) {
        const auto& e = entry_at(i);
        char line[512];
        std::snprintf(line, sizeof(line),
            "%lld\t%s\t%s\t%s\t%s\t%s\t%.4f\t%.4f\t%.8f\t%s\n",
            (long long)e.ts,
            e.actor, e.category, e.tf_label,
            e.detector, e.window,
            e.score, e.thresh, e.price,
            e.msg);
        buf += line;
    }

    // Write directly to browser clipboard API — works in WASM
    EM_ASM({
        var text = UTF8ToString($0);
        if (navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(text);
        }
    }, buf.c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
// debug.proto  (add alongside messages.proto, compile with protoc)
// ─────────────────────────────────────────────────────────────────────────────
//
//  syntax = "proto3";
//  option optimize_for = LITE_RUNTIME;
//  package debug;
//
//  message DebugEntry {
//      int64  ts     = 1;
//      string actor  = 2;
//      string cat    = 3;
//      int64  tf     = 4;
//      string det    = 5;
//      string win    = 6;
//      double score  = 7;
//      double thresh = 8;
//      double price  = 9;
//      string msg    = 10;
//  }
//
//  message DebugLogBatch {
//      string symbol      = 1;
//      bool   is_backfill = 2;
//      int32  ring_total  = 3;
//      repeated DebugEntry entries = 4;
//  }
//
// Also add to messages.proto enum Stream:
//      STREAM_DEBUG = 23;