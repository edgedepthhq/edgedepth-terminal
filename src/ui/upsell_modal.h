#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// upsell_modal.h - the ONE upsell modal every free-tier gate funnels into (§8.3).
//
// Locked range / preset / speed / symbol / layer pill, a TIER_* server error, the
// /events padlock, and a lesson lock ALL call UpsellModal::open(...) - a single,
// consistent conversion surface instead of scattered ad-hoc nudges. Primary CTA is
// "Explore Pro plans" -> /pricing in a new tab (Entitlements::open_pricing). AUTH_REQUIRED
// / GRANT_REQUIRED route to a login variant instead (Entitlements::open_login).
//
// Every explicit locked action opens this dialog, including repeated actions.
// render() is called once per frame from the main loop; open(...) can be called
// from anywhere (it just latches). Toasts are reserved for session notices.
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>

namespace ui {

class UpsellModal {
public:
    // Which gate opened it drives the contextual subline and analytics.
    // Auth is the login variant (not logged in / no grant).
    enum class Trigger : uint8_t {
        Generic = 0, Range, Preset, Speed, Symbol, Layer,
        ServerTier, Events, Lesson, Daily, Auth, Research, Timeframe, _Count
    };

    static UpsellModal& instance();

    // Open the upsell for a gate.
    //   `detail` overrides the default subline if non-null - it is PROSE, written
    //           for the reader, and is free to be reworded at any time.
    //   `layer`  is a STABLE ANALYTICS SLUG (e.g. "liq_levels"), never prose:
    //           it is what the funnel GROUPS ON, so it must not track the
    //           wording. Absent from the props when the call site has none, so
    //           rows emitted before this existed stay distinguishable from rows
    //           whose gate genuinely has no layer.
    void open(Trigger t, const char* detail = nullptr, const char* layer = nullptr);
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
    bool blocks_replay_shortcuts() const;

private:
    UpsellModal() = default;
    void render_modal_body();
    void render_copy(float width, bool with_benefits);  // headline, billing, CTAs
    void render_benefits();            // what Pro includes
    void render_preview(float width);  // the recorded clip for this gate, if any
    void render_toast();
    void stop_preview();
    void dismiss();  // shared "Maybe later"/Escape path (runs the redirect, if armed)

    Trigger     trigger_ = Trigger::Generic;
    std::string detail_;               // PROSE subline (human)
    std::string layer_;                // stable analytics slug (empty = absent)
    std::string dismiss_redirect_;   // full path ("/terminal/<sym>"); empty = just close
    bool        login_variant_ = false;
    // Recorded clip shown beside the copy (public/media/terminal/<name>.webm
    // or .mp4 on the web host), chosen per gate; nullptr = text-only dialog.
    const char* preview_ = nullptr;
    unsigned    preview_tex_ = 0;      // GL texture the video frames upload into
    bool        preview_playing_ = false;

    int      dismissed_frame_ = -1;   // do not pass the closing Escape to replay
    bool     want_open_ = false;      // latched by open(), consumed by render()
    bool     open_      = false;      // popup currently on screen
    bool     yearly_billing_ = true;  // annual default; user may choose monthly

    bool        toast_active_ = false;
    double      toast_until_  = 0.0;  // ImGui::GetTime() deadline
    std::string toast_text_;
};

}  // namespace ui
