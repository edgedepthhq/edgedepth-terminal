// ═══════════════════════════════════════════════════════════════════════════════
// upsell_modal.cpp - see upsell_modal.h. The single free→Pro conversion surface.
// ═══════════════════════════════════════════════════════════════════════════════

#include "upsell_modal.h"
#include <algorithm>

#include "imgui.h"
#include <cstdio>
#include <nlohmann/json.hpp>
#include "../rendering/theme.h"
#include "../core/entitlements.h"
#include "../core/usage_emit.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace ui {

namespace {

const char* trigger_name(UpsellModal::Trigger t) {
    using T = UpsellModal::Trigger;
    switch (t) {
        case T::Range:      return "range";
        case T::Preset:     return "preset";
        case T::Speed:      return "speed";
        case T::Symbol:     return "symbol";
        case T::Layer:      return "layer";
        case T::ServerTier: return "server_tier";
        case T::Events:     return "events";
        case T::Lesson:     return "lesson";
        case T::Daily:      return "daily";
        case T::Auth:       return "auth";
        case T::Research:   return "research";
        default:            return "generic";
    }
}

// `layer` is passed explicitly at every call site (not defaulted) so a new event
// cannot quietly ship without deciding whether it carries the slug.
void emit_upsell_usage(const char* event, UpsellModal::Trigger trigger, bool login,
                       const std::string& layer,
                       const nlohmann::json& props = nlohmann::json::object()) {
    nlohmann::json merged = props;
    merged["source"] = "terminal";
    merged["trigger"] = trigger_name(trigger);
    merged["variant"] = login ? "login" : "pro";
    // ABSENT rather than empty: `props->>'layer' is null` then means "this row
    // predates the slug or its gate has no layer", which an empty string could
    // not say. Never emit "" here.
    if (!layer.empty()) merged["layer"] = layer;
    usage::dispatch_detail(nlohmann::json{
        {"event", event},
        {"mode", "terminal"},
        {"props", std::move(merged)},
    }.dump());
}

}  // namespace

UpsellModal& UpsellModal::instance() {
    static UpsellModal inst;
    return inst;
}

// Default contextual subline for a gate (overridden by open()'s `detail`).
static const char* default_subline(UpsellModal::Trigger t, bool login) {
    using T = UpsellModal::Trigger;
    if (login) return "Replay a full Free day from any account, then review Pro and Research when you need deeper history.";
    switch (t) {
        case T::Range:
        case T::Preset:     return "Free replays one archived day (shown below). Pro adds a rolling replay window; Research reaches older available recordings.";
        case T::Speed:      return "Free replay plays up to 2\xc3\x97. Pro plays up to 4\xc3\x97.";
        case T::Symbol:     return "Free replay covers 6 majors. Pro replays every recorded pair.";
        case T::Layer:      return "Basic modeled heatmaps are free. Pro adds Hyperliquid position levels, reported liquidations and leverage filters.";
        case T::Events:     return "This archived event is outside the free recent window.";
        case T::Lesson:     return "This lesson is available to Pro subscribers.";
        case T::Daily:      return "You've used all 6 free replays for today - they reset at 00:00 UTC.";
        case T::ServerTier: return "That replay is outside your free window.";
        case T::Research:   return "Reading the record at a past minute is a Pro feature. The live read (this minute) stays free.";
        default:            return "A Free replay day is included. Pro adds live RT, sub-minute candles and deeper replay.";
    }
}

static const char* modal_headline(UpsellModal::Trigger t, bool login) {
    if (login) return "Log in to replay";
    if (t == UpsellModal::Trigger::Research) return "Investigate past moments with Pro";
    if (t == UpsellModal::Trigger::Range) return "Replay this exact moment";
    return "Unlock advanced terminal tools";
}

void UpsellModal::open(Trigger t, const char* detail, const char* layer) {
    trigger_ = t;
    login_variant_ = (t == Trigger::Auth);
    yearly_billing_ = true;
    detail_ = detail ? detail : "";
    // Assigned unconditionally, like detail_: this is a singleton, so a gate that
    // passes no layer must CLEAR the previous one or a Speed gate would inherit
    // the slug of the last layer pill that fired.
    layer_ = layer ? layer : "";
    dismiss_redirect_.clear();  // never inherit an event/lesson-boot redirect
    want_open_ = true;
    emit_upsell_usage("locked_action", trigger_, login_variant_, layer_);
}

