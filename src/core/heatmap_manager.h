#pragma once

#include "types/types.h"
#include "rendering/shader_heatmap_renderer.h"
#include "pb/messages.pb.h"
#include <map>
#include <memory>

struct HeatmapKey {
    std::string exchange;
    std::string symbol;
    std::string mode;

    bool operator<(const HeatmapKey& other) const {
        if (exchange != other.exchange) return exchange < other.exchange;
        if (symbol != other.symbol) return symbol < other.symbol;
        return mode < other.mode;
    }
};

class HeatmapManager {
public:
    HeatmapManager() = default;
    void apply_snapshot(const Terminal::Pair& pair, const pb::HeatmapSnapshot& snapshot_pb);
    void finalize_snapshot(const Terminal::Pair& pair, const pb::HeatmapSnapshot& snapshot_pb);

    // Returns the shader renderer for a symbol+mode
    ShaderHeatmapRenderer* get_reconstructor(const Terminal::Pair& pair, const std::string& mode);

    int64_t get_oldest_timestamp(const Terminal::Pair& pair, const std::string& mode);
    int64_t get_newest_timestamp(const Terminal::Pair& pair, const std::string& mode);

    bool has_data(const Terminal::Pair& pair, const std::string& mode) const;
    void clear(const Terminal::Pair& pair, const std::string& mode);
    void clear_all();

    // Mark all renderers for a symbol dirty (forces rebuild after batch load)
    void mark_dirty(const Terminal::Pair& pair);

private:
    std::map<HeatmapKey, std::unique_ptr<ShaderHeatmapRenderer>> heatmaps_;
    ShaderHeatmapRenderer& get_or_create(const HeatmapKey& key);
};