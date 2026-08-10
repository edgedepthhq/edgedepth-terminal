#pragma once

#include "core/app_context.h"
#include "ui/widget.h"

#include <cstdint>
#include <string>
#include <vector>

// Public, manifest-driven .edpack catalog. Packs are static objects and replay
// entirely through PackReplayEngine, so this widget needs no account, backend
// replay session, or additional data path.
class ReplayLibraryWidget final : public Widget {
public:
    explicit ReplayLibraryWidget(const AppContext& ctx);
    ~ReplayLibraryWidget() override = default;

    void render() override;
    void update() override;

    [[nodiscard]] WidgetType type() const override {
        return WidgetType::ReplayLibrary;
    }
    [[nodiscard]] const char* title() const override {
        return "Replay Library###replay_library";
    }
    [[nodiscard]] UpdateFrequency update_frequency() const override {
        return UpdateFrequency::Slow;
    }

private:
    struct Pack {
        std::string id;
        std::string title;
        std::string summary;
        std::string exchange;
        std::string symbol;
        std::string display_symbol;
        std::string captured_at;
        std::string pack_url;
        std::string metadata_line;
        std::string tags_line;
        int64_t duration_seconds = 0;
        int64_t size_bytes = 0;
        bool featured = false;
    };

    const AppContext& ctx_;
    std::vector<Pack> packs_;
    std::string manifest_url_;
    std::string error_;
    std::string notice_;
    double notice_until_ = 0.0;
    int manifest_revision_ = -1;
    int pending_pack_ = -1;
    bool loading_ = false;
    bool catalog_ready_ = false;
    bool replace_popup_requested_ = false;

    void begin_fetch(bool force);
    bool parse_manifest(const std::string& payload, std::string& parse_error);
    void play_pack(const Pack& pack);
    void render_pack(const Pack& pack, int index);
    void render_replace_confirmation();

    static bool is_http_url(const std::string& value);
    static std::string format_duration(int64_t seconds);
    static std::string format_size(int64_t bytes);
};
