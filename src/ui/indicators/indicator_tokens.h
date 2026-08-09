#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// indicator_tokens.h - C++ constants for the Microstructure Indicator Suite
//
// SOURCE OF TRUTH: the Microstructure Indicator Suite tokens.json
// (2026-07-03; RRGGBBAA), with its SPEC as the implementation contract.
// This header carries only the tokens the shipped panes consume - each
// wave adds its own block. Do not invent values here; edit tokens.json
// first (design owns it), then mirror.
//
// The two color grammars (system rule):
//   EMBER  = liquidation magnitude/density (Field owns it).
//   RISK   = model-classified danger - toxicity regimes, cascade, pills.
//   Green/red stays direction. Teal stays chrome. They never mix.
// ═══════════════════════════════════════════════════════════════════════════════

#include <imgui.h>

namespace IndiTokens {

    // RRGGBBAA → ImU32 (ImGui is ABGR-packed via IM_COL32).
    constexpr ImU32 rgba(unsigned rrggbbaa) {
        return IM_COL32((rrggbbaa >> 24) & 0xFF,
                        (rrggbbaa >> 16) & 0xFF,
                        (rrggbbaa >> 8)  & 0xFF,
                        (rrggbbaa)       & 0xFF);
    }

    // ── RISK ramp (--risk-*) - regime index 0..3 ──
    constexpr ImU32 RISK_NORMAL   = rgba(0x6F7F93FFu);
    constexpr ImU32 RISK_ELEVATED = rgba(0xC9A53CFFu);
    constexpr ImU32 RISK_HIGH     = rgba(0xD96F3DFFu);
    constexpr ImU32 RISK_CRITICAL = rgba(0xFF4257FFu);

    constexpr ImU32 risk_color(int regime_idx) {
        switch (regime_idx) {
            case 1:  return RISK_ELEVATED;
            case 2:  return RISK_HIGH;
            case 3:  return RISK_CRITICAL;
            default: return RISK_NORMAL;
        }
    }

    // ── Shared pane furniture (SPEC §0) ──
    constexpr ImU32 TAG_NEUTRAL = rgba(0x46536AFFu);  // --ed-tag-neutral
    constexpr ImU32 TAG_TEXT    = rgba(0x0B0E15FFu);  // --ed-tag-text
    constexpr ImU32 INK         = rgba(0x8B98ABFFu);  // --ed-ink
    constexpr ImU32 INK_DIM     = rgba(0x5C6878FFu);  // --ed-ink-dim
    constexpr ImU32 INK_FAINT   = rgba(0x39424FFFu);  // --ed-ink-faint

    // ── Toxicity pane (SPEC §2) ──
    constexpr ImU32 CFTI_LINE          = rgba(0x7C8BA18Cu);  // --indi-cfti (slate @0.55)
    constexpr ImU32 TOX_THRESHOLDS     = rgba(0x6F7F9317u);  // --indi-tox-thresholds
    constexpr float TOX_THRESHOLD_1    = 0.30f;              // --indi-tox-threshold-1-val
    constexpr float TOX_THRESHOLD_2    = 0.45f;
    constexpr float TOX_THRESHOLD_3    = 0.60f;
    constexpr float TOX_THRESH_DASH_ON  = 1.0f;              // dash "1 4"
    constexpr float TOX_THRESH_DASH_OFF = 4.0f;
    constexpr float TOX_STRIP_H_PX     = 2.0f;               // --indi-tox-strip-h-px
    constexpr float TOX_STRIP_ALPHA    = 0.9f;               // --indi-tox-strip-alpha
    constexpr float TOX_FALLBACK_MUL   = 0.4f;               // --indi-tox-fallback-mul
    constexpr float TENTATIVE_ALPHA    = 0.55f;              // --indi-tentative-alpha
    constexpr float TENTATIVE_CONF     = 0.65f;              // --indi-tentative-conf-val

    // ── Empty-history epoch (SPEC §0.4) ──
    constexpr ImU32 EPOCH_RULE = rgba(0x5C687866u);
    constexpr ImU32 EPOCH_INK  = rgba(0x39424FFFu);

    // apply alpha multiplier to a packed color's A channel
    constexpr ImU32 mul_alpha(ImU32 col, float m) {
        const unsigned a = (col >> IM_COL32_A_SHIFT) & 0xFF;
        const unsigned na = static_cast<unsigned>(static_cast<float>(a) * m);
        return (col & ~(0xFFu << IM_COL32_A_SHIFT)) |
               ((na > 255 ? 255u : na) << IM_COL32_A_SHIFT);
    }

} // namespace IndiTokens
