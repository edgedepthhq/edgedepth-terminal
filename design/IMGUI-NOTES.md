# edgedepth - ImGui / ImPlot mapping notes

Practical notes for porting the HTML reference into C++ / ImGui (docking) / ImPlot.
Values come from `tokens.json`. ImGui colors are `ImVec4` (0-1 floats, RGBA).

---

## 1. Global style

```cpp
// Helper: hex → ImU32 / ImVec4
ImVec4 H(uint32_t rgb, float a = 1.f) {
  return ImVec4(((rgb>>16)&0xFF)/255.f, ((rgb>>8)&0xFF)/255.f, (rgb&0xFF)/255.f, a);
}

ImGuiStyle& s = ImGui::GetStyle();
s.WindowRounding   = 0.f;     // panels are square; only pills/buttons round
s.FrameRounding    = 5.f;     // --r2
s.GrabRounding     = 5.f;
s.WindowBorderSize = 1.f;
s.FrameBorderSize  = 1.f;
s.ScrollbarRounding= 6.f;
s.ScrollbarSize    = 9.f;
s.CellPadding      = ImVec2(6, 2);
s.ItemSpacing      = ImVec2(8, 6);

ImVec4* c = s.Colors;
c[ImGuiCol_WindowBg]        = H(0x0A0E12);        // --bg-panel
c[ImGuiCol_ChildBg]         = H(0x0A0E12);
c[ImGuiCol_PopupBg]         = H(0x0A0E12, .98f);
c[ImGuiCol_MenuBarBg]       = H(0x0D1217);        // --bg-elev
c[ImGuiCol_Border]          = H(0x96A8B8, .13f);  // --bd-2
c[ImGuiCol_FrameBg]         = H(0x11171D);        // --bg-input
c[ImGuiCol_FrameBgHovered]  = H(0x161D25);
c[ImGuiCol_FrameBgActive]   = H(0x1D2630);
c[ImGuiCol_Text]            = H(0xE9EFF5);        // --tx-1
c[ImGuiCol_TextDisabled]    = H(0x5F6F7C);        // --tx-3
c[ImGuiCol_Header]          = H(0x22C5DB, .14f);  // brand soft (selection)
c[ImGuiCol_HeaderHovered]   = H(0x161D25);
c[ImGuiCol_HeaderActive]    = H(0x22C5DB, .22f);
c[ImGuiCol_Button]          = H(0x11171D);
c[ImGuiCol_ButtonHovered]   = H(0x161D25);
c[ImGuiCol_ButtonActive]    = H(0x1D2630);
c[ImGuiCol_CheckMark]       = H(0x22C5DB);        // --brand
c[ImGuiCol_SliderGrab]      = H(0x22C5DB);
c[ImGuiCol_TableHeaderBg]   = H(0x0D1217);
c[ImGuiCol_TableBorderLight]= H(0x96A8B8, .07f);  // --bd-1
c[ImGuiCol_TableRowBg]      = H(0x0A0E12);
c[ImGuiCol_TableRowBgAlt]   = H(0x0A0E12);        // no zebra; use hairlines
c[ImGuiCol_PlotLines]       = H(0x22C5DB);
c[ImGuiCol_DockingPreview]  = H(0x22C5DB, .35f);
```

Semantic shorthands to reuse everywhere:
`UP=H(0x15C99E)`, `DOWN=H(0xFF4D6D)`, `BRAND=H(0x22C5DB)`, `WARN=H(0xF3B24A)`.

---

## 2. Fonts

Load two faces (bundle the TTFs, or embed). Build a small atlas with sizes you actually use.

- **Hanken Grotesk** → chrome, labels, headings (13-17px; 9.5px caps for labels).
- **JetBrains Mono** → **all** numerics, every table/ladder/clock. ImGui mono is already
  fixed-advance; that gives you the tabular alignment the CSS gets via `tabular-nums`.

