// ═══════════════════════════════════════════════════════════════════════════════
// settings_panel.h — Reusable tabbed settings panel (MMT-style)
//
// Usage:
//   SettingsPanel panel("My Settings", {"General", "Style", "Advanced"});
//   if (panel.begin()) {
//       if (panel.tab("General")) {
//           // ImGui widgets for General tab...
//       }
//       if (panel.tab("Style")) {
//           // ImGui widgets for Style tab...
//       }
//       if (panel.tab("Advanced")) {
//           // ImGui widgets for Advanced tab...
//       }
//       panel.end();
//   }
//
// The panel renders as a popup with a left sidebar of tab names and a right
// content area. Tabs are highlighted when active. The panel handles all
// layout, spacing, and styling internally.
// ═══════════════════════════════════════════════════════════════════════════════
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstdint>

class SettingsPanel {
public:
    // Construct with popup ID and list of tab names.
    // popup_id: the ImGui popup identifier (used with ImGui::OpenPopup / BeginPopup)
    // tab_names: ordered list of tab labels shown in the left sidebar
    SettingsPanel(const char* popup_id, std::initializer_list<const char*> tab_names)
        : popup_id_(popup_id) {
        for (auto* name : tab_names) {
            tabs_.push_back(name);
        }
    }

    // Vector overload for dynamic tab lists
    SettingsPanel(const char* popup_id, const std::vector<std::string>& tab_names)
        : popup_id_(popup_id) {
        for (const auto& name : tab_names) {
            tabs_.push_back(name);
        }
    }

    // Call to open the popup (wraps ImGui::OpenPopup)
    void open() { ImGui::OpenPopup(popup_id_); }

    // Begin the settings panel. Returns true if the popup is open.
    // Call tab() for each tab, then end().
    bool begin() {
        // Use a modal for better focus behavior, or popup for lighter feel.
        // Floating chrome rules: radius 6 (docked panels stay square).
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.0f);
        bool open = ImGui::BeginPopup(popup_id_, ImGuiWindowFlags_NoMove);
        if (!open) {
            ImGui::PopStyleVar(2);
            return false;
        }
        ImGui::PopStyleVar(2);

        // Outer container size
        const float sidebar_width = sidebar_width_;
        const float content_width = content_width_;
        const float total_width = sidebar_width + content_width;
        const float total_height = panel_height_;

        // ── Left sidebar ─────────────────────────────────────────
        ImGui::BeginChild("##settings_sidebar", ImVec2(sidebar_width, total_height),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));

        // Top padding in sidebar
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);

        for (size_t i = 0; i < tabs_.size(); ++i) {
            bool is_selected = (active_tab_ == static_cast<int>(i));

            // Indent tab labels
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0f);

            // Highlight color for active tab
            if (is_selected) {
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.20f, 0.25f, 0.35f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.22f, 0.27f, 0.37f, 1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.15f, 0.18f, 0.25f, 1.0f));
            }

            if (ImGui::Selectable(tabs_[i].c_str(), is_selected,
                                  ImGuiSelectableFlags_None,
                                  ImVec2(sidebar_width - 1.0f, tab_height_))) {
                active_tab_ = static_cast<int>(i);
            }

            ImGui::PopStyleColor(2);
        }

        ImGui::PopStyleVar(2);
        ImGui::EndChild();

        // ── Divider line ─────────────────────────────────────────
        ImGui::SameLine(0, 0);
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(p.x, p.y),
                ImVec2(p.x, p.y + total_height),
                IM_COL32(60, 65, 80, 200), 1.0f);
        }

        // ── Right content area ───────────────────────────────────
        ImGui::SameLine(0, 1.0f);
        ImGui::BeginChild("##settings_content",
                          ImVec2(content_width, total_height),
                          ImGuiChildFlags_None, ImGuiWindowFlags_None);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));

        // Padding inside content area
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);

        // Begin an inner group so content is indented
        ImGui::BeginGroup();

        content_started_ = true;
        current_tab_idx_ = 0;
        return true;
    }

    // Check if a specific tab is active. Call this for each tab in order.
    // Returns true if this tab's content should be rendered.
    // The tab_name must match one of the names passed to the constructor.
    bool tab(const char* tab_name) {
        // Find the index for this tab name
        for (size_t i = 0; i < tabs_.size(); ++i) {
            if (tabs_[i] == tab_name) {
                return active_tab_ == static_cast<int>(i);
            }
        }
        return false;
    }

    // End the settings panel. Must be called after begin() returns true.
    void end() {
        ImGui::EndGroup();
        ImGui::PopStyleVar(); // ItemSpacing
        ImGui::EndChild();    // content area
        ImGui::EndPopup();
        content_started_ = false;
    }

    // ── Configuration ────────────────────────────────────────────
    // Call these before begin() to customize the panel size.
    void set_sidebar_width(float w) { sidebar_width_ = w; }
    void set_content_width(float w) { content_width_ = w; }
    void set_panel_height(float h)  { panel_height_ = h; }
    void set_tab_height(float h)    { tab_height_ = h; }

    // Get/set active tab index (persists across frames)
    int  active_tab() const { return active_tab_; }
    void set_active_tab(int idx) { active_tab_ = idx; }

    // Get popup ID (for OpenPopup calls)
    const char* popup_id() const { return popup_id_; }

private:
    const char* popup_id_;
    std::vector<std::string> tabs_;
    int active_tab_ = 0;
    int current_tab_idx_ = 0;
    bool content_started_ = false;

    // Layout dimensions (pixels)
    float sidebar_width_ = 140.0f;
    float content_width_ = 320.0f;
    float panel_height_  = 350.0f;
    float tab_height_    = 28.0f;
};
