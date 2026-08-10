// ═══════════════════════════════════════════════════════════════════════════════
// upsell_modal.cpp - see upsell_modal.h. The single free→Pro conversion surface.
// ═══════════════════════════════════════════════════════════════════════════════

#include "upsell_modal.h"

#include "imgui.h"
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

void emit_upsell_usage(const char* event, UpsellModal::Trigger trigger, bool login,
                       const nlohmann::json& props = nlohmann::json::object()) {
    nlohmann::json merged = props;
    merged["source"] = "terminal";
    merged["trigger"] = trigger_name(trigger);
    merged["variant"] = login ? "login" : "pro";
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
    if (login) return "Replay a full free day from any account - or go Pro for the full 30-day archive, up to the live edge.";
    switch (t) {
        case T::Range:
        case T::Preset:     return "Free replays one archived day (shown below). Anything newer or older is Pro.";
        case T::Speed:      return "Free replay plays up to 2\xc3\x97. Pro plays up to 4\xc3\x97.";
        case T::Symbol:     return "Free replay covers 6 majors. Pro replays all 550+ symbols.";
        case T::Layer:      return "The Liquidation Field is free. Levels, Observed and per-tier LIQ-LEV isolation are Pro layers.";
        case T::Events:     return "This archived event is outside the free recent window.";
        case T::Lesson:     return "This lesson is available to Pro subscribers.";
        case T::Daily:      return "You've used all 6 free replays for today - they reset at 00:00 UTC.";
        case T::ServerTier: return "That replay is outside your free window.";
        case T::Research:   return "Reading the record at a past minute is a Pro feature. The live read (this minute) stays free.";
        default:            return "A free replay day is included daily. Pro unlocks the full 30-day archive, all symbols, and every layer.";
    }
}

static const char* modal_headline(UpsellModal::Trigger t, bool login) {
    if (login) return "Log in to replay";
    if (t == UpsellModal::Trigger::Research) return "Investigate past moments with Pro";
    return "Replay any moment of the last 30 days";
}

void UpsellModal::open(Trigger t, const char* detail) {
    trigger_ = t;
    login_variant_ = (t == Trigger::Auth);
    detail_ = detail ? detail : "";
    dismiss_redirect_.clear();  // never inherit an event/lesson-boot redirect
    want_open_ = true;
    emit_upsell_usage("locked_action", trigger_, login_variant_);
}

void UpsellModal::open_login(const char* detail) {
    trigger_ = Trigger::Auth;
    login_variant_ = true;
    detail_ = detail ? detail : "";
    dismiss_redirect_.clear();  // never inherit an event/lesson-boot redirect
    want_open_ = true;
    emit_upsell_usage("locked_action", trigger_, login_variant_);
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
void UpsellModal::dismiss() {
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
        const uint32_t bit = 1u << static_cast<uint8_t>(trigger_);
        // Full modal the first time a surface fires; a slim toast on repeats so the
        // funnel never nags (login always shows the full prompt).
        const bool repeat = !login_variant_ && (full_shown_mask_ & bit) != 0;
        if (repeat) {
            toast_text_   = "Pro unlocks this - see pricing";
            toast_active_ = true;
            toast_until_  = ImGui::GetTime() + 3.2;
            emit_upsell_usage("upsell_impression", trigger_, login_variant_,
                              {{"surface", "toast"}});
        } else {
            full_shown_mask_ |= bit;
            ImGui::OpenPopup("##edx_upsell");
            open_ = true;
            emit_upsell_usage("upsell_impression", trigger_, login_variant_,
                              {{"surface", "modal"}});
        }
    }

    render_toast();

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(444.0f, 0.0f), ImGuiCond_Appearing);

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

