#!/usr/bin/env bash
# Regenerate fonts/NotoSansCJK-Subset.ttf.
#
# Binance lists a handful of meme perps whose ticker is Chinese (today 龙虾USDT
# and 牛来USDT). Neither Hanken Grotesk nor JetBrains Mono carries a CJK glyph,
# so those rows rendered as "??" (ImGui's U+FFFD fallback) everywhere a symbol
# name is drawn.
#
# The subset is built from the LIVE universe rather than a fixed range: a
# general Chinese font is ~780 KB even cut down to GB2312 level 1, against 2 KB
# for the codepoints actually listed, and this payload is preloaded into
# index.data on every cold boot.
#
# Nothing in the build sees the universe, so there is no build-time check: a
# ticker listed after the last run reverts to "??" in the watchlist, and that
# is the cue to rerun this.
#
# Needs fonttools (pip install fonttools) and a Noto Sans CJK source.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${NOTO_CJK_TTC:-/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc}"
OUT="$ROOT/fonts/NotoSansCJK-Subset.ttf"
API="${SYMBOLS_METADATA_URL:-https://api.edgedepth.com/symbols/metadata}"

[ -f "$SRC" ] || { echo "no CJK source at $SRC (set NOTO_CJK_TTC)" >&2; exit 1; }

# Face 2 of the collection is Noto Sans CJK SC. These tickers are simplified
# Chinese, and the SC face picks the simplified glyph forms.
CODEPOINTS="$(curl -fsS "$API" | python3 -c '
import json, sys
seen = set()
for s in json.load(sys.stdin).get("symbols", []):
    for ch in s.get("symbol", "") + s.get("base_asset", ""):
        if ord(ch) > 127:
            seen.add(ord(ch))
if not seen:
    sys.exit("no non-ascii codepoints in the universe; nothing to subset")
print(",".join("U+%04X" % c for c in sorted(seen)))
')"

echo "subsetting $(echo "$CODEPOINTS" | tr ',' '\n' | wc -l) codepoints: $CODEPOINTS"
pyftsubset "$SRC" \
    --font-number=2 \
    --unicodes="$CODEPOINTS" \
    --output-file="$OUT" \
    --no-hinting --desubroutinize \
    --drop-tables+=GSUB,GPOS,GDEF
ls -l "$OUT"