void UpsellModal::open_login(const char* detail) {
    trigger_ = Trigger::Auth;
    login_variant_ = true;
    yearly_billing_ = true;
    detail_ = detail ? detail : "";
    layer_.clear();             // an auth gate is not a layer gate
    dismiss_redirect_.clear();  // never inherit an event/lesson-boot redirect
    want_open_ = true;
    emit_upsell_usage("locked_action", trigger_, login_variant_, layer_);
}

void UpsellModal::set_dismiss_redirect(const char* symbol) {
    // Built once here, outside the render loop; dismiss() only reads it.
    dismiss_redirect_ = (symbol && *symbol)
        ? std::string("/terminal/") + symbol
        : std::string("/terminal");
}

// Shared dismiss for "Maybe later"/"Not now" and Escape - NOT the primary CTA.
// With a redirect armed (event/lesson boot denial) this leaves the dead embedded
// chrome for the live terminal via a FULL navigation (the embedded page can't be
// repurposed in place); otherwise it just closes.
bool UpsellModal::blocks_replay_shortcuts() const {
    return open_ || want_open_ || dismissed_frame_ == ImGui::GetFrameCount();
}

void UpsellModal::dismiss() {
    dismissed_frame_ = ImGui::GetFrameCount();
#ifdef __EMSCRIPTEN__
    if (!dismiss_redirect_.empty()) {
        EM_ASM({ window.location.assign(UTF8ToString($0)); }, dismiss_redirect_.c_str());
    }
#endif
    ImGui::CloseCurrentPopup();
    open_ = false;
}

void UpsellModal::toast(const char* msg) {
    toast_text_   = msg ? msg : "";
    toast_active_ = true;
    toast_until_  = ImGui::GetTime() + 3.4;
}

// ── Per-frame entry point ────────────────────────────────────────────────────
void UpsellModal::render() {
    using namespace Theme;

    if (want_open_) {
        want_open_ = false;
        // Every explicit locked action opens the same reviewable plan dialog.
        toast_active_ = false;
        ImGui::OpenPopup("##edx_upsell");
        open_ = true;
        emit_upsell_usage("upsell_impression", trigger_, login_variant_, layer_,
                          {{"surface", "modal"}});
    }

    render_toast();

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    const ImVec2 viewport_size = ImGui::GetMainViewport()->Size;
    const float modal_width = std::min(560.0f, viewport_size.x - 32.0f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(modal_width, 0.0f),
                                       ImVec2(modal_width, viewport_size.y - 32.0f));
    ImGui::SetNextWindowSize(ImVec2(modal_width, 0.0f), ImGuiCond_FirstUseEver);

    ImGui::PushStyleColor(ImGuiCol_PopupBg, Tokens::PANEL);
    ImGui::PushStyleColor(ImGuiCol_Border, Tokens::BD2);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.55f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f, 20.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Radius::R3);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);

    if (ImGui::BeginPopupModal("##edx_upsell", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize)) {
        render_modal_body();
        ImGui::EndPopup();
    } else {
        open_ = false;
    }

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
}

// ── Non-blocking session notice (bottom-center, above the transport) ───────────
void UpsellModal::render_toast() {
    if (!toast_active_) return;
    const double now = ImGui::GetTime();
    if (now > toast_until_) { toast_active_ = false; return; }

    using namespace Theme;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::PushFont(Fonts::ui_semibold());
    const ImVec2 ts = ImGui::CalcTextSize(toast_text_.c_str());
    const float padx = 14.0f, pady = 9.0f;
    const float w = ts.x + padx * 2.0f, h = ts.y + pady * 2.0f;
    const float x = vp->Pos.x + (vp->Size.x - w) * 0.5f;
    const float y = vp->Pos.y + vp->Size.y - h - 88.0f;
    float a = 1.0f;
    const double remaining = toast_until_ - now;
    if (remaining < 0.4) a = static_cast<float>(remaining / 0.4);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), u32(Tokens::ELEV, a), Radius::R2);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), u32(Tokens::BRAND_LINE, a), Radius::R2, 0, 1.0f);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x + padx, y + pady),
                u32(Tokens::TX1, a), toast_text_.c_str());
    ImGui::PopFont();
}