// ── Slim repeat-trigger toast (bottom-center, above the transport) ───────────
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

    // Headline
    ImGui::PushFont(Fonts::heading());
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX1);
    ImGui::TextUnformatted(modal_headline(trigger_, login_variant_));
    ImGui::PopStyleColor();
    ImGui::PopFont();

    // Contextual subline
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(detail_.empty() ? default_subline(trigger_, login_variant_)
                                           : detail_.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    // Concrete "from … up until …" window line for the window-related surfaces.
    if (trigger_ == Trigger::Range || trigger_ == Trigger::Preset || trigger_ == Trigger::Auth) {
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::BRAND_TX);
        ImGui::Text("Free replay window (UTC): %s", Entitlements::free_window_label().c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    // Proof bullets - accent dot + primary text
    auto bullet = [&](const char* s) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float cy = p.y + ImGui::GetTextLineHeight() * 0.5f;
        dl->AddCircleFilled(ImVec2(p.x + 3.0f, cy), 2.5f, u32(Tokens::BRAND));
        ImGui::Indent(15.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX1);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(s);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Unindent(15.0f);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
    };
    bullet("30-day tick replay: all 660+ pairs, up to 4\xc3\x97");
    bullet("Complete event archive + full lesson catalog");
    bullet("Levels, Observed, LIQ-LEV tiers; footprint, TPO, scanner, alerts, paper trading");

    // 30-day timeline (1e): full bar = Pro reach (accent-soft), 3.3% amber slice
    // at the right = the one free day. Labels underneath.
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    {
        const ImVec2 bp = ImGui::GetCursorScreenPos();
        const float bw = ImGui::GetContentRegionAvail().x, bh = 8.0f;
        dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + bh), u32(Tokens::BRAND_SOFT));
        dl->AddRect(bp, ImVec2(bp.x + bw, bp.y + bh), u32(Tokens::BD2), 0.0f, 0, 1.0f);
        const float slice = bw * 0.033f;
        dl->AddRectFilled(ImVec2(bp.x + bw - slice, bp.y), ImVec2(bp.x + bw, bp.y + bh),
                          u32(Tokens::WARN));
        ImGui::Dummy(ImVec2(bw, bh + 5.0f));
        ImGui::PushFont(Fonts::label());
        const ImVec2 lp = ImGui::GetCursorScreenPos();
        const ImU32 lc = u32(Tokens::TX3);
        dl->AddText(lp, lc, "-30 DAYS");
        const char* mid = "PRO: THE FULL BAR \xC2\xB7 FREE: THE AMBER DAY";
        const float mw = ImGui::CalcTextSize(mid).x;
        dl->AddText(ImVec2(bp.x + (bw - mw) * 0.5f, lp.y), lc, mid);
        const float nw = ImGui::CalcTextSize("NOW").x;
        dl->AddText(ImVec2(bp.x + bw - nw, lp.y), lc, "NOW");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(bw, ImGui::GetTextLineHeight()));
    }

    // Price (effective monthly first, with the annual charge explicit)
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::PushFont(Fonts::mono_lg());
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX1);
    ImGui::TextUnformatted("$20/mo");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
    ImGui::TextUnformatted("billed yearly at $240  \xc2\xb7  or $29/mo billed monthly");
    ImGui::PopStyleColor();

    // ── CTAs ─────────────────────────────────────────────────────────────────
    // Upgrade: dual rails (1e) - solid "pay by card" + outline "pay with Bitcoin".
    // Login: a single solid "Log in". Both rails route through the web checkout
    // gateway (open_upgrade_rail); card vs BTC is resolved there.
    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    const float w = ImGui::GetContentRegionAvail().x;

    if (login_variant_) {
        ImGui::PushStyleColor(ImGuiCol_Button, Tokens::BRAND);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Tokens::BRAND_TX);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Tokens::BRAND);
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::BRAND_INK);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::R2);
        ImGui::PushFont(Fonts::ui_semibold());
        if (ImGui::Button("Log in", ImVec2(w, 38.0f))) {
            emit_upsell_usage("login_click", trigger_, true);
            Entitlements::open_login();
            ImGui::CloseCurrentPopup();
            open_ = false;
        }
        ImGui::PopFont();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
    } else {
        // Primary solid - pay by card.
        ImGui::PushStyleColor(ImGuiCol_Button, Tokens::BRAND);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Tokens::BRAND_TX);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Tokens::BRAND);
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::BRAND_INK);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::R2);
        ImGui::PushFont(Fonts::ui_semibold());
        if (ImGui::Button("Go Pro: pay by card", ImVec2(w, 38.0f))) {
            emit_upsell_usage("upgrade_click", trigger_, false,
                              {{"plan", "pro"}, {"billing", "yearly"}, {"rail", "card"}});
            Entitlements::open_upgrade_rail("card");
            ImGui::CloseCurrentPopup();
            open_ = false;
        }
        ImGui::PopFont();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);

        // Secondary outline - pay with Bitcoin (10% off list).
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Tokens::HOVER);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Tokens::ACTIVE);
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX1);
        ImGui::PushStyleColor(ImGuiCol_Border, Tokens::BD2);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::R2);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushFont(Fonts::ui_semibold());
        if (ImGui::Button("Pay with Bitcoin: $216/yr (save 10%)", ImVec2(w, 36.0f))) {
            emit_upsell_usage("upgrade_click", trigger_, false,
                              {{"plan", "pro"}, {"billing", "yearly"}, {"rail", "btc"}});
            Entitlements::open_upgrade_rail("btc");
            ImGui::CloseCurrentPopup();
            open_ = false;
        }
        ImGui::PopFont();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);

        // Rail micro-note (centered).
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::PushFont(Fonts::label());
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX3);
        {
            const char* note = "CARD AUTO-RENEWS, CANCEL ANYTIME \xC2\xB7 BITCOIN NEVER AUTO-CHARGES";
            const float nw = ImGui::CalcTextSize(note).x;
            if (nw < w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - nw) * 0.5f);
            ImGui::TextUnformatted(note);
        }
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    // Ghost dismiss.
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Tokens::HOVER);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Tokens::ACTIVE);
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
    if (ImGui::Button(login_variant_ ? "Not now" : "Maybe later", ImVec2(w, 26.0f))) {
        dismiss();
    }
    ImGui::PopStyleColor(4);

    // Footer founder line - amber, with a drawn diamond (no U+25C6 in the atlas).
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::PushFont(Fonts::label());
    if (login_variant_) {
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX3);
        ImGui::TextUnformatted("A FREE 24H REPLAY WINDOW EVERY DAY - ANY LOGGED-IN ACCOUNT");
        ImGui::PopStyleColor();
    } else {
        const ImVec2 fp = ImGui::GetCursorScreenPos();
        const float fcy = fp.y + ImGui::GetFontSize() * 0.5f, dr = 3.3f;
        const ImU32 wc = u32(Tokens::WARN);
        dl->AddQuadFilled(ImVec2(fp.x + dr, fcy - dr), ImVec2(fp.x + dr * 2.0f, fcy),
                          ImVec2(fp.x + dr, fcy + dr), ImVec2(fp.x, fcy), wc);
        dl->AddText(ImVec2(fp.x + dr * 2.0f + 6.0f, fp.y), wc,
                    "FOUNDER RATE LOCKS WHILE SUBSCRIBED");
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
    }
    ImGui::PopFont();

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) dismiss();
}

}  // namespace ui
