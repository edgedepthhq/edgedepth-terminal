#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// upsell_modal.h - the ONE upsell modal every free-tier gate funnels into (§8.3).
//
// Locked range / preset / speed / symbol / layer pill, a TIER_* server error, the
// /events padlock, and a lesson lock ALL call UpsellModal::open(...) - a single,
// consistent conversion surface instead of scattered ad-hoc nudges. Primary CTA is
// "Go Pro - pay with Bitcoin" → /pricing (Entitlements::open_upgrade). AUTH_REQUIRED
// / GRANT_REQUIRED route to a login variant instead (Entitlements::open_login).
//
// Shown at most once per surface per session; a repeat gate shows a slim toast so
// the funnel never nags (§8.3). render() is called once per frame from the main
// loop; open(...) can be called from anywhere (it just latches).
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>

namespace ui {

class UpsellModal {
public:
    // Which gate opened it - drives the contextual subline + the once-per-surface
    // de-dupe. Auth is the login variant (not logged in / no grant).
    enum class Trigger : uint8_t {
        Generic = 0, Range, Preset, Speed, Symbol, Layer,
        ServerTier, Events, Lesson, Daily, Auth, Research, _Count
    };

    static UpsellModal& instance();

    // Open the upsell for a gate. `detail` overrides the default subline if non-null.
    void open(Trigger t, const char* detail = nullptr);
    // Login variant (AUTH_REQUIRED / GRANT_REQUIRED) - CTA goes to /login.
    void open_login(const char* detail = nullptr);

    // Embedded event/lesson boot escape hatch: when the denied replay IS the whole
    // page (?event= / ?lesson= chrome), a plain dismiss strands the user in event
    // chrome over plain live data. Call AFTER open()/open_login() to make BOTH
    // dismiss paths ("Maybe later"/"Not now" and Escape) do a full navigation to
    // the live terminal - window.location.assign("/terminal/<symbol>") - instead
    // of just closing. The primary CTA (Go Pro / Log in) is unaffected.
    // open()/open_login() CLEAR any armed redirect, so the live-terminal gates
    // (speed/symbol/layer/range pills, launcher) can never inherit a stale one.
    void set_dismiss_redirect(const char* symbol);

    // Show a slim, non-blocking notice toast (neutral - e.g. an authenticated user
    // hit a token/session error, which is NOT a login or an upsell situation).
    void toast(const char* msg);

    // Call ONCE per frame from the main render loop (renders the modal + any toast).
    void render();

    bool is_open() const { return open_; }

private:
    UpsellModal() = default;
    void render_modal_body();
    void render_toast();
    void dismiss();  // shared "Maybe later"/Escape path (runs the redirect, if armed)

    Trigger     trigger_ = Trigger::Generic;
    std::string detail_;
    std::string dismiss_redirect_;   // full path ("/terminal/<sym>"); empty = just close
    bool        login_variant_ = false;

    bool     want_open_ = false;      // latched by open(), consumed by render()
    bool     open_      = false;      // popup currently on screen
    uint32_t full_shown_mask_ = 0;    // bit per Trigger already shown as a full modal

    bool        toast_active_ = false;
    double      toast_until_  = 0.0;  // ImGui::GetTime() deadline
    std::string toast_text_;
};

}  // namespace ui