// ── Modal body ───────────────────────────────────────────────────────────────
void UpsellModal::render_modal_body() {
    using namespace Theme;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::PushFont(Fonts::label());
    ImGui::TextColored(Tokens::TX2, "%s", login_variant_ ? "YOUR FREE ACCOUNT" : "EDGEDEPTH PRO");
    ImGui::PopFont();
    ImGui::SameLine(width - 2.0f);
    if (ImGui::SmallButton("X##close_upgrade")) { dismiss(); return; }
    if (ImGui::IsItemHovered()) Theme::tooltip("Close");
    ImGui::Dummy(ImVec2(0, 10));

    const char* headline = modal_headline(trigger_, login_variant_);
    const char* context = default_subline(trigger_, login_variant_);
    if (!login_variant_ && trigger_ == Trigger::Layer) {
        if (layer_ == "hl_liq_levels" || layer_ == "hl_history") {
            headline = "See where positions are exposed.";
            context = "Add Hyperliquid liquidation levels and explore how the recorded position sample changed.";
        } else if (layer_ == "liq_observed") {
            headline = "See the liquidations that printed.";
            context = "Put reported liquidation events alongside price and order flow.";
        } else {
            headline = "Read the market in more detail.";
        }
    }
    ImGui::PushFont(Fonts::heading());
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(Tokens::TX1, "%s", headline);
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
    ImGui::TextWrapped("%s", detail_.empty() ? context : detail_.c_str());
    ImGui::PopStyleColor();

    if (trigger_ == Trigger::Range || trigger_ == Trigger::Preset || trigger_ == Trigger::Auth) {
        ImGui::TextWrapped("Free replay window (UTC): %s", Entitlements::free_window_label().c_str());
    }
    ImGui::Dummy(ImVec2(0, 12));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 10));

    // A short outcome and its concrete tools, rather than a feature inventory.
    auto benefit = [&](const char* title, const char* description) {
        ImGui::PushFont(Fonts::ui_semibold());
        ImGui::TextWrapped("%s", title);
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
        ImGui::TextWrapped("%s", description);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 9));
    };
    benefit("Read the live pressure", "Real-time depth, trade bubbles and sub-minute candles.");
    benefit("Add liquidation context", "Hyperliquid sampled-position levels, available history and reported events.");
    char replay[128];
    snprintf(replay, sizeof(replay), "%d-day tick replay. Every recorded pair. Up to 4\xc3\x97 speed.", Entitlements::pro_lookback_days());
    benefit("Go back and study the move", replay);

    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 10));
    if (!login_variant_) {
        const float choice_w = (ImGui::GetContentRegionAvail().x - 8) * 0.5f;
        auto billing_option = [&](const char* label, bool yearly) {
            const bool selected = yearly_billing_ == yearly;
            if (Theme::choice_button(label, selected, ImVec2(choice_w, 32))) yearly_billing_ = yearly;
        };
        billing_option("Annual · save $108", true);
        ImGui::SameLine(0, 8);
        billing_option("Monthly", false);
        ImGui::Dummy(ImVec2(0, 7));
        ImGui::PushFont(Fonts::heading());
        ImGui::TextUnformatted(yearly_billing_ ? "$20 / month" : "$29 / month");
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
        ImGui::TextWrapped("%s", yearly_billing_
            ? "$240 billed yearly on card. Founder rate locks while subscribed."
            : "$29 billed monthly on card. Founder rate locks while subscribed.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
        ImGui::TextWrapped("Need deeper research? Research includes Pro, more credits and deeper available replay history.");
        ImGui::PopStyleColor();
    } else {
        ImGui::TextWrapped("Free includes a daily replay window, research credits and supported setup monitoring on the web.");
    }

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 10));
    const float dismiss_w = std::min(136.0f, width * 0.35f);
    ImGui::PushStyleColor(ImGuiCol_Button, Tokens::PANEL);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Tokens::HOVER);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Tokens::ACTIVE);
    if (ImGui::Button(login_variant_ ? "Not now" : "Maybe later", ImVec2(dismiss_w, 40))) dismiss();
    ImGui::PopStyleColor(3);
    ImGui::SameLine(0, 8);
    ImGui::PushFont(Fonts::ui_semibold());
    if (Theme::choice_button(login_variant_ ? "Log in" : "Explore Pro plans", true, ImVec2(width - dismiss_w - 8, 40))) {
        if (login_variant_) {
            emit_upsell_usage("login_click", trigger_, true, layer_);
            Entitlements::open_login();
        } else {
            const char* billing = yearly_billing_ ? "yearly" : "monthly";
            emit_upsell_usage("upgrade_click", trigger_, false, layer_,
                              {{"plan", "unselected"}, {"billing", billing}, {"rail", "pricing"}});
            Entitlements::open_pricing(billing);
        }
        ImGui::CloseCurrentPopup();
        open_ = false;
    }
    ImGui::PopFont();

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) dismiss();
}

}  // namespace ui
