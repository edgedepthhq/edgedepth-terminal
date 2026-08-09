#pragma once
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <chrono>

// ── Standalone ImGui date-time picker ─────────────────────────────────────
// Usage:
//   DateTimePicker picker;
//   picker.init_from_now(-1);            // 1 hour ago
//   if (DateTimePicker::render("start", picker)) { /* value changed */ }
//   int64_t ms = picker.to_ms();
//
// Opens as a popup anchored to a button showing "YYYY-MM-DD HH:MM".
// No modal. No blocking. Compact for dense toolbar layouts.
// ──────────────────────────────────────────────────────────────────────────

struct DateTimePicker {
    int year   = 2025;
    int month  = 1;
    int day    = 1;
    int hour   = 0;
    int minute = 0;

    // Initialize from current time + offset
    void init_from_now(int offset_hours = 0);

    // Initialize from unix milliseconds
    void init_from_ms(int64_t ms);

    // Convert to unix milliseconds (local time)
    int64_t to_ms() const;

    // Format as "YYYY-MM-DD HH:MM" into buffer
    void format_into(char* buf, size_t sz) const;

    // Days in current month (handles leap years)
    int days_in_month() const;

    // ── Rendering ─────────────────────────────────────────────────────
    // Renders a button + popup calendar. Returns true if value changed.
    // `id` must be unique within the current ImGui ID stack.
    static bool render(const char* id, DateTimePicker& picker);
};