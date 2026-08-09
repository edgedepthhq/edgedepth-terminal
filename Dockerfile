# ── build the WebAssembly client ────────────────────────────────────────────
# emsdk version is pinned to match .github/workflows/build.yml. Emscripten
# releases move fast and a mismatch between CI and this image is exactly the
# sort of thing that only shows up as a runtime fault in someone else's
# browser.
FROM emscripten/emsdk:3.1.61 AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends protobuf-compiler \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN mkdir -p build && cd build \
 && emcmake cmake -DCMAKE_BUILD_TYPE=Release .. \
 && emmake make -j"$(nproc)" c_based_trader_client

# Four artifacts, not three. index.data holds the fonts preloaded via
# --preload-file (CMakeLists.txt), and the app renders nothing without it.
RUN cd build && test -f index.html && test -f index.js \
 && test -f index.wasm && test -f index.data

# Load the runtime config before the Emscripten glue. Injecting straight
# after <head> guarantees it runs before any script the shell emits, so
# window.__EDGEDEPTH_WS_URL__ is set by the time main() reads it.
RUN cd build \
 && sed -i 's|<head>|<head>\n<script src="edgedepth-config.js"></script>|' index.html \
 && grep -q 'edgedepth-config.js' index.html

# ── serve ───────────────────────────────────────────────────────────────────
FROM nginx:1.27-alpine

COPY docker/nginx.conf /etc/nginx/conf.d/default.conf
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

COPY --from=build /src/build/index.html  /usr/share/nginx/html/
COPY --from=build /src/build/index.js    /usr/share/nginx/html/
COPY --from=build /src/build/index.wasm  /usr/share/nginx/html/
COPY --from=build /src/build/index.data  /usr/share/nginx/html/

EXPOSE 8080

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["nginx", "-g", "daemon off;"]
