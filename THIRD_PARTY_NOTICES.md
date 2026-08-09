# Third-party notices

The EdgeDepth Terminal build fetches and links the following third-party components (all pinned in `CMakeLists.txt` via FetchContent):

| Component | Version / pin | License |
|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) (docking branch) | commit `dee5bf3ec` | MIT |
| [ImPlot](https://github.com/epezent/implot) | commit `d65a2bef` | MIT |
| [zstd](https://github.com/facebook/zstd) | v1.5.6 (decompression only) | BSD-3-Clause |
| [Protocol Buffers](https://github.com/protocolbuffers/protobuf) (lite runtime) | v21.12 | BSD-3-Clause |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.12.0 | MIT |
| SDL3 + WebGL2 bindings | via Emscripten | zlib |
| [Emscripten](https://emscripten.org) | toolchain | MIT/LLVM |
| [coi-serviceworker](https://github.com/gzuidhof/coi-serviceworker) | vendored (`src/coi-serviceworker.js`) | MIT |

## Fonts (embedded in the WASM asset bundle)

| Font | License |
|---|---|
| [Hanken Grotesk](https://github.com/marcologous/hanken-grotesk) | SIL Open Font License 1.1 |
| [JetBrains Mono](https://github.com/JetBrains/JetBrainsMono) | SIL Open Font License 1.1 |

## Symbol icons

Coin/exchange icons rendered in the watchlist are fetched from EdgeDepth's CDN at runtime and are not part of this repository. They derive from [VadimMalykhin/binance-icons](https://github.com/VadimMalykhin/binance-icons) (MIT) plus exchange brand kits. Exchange names and logos are trademarks of their respective owners.
