#include "heatmap_manager.h"

ShaderHeatmapRenderer& HeatmapManager::get_or_create(const HeatmapKey& key) {
    auto it = heatmaps_.find(key);
    if (it == heatmaps_.end()) {
        auto renderer = std::make_unique<ShaderHeatmapRenderer>();
        it = heatmaps_.emplace(key, std::move(renderer)).first;
    }
    return *it->second;
}

// Called for HISTORICAL snapshots - builds the grid
void HeatmapManager::apply_snapshot(const Terminal::Pair& pair, const pb::HeatmapSnapshot& snapshot_pb) {
    const HeatmapKey key{pair.exchange, pair.symbol, snapshot_pb.mode()};
    auto& reconstructor = get_or_create(key);
    // Use process_snapshot which builds/expands the grid
    // This is the method that initializes native_bucket_size_, bounds, etc.
    reconstructor.process_snapshot(snapshot_pb);
}

// Called for LIVE snapshots from server - finalizes a column (authoritative)
void HeatmapManager::finalize_snapshot(const Terminal::Pair& pair, const pb::HeatmapSnapshot& snapshot_pb) {
    const std::string mode = snapshot_pb.mode();
    auto* reconstructor = get_reconstructor(pair, mode);
    // No grid yet (live column arrived before any historical batch - e.g. a
    // pack replay whose backfill hasn't been requested/served, or a v1 pack
    // with no heatmap seed at all): BOOTSTRAP the grid from this column via
    // process_snapshot instead of dropping it. Before this, the book-depth
    // heatmap stayed permanently empty in pack mode because the historical
    // request was swallowed and every live column hit this early-return.
    if (!reconstructor || !reconstructor->has_data()) {
        apply_snapshot(pair, snapshot_pb);
        return;
    }
    // Convert protobuf to native map
    std::unordered_map<double, float> price_qty_map;
    price_qty_map.reserve(snapshot_pb.prices_size());

    for (int i = 0; i < snapshot_pb.prices_size(); ++i) {
        const double price = snapshot_pb.prices(i);
        const float total_qty = snapshot_pb.bid_qty(i) + snapshot_pb.ask_qty(i);
        if (total_qty >= 0.001f) {
            price_qty_map[price] = total_qty;
        }
    }

    // Finalize the column (authoritative server data)
    reconstructor->finalize_column(snapshot_pb.timestamp_ms(), price_qty_map);
}

ShaderHeatmapRenderer* HeatmapManager::get_reconstructor(const Terminal::Pair& pair, const std::string& mode) {
    const HeatmapKey key{pair.exchange, pair.symbol, mode};
    const auto it = heatmaps_.find(key);
    return (it != heatmaps_.end()) ? it->second.get() : nullptr;
}

int64_t HeatmapManager::get_oldest_timestamp(const Terminal::Pair& pair, const std::string& mode) {
    const auto* r = get_reconstructor(pair, mode);
    return r ? r->get_min_time() : 0;
}

int64_t HeatmapManager::get_newest_timestamp(const Terminal::Pair& pair, const std::string& mode) {
    const auto* r = get_reconstructor(pair, mode);
    return r ? r->get_max_time() : 0;
}

bool HeatmapManager::has_data(const Terminal::Pair& pair, const std::string& mode) const {
    const HeatmapKey key{pair.exchange, pair.symbol, mode};
    return heatmaps_.contains(key);
}

void HeatmapManager::clear(const Terminal::Pair& pair, const std::string& mode) {
    const HeatmapKey key{pair.exchange, pair.symbol, mode};
    auto it = heatmaps_.find(key);
    if (it != heatmaps_.end()) {
        it->second->clear();
    }
}

void HeatmapManager::clear_all() {
    heatmaps_.clear();
}

void HeatmapManager::mark_dirty(const Terminal::Pair& pair) {
    for (auto& [key, reconstructor] : heatmaps_) {
        if (key.exchange == pair.exchange && key.symbol == pair.symbol) {
            reconstructor->mark_dirty();
        }
    }
}