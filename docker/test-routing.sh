#!/usr/bin/env bash
# Exercise nginx over HTTP without rebuilding the WASM application.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
config=${1:-"$repo/docker/nginx.conf"}
fixture=$(mktemp -d)
container="edgedepth-routing-$$"
cleanup() {
  docker rm -f "$container" >/dev/null 2>&1 || true
  rm -rf "$fixture"
}
trap cleanup EXIT
chmod 755 "$fixture"
printf '<!doctype html><title>Terminal routing fixture</title>\n' > "$fixture/index.html"
for script in index.js edgedepth-config.js coi-serviceworker.js; do
  printf '// Actual JavaScript asset: %s\n' "$script" > "$fixture/$script"
done
printf 'wasm fixture\n' > "$fixture/index.wasm"
docker run --rm -d --name "$container" -p 127.0.0.1::8080 \
  -v "$config:/etc/nginx/conf.d/default.conf:ro" \
  -v "$fixture:/usr/share/nginx/html:ro" nginx:1.27-alpine >/dev/null
port=$(docker port "$container" 8080/tcp | cut -d: -f2)
base="http://127.0.0.1:$port"
curl -fsS --retry 10 --retry-connrefused --retry-delay 1 "$base/healthz" >/dev/null
check() {
  local path=$1 expected=$2 type=$3
  curl -fsS -D "$fixture/headers" "$base$path" -o "$fixture/body"
  if ! cmp -s "$fixture/$expected" "$fixture/body"; then
    echo "FAIL: $path did not return $expected" >&2
    exit 1
  fi
  grep -qi "^Content-Type: $type" "$fixture/headers"
  grep -qi '^Cross-Origin-Opener-Policy: same-origin' "$fixture/headers"
  grep -qi '^Cross-Origin-Embedder-Policy: require-corp' "$fixture/headers"
}
for route in / /terminal/btcusdt /terminal/binancef/btcusdt /terminal/bybit/btcusdt /terminal/hl/BTC; do
  check "$route" index.html text/html
done
# A browser resolves these relative script URLs from both supported route shapes.
for prefix in '' /terminal /terminal/binancef /terminal/bybit /terminal/hl; do
  for script in index.js edgedepth-config.js coi-serviceworker.js; do
    check "$prefix/$script" "$script" application/javascript
  done
done
check /index.wasm index.wasm application/wasm
status=$(curl -sS -o /dev/null -w '%{http_code}' "$base/missing.js")
test "$status" = 404
printf 'PASS: root, symbol and venue routes serve real assets with isolation headers\n'
