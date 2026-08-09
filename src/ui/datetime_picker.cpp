#include "ui/datetime_picker.h"
#include "imgui.h"
#include <cstring>
#include <cstdlib>

void DateTimePicker::init_from_now(int offset_hours) {
    auto now = std::chrono::system_clock::now();
    now += std::chrono::hours(offset_hours);
    time_t t = std::chrono::system_clock::to_time_t(now);
    struct tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    year   = tm.tm_year + 1900;
    month  = tm.tm_mon + 1;
    day    = tm.tm_mday;
    hour   = tm.tm_hour;
    minute = tm.tm_min;
}

void DateTimePicker::init_from_ms(int64_t ms) {
    time_t t = ms / 1000;
    struct tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    year   = tm.tm_year + 1900;
    month  = tm.tm_mon + 1;
    day    = tm.tm_mday;
    hour   = tm.tm_hour;
    minute = tm.tm_min;
}

int64_t DateTimePicker::to_ms() const {
    struct tm t{};
    t.tm_year  = year - 1900;
    t.tm_mon   = month - 1;
    t.tm_mday  = day;
    t.tm_hour  = hour;
    t.tm_min   = minute;
    t.tm_sec   = 0;
    t.tm_isdst = -1;
    time_t epoch = mktime(&t);
    if (epoch < 0) return 0;
    return static_cast<int64_t>(epoch) * 1000;
}

void DateTimePicker::format_into(char* buf, size_t sz) const {
    std::snprintf(buf, sz, "%04d-%02d-%02d %02d:%02d", year, month, day, hour, minute);
}