```cpp
io.Fonts->AddFontFromFileTTF("HankenGrotesk-Regular.ttf", 13.f);
ImFont* mono   = io.Fonts->AddFontFromFileTTF("JetBrainsMono-Regular.ttf", 12.f);
ImFont* monoSm = io.Fonts->AddFontFromFileTTF("JetBrainsMono-Regular.ttf", 11.f);
```

---

## 3. Panels / docking

Each HTML panel = one ImGui dockable window. Use the **docking branch** with a default
layout that matches `terminal.html`: watchlist (left, ~254px) · chart center · DOM+tape
(right, ~388px) · transport (bottom, full width, fixed 66px) · stats strip under the menu
bar. Panel headers in the HTML are 31px uppercase labels - emulate with a custom title bar
or `MenuBar` per window; keep chrome quiet.

---

## 4. Chart (ImPlot)

- Candles: custom draw via `ImPlot::GetPlotDrawList()` (ImPlot has no native candle).
  Body = `UP/DOWN` fill at ~0.92 alpha; wick = 1px same color.
- **Liquidation heatmap**: render the field to a texture (your engine owns the data),
  colormap = viridis LUT (`tokens.json`), alpha = `intensity^1.15 * 0.55`, low→transparent.
  Draw it as an image **under** the candle draw list, full plot rect, into the future too
  (levels ahead of the playhead are the point). Add an `ImPlotColormap_Viridis` legend bar.
- **VPVR**: horizontal bars from the right axis growing left; per price bin split
  buy(`UP`)/sell(`DOWN`); value-area bins at full alpha, outside at ~0.42; POC line in
  `WARN`, VAH/VAL dashed in `--tx-3`. See `drawVPVR()` in `render.js` for the exact recipe
  (60 bins, 70% value area).
- Replay playhead: dashed vertical line in `WARN` at the reveal boundary.
- Crosshair: dashed `rgba(150,168,184,.35)` + price/time tags on the axes.

---

## 5. DOM ladder (the upgraded one)

Use `ImGui::BeginTable` with 6 columns: **buys · bids · price · asks · sells · Δ**.

- bids/asks cells: draw a depth bar with the foreground draw list, width ∝ size/maxSize,
  **color = ocean ramp** at `0.12 + size/max*0.88` (small=deep blue → large=green/yellow),
  number on top (near-white, subtle shadow). Bid bar anchored to the price-side edge
  (grows left); ask bar grows right.
- buys/sells: historical traded volume, faint `UP`/`DOWN` (≈0.55 alpha).
- Δ = buys − sells, colored `UP`/`DOWN`.
- Current price row: fill `--bg-elev`, price chip solid `BRAND` with dark text.
- Iceberg markers: 2px vertical tick at the row's left edge (`BRAND` or violet `#b07cff`).
- Keep it centered on the inside price each tick. Ramp LUT in `tokens.json` →
  `heatmap.dom_depth.ocean`.

---

## 6. Replay transport (custom widgets)

Full-width bottom dock, 66px. Build the scrubber as a custom widget over an
`InvisibleButton` rect:
- rail (`--bg-active`), buffered range (`--bd-2`), progress fill (cyan gradient),
  draggable thumb (white + brand glow).
- event flags = thin verticals on the rail (`WARN`, or `DOWN` for severe), hover tooltip,
  click to seek.
- speed pills 0.1×-10× as a segmented row; active = solid `BRAND`, dark text.
- play orb = filled `BRAND` circle. Spacebar toggles.

---

## 7. Indicator pane

A dockable bottom child (Open Interest / CVD / Volume / Funding tabs). Each is a small
ImPlot subplot reading the same `progress` playhead:
OI = line+area (`BRAND`), CVD = signed line with zero baseline (color by sign),
Volume = up/down histogram, Funding = bipolar bars around zero. See `EDIndicator` in
`render.js`.

---

## 8. Symbol finder

Modal/popup with a search input, category chips, and an `ImGui::BeginTable`
(Symbol · Score · VPIN · Price · 24h% · Volume · Type) with sortable headers and colored
type badges. ⌘K opens; arrow keys navigate; Enter loads; Esc closes.
