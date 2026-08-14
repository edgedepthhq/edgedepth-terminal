#!/bin/sh
# Write the runtime config the page reads before the Emscripten glue loads.
#
# The client resolves its feed in this order (src/main.cpp:343):
#   1. ?ws=<url> query param
#   2. window.__EDGEDEPTH_WS_URL__
#   3. the hosted default
#
# Baking option 2 in at container start is what makes the bundled stack a
# single command: you open http://localhost:8080 with no query string and it
# connects to the gateway beside it. Doing this at START rather than at BUILD
# time means one image works against any gateway URL.
set -eu

WS_URL="${EDGEDEPTH_WS_URL:-ws://localhost:9001/ws}"
OUT=/usr/share/nginx/html/edgedepth-config.js

# JSON-encode via printf %s inside quotes; the URL is operator supplied and
# ends up inside a JS string literal, so strip anything that could close it.
SAFE_URL=$(printf '%s' "$WS_URL" | tr -d '"\\'"'"'<>')

# __EDGEDEPTH_HOSTED__ tells the client it is NOT running behind
# app.edgedepth.com, so it can stop rendering account chrome that is untrue
# here: there is no EdgeDepth account behind a local docker stack. The client
# defaults this to true when absent, so the hosted deploy is unaffected by its
# existence and only this file opts out.
cat > "$OUT" <<EOF
// Generated at container start from EDGEDEPTH_WS_URL. Do not edit.
window.__EDGEDEPTH_WS_URL__ = "${SAFE_URL}";
window.__EDGEDEPTH_HOSTED__ = false;
EOF

echo "edgedepth-terminal: feed set to ${SAFE_URL}"
exec "$@"
