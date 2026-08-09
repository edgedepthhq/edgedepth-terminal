#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// logo_manager.h — lazy async loader for coin + exchange logo images.
//
// Images are PNGs hosted on R2 (see logo_base_url()), decoded in the BROWSER via
// an <img> element and uploaded straight into a GL texture with
// texImage2D(HTMLImageElement) — the exact path recorder_glue uses for the cam
// bubble. No C++ PNG decoder, no stb_image.
//
//   coin("BTC")       -> https://<base>/logos/coins/btc.png     (LRU-capped)
//   exchange("hl")    -> https://<base>/logos/exchanges/hyperliquid.png (permanent)
//
// Coin textures are LRU-capped so 800+ symbols never all sit in GL memory;
// exchange logos are a tiny fixed set kept permanently. Keys that are still
// loading or have failed draw a deterministic MONOGRAM (initials in a hashed-hue
// rounded square) so a row is never blank.
//
// Threading: every method touches GL / browser DOM and MUST run on the main
// thread. draw_*() are safe to call every frame from render code; tick() drains
// pending decodes + runs eviction and must be called once per frame.
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>
#include <unordered_map>

#include "imgui.h"

class LogoManager {
public:
    static LogoManager& instance();

    // Draw a rounded coin logo into [p, p+{size,size}] if decoded, else a
    // monogram fallback. Marks the key most-recently-used. base_asset is
    // upper-cased for the monogram and lower-cased for the URL internally.
    void draw_coin(ImDrawList* dl, const std::string& base_asset, ImVec2 p, float size);

    // Draw an exchange logo ("binancef" | "hl") into [p, p+{size,size}] if
    // decoded, else nothing (callers keep the text label regardless).
    void draw_exchange(ImDrawList* dl, const std::string& exchange_id, ImVec2 p, float size);

    // Ready texture (non-zero) for a coin, or 0 while loading / failed. Kicks off
    // a load on first request. Prefer draw_coin() unless you need the raw handle.
    ImTextureID coin(const std::string& base_asset);
    ImTextureID exchange(const std::string& exchange_id);

    // Per-frame, main thread: upload freshly-decoded images to GL and evict the
    // LRU tail of the coin cache. Cheap when nothing is pending.
    void tick();

private:
    LogoManager() = default;

    enum class State : uint8_t { Loading, Ready, Failed };

    struct Entry {
        std::string  url;
        ImTextureID  tex        = 0;       // 0 until uploaded
        State        state      = State::Loading;
        uint32_t     last_used  = 0;       // frame index (coins only; LRU)
        bool         permanent  = false;   // exchange logos: never evicted
    };

    Entry& ensure(const std::string& key, const std::string& url, bool permanent);
    void   evict_lru();

    std::unordered_map<std::string, Entry> entries_;  // key = "coin:BTC" / "ex:hl"
    uint32_t frame_   = 0;

    static constexpr size_t kCoinCap = 256;  // ~4 MB of 64px RGBA; ~30 ever visible
};

// Monogram fallback, also used directly by web-parity call sites: a hashed-hue
// rounded square with the first 1-3 chars of base_asset centered. Public so
// widgets can draw it without going through LogoManager (e.g. explicit placeholder).
void draw_logo_monogram(ImDrawList* dl, const std::string& base_asset, ImVec2 p, float size);