int DateTimePicker::days_in_month() const {
    static constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int d = days[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        d = 29;
    return d;
}

bool DateTimePicker::render(const char* id, DateTimePicker& picker) {
    bool changed = false;

    char display_buf[32];
    picker.format_into(display_buf, sizeof(display_buf));

    ImGui::PushID(id);

    float btn_w = ImGui::CalcTextSize("2025-03-09 14:00").x + 16.0f;
    if (ImGui::Button(display_buf, ImVec2(btn_w, 0))) {
        ImGui::OpenPopup("##dtpick");
    }

    if (ImGui::BeginPopup("##dtpick")) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));

        // ── Year navigation ────────────────────────────────────────
        if (ImGui::ArrowButton("##py", ImGuiDir_Left)) {
            picker.year--;
            changed = true;
        }
        ImGui::SameLine();
        ImGui::Text("%04d", picker.year);
        ImGui::SameLine();
        if (ImGui::ArrowButton("##ny", ImGuiDir_Right)) {
            picker.year++;
            changed = true;
        }

        ImGui::SameLine(0.0f, 12.0f);

        // ── Month navigation ───────────────────────────────────────
        static constexpr const char* kMonths[] = {
            "Jan", "Feb", "Mar", "Apr", "May", "Jun",
            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
        };
        if (ImGui::ArrowButton("##pm", ImGuiDir_Left)) {
            if (--picker.month < 1) { picker.month = 12; picker.year--; }
            changed = true;
        }
        ImGui::SameLine();
        ImGui::Text("%s", kMonths[picker.month - 1]);
        ImGui::SameLine();
        if (ImGui::ArrowButton("##nm", ImGuiDir_Right)) {
            if (++picker.month > 12) { picker.month = 1; picker.year++; }
            changed = true;
        }

        ImGui::Separator();

        // ── Day-of-week headers ────────────────────────────────────
        static constexpr const char* kDayHeaders[] = {
            "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"
        };
        constexpr float cell_sz = 24.0f;
        for (int i = 0; i < 7; i++) {
            if (i > 0) ImGui::SameLine(0.0f, 2.0f);
            float text_w = ImGui::CalcTextSize(kDayHeaders[i]).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (cell_sz - text_w) * 0.5f);
            ImGui::TextDisabled("%s", kDayHeaders[i]);
        }

        // ── Calendar grid ──────────────────────────────────────────
        struct tm first_tm{};
        first_tm.tm_year  = picker.year - 1900;
        first_tm.tm_mon   = picker.month - 1;
        first_tm.tm_mday  = 1;
        first_tm.tm_isdst = -1;
        mktime(&first_tm);
        int start_dow = (first_tm.tm_wday + 6) % 7;

        int dim = picker.days_in_month();
        int col = 0;

        for (int b = 0; b < start_dow; b++) {
            if (col > 0) ImGui::SameLine(0.0f, 2.0f);
            ImGui::Dummy(ImVec2(cell_sz, cell_sz));
            col++;
        }

        for (int d = 1; d <= dim; d++) {
            if (col > 0) ImGui::SameLine(0.0f, 2.0f);

            char day_label[4];
            std::snprintf(day_label, sizeof(day_label), "%d", d);

            bool is_selected = (d == picker.day);
            if (is_selected) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                    ImVec4(0.15f, 0.65f, 0.60f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            }
            if (ImGui::Button(day_label, ImVec2(cell_sz, cell_sz))) {
                picker.day = d;
                changed = true;
                ImGui::CloseCurrentPopup();
            }
            if (is_selected) ImGui::PopStyleColor(2);

            col++;
            if (col >= 7) col = 0;
        }

        ImGui::Separator();

        // ── Hour : Minute (persistent buffers) ─────────────────────
        // Static buffers per picker instance - keyed by ImGui ID
        static ImGuiID last_id = 0;
        static char hr_buf[4] = {};
        static char mn_buf[4] = {};
        static bool hr_active = false;
        static bool mn_active = false;

        ImGuiID cur_id = ImGui::GetID("##hr");
        if (cur_id != last_id) {
            // Different picker opened - reset buffers
            last_id = cur_id;
            std::snprintf(hr_buf, sizeof(hr_buf), "%02d", picker.hour);
            std::snprintf(mn_buf, sizeof(mn_buf), "%02d", picker.minute);
        }

        // Only sync from picker when not actively editing
        if (!hr_active) std::snprintf(hr_buf, sizeof(hr_buf), "%02d", picker.hour);
        if (!mn_active) std::snprintf(mn_buf, sizeof(mn_buf), "%02d", picker.minute);

        ImGui::SetNextItemWidth(32.0f);
        ImGui::InputText("##hr", hr_buf, sizeof(hr_buf),
            ImGuiInputTextFlags_CharsDecimal);
        hr_active = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            picker.hour = std::clamp(std::atoi(hr_buf), 0, 23);
            std::snprintf(hr_buf, sizeof(hr_buf), "%02d", picker.hour);
            changed = true;
        }

        ImGui::SameLine(0.0f, 2.0f);
        ImGui::TextUnformatted(":");
        ImGui::SameLine(0.0f, 2.0f);

        ImGui::SetNextItemWidth(32.0f);
        ImGui::InputText("##mn", mn_buf, sizeof(mn_buf),
            ImGuiInputTextFlags_CharsDecimal);
        mn_active = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            picker.minute = std::clamp(std::atoi(mn_buf), 0, 59);
            std::snprintf(mn_buf, sizeof(mn_buf), "%02d", picker.minute);
            changed = true;
        }
        // Quick adjust buttons
        ImGui::SameLine(0.0f, 8.0f);
        if (ImGui::SmallButton("-1h")) {
            picker.hour--;
            if (picker.hour < 0) {
                picker.hour = 23;
                picker.day--;
                if (picker.day < 1) {
                    picker.month--;
                    if (picker.month < 1) { picker.month = 12; picker.year--; }
                    picker.day = picker.days_in_month();
                }
            }
            changed = true;
        }
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::SmallButton("+1h")) {
            picker.hour++;
            if (picker.hour > 23) {
                picker.hour = 0;
                picker.day++;
                if (picker.day > picker.days_in_month()) {
                    picker.day = 1;
                    picker.month++;
                    if (picker.month > 12) { picker.month = 1; picker.year++; }
                }
            }
            changed = true;
        }
        // Presets
        ImGui::SameLine(0.0f, 8.0f);
        if (ImGui::SmallButton("00:00")) {
            picker.hour = 0; picker.minute = 0; changed = true;
        }
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::SmallButton("12:00")) {
            picker.hour = 12; picker.minute = 0; changed = true;
        }
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::SmallButton("Now")) {
            picker.init_from_now(); changed = true;
        }
        ImGui::Separator();
        if (ImGui::Button("Done", ImVec2(-1, 0))) {
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
    // Clamp day
    int dim = picker.days_in_month();
    if (picker.day > dim) { picker.day = dim; changed = true; }
    ImGui::PopID();
    return changed;
}