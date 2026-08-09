# Design system

The single source of truth for the terminal's look. `src/rendering/theme.{h,cpp}`
mirrors these values into ImGui/ImPlot styles at runtime.

| File | What it is |
|---|---|
| `tokens.json` | Machine-readable design tokens: colors, type ramp, spacing, radii, heatmap LUTs |
| `edgedepth.css` | The full system; the `:root` block is the token layer |
| `IMGUI-NOTES.md` | Practical notes for mapping these tokens onto ImGui / ImPlot |

Dark-only by design: near-black cool charcoal surfaces, teal-up / magenta-rose-down
market data, cyan brand accent used sparingly, amber for replay and events.

Changing a color? Edit `tokens.json` first, then mirror it into `Theme::Tokens`.
Widgets must never hardcode an `ImVec4`. Always go through the theme tokens.
