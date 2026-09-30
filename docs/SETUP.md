# Run or build the terminal

[README](../README.md) · [Data sources](DATA_SOURCES.md)

## Quick start

Install Git and Docker with Compose. On Windows/macOS, use Docker Desktop with
Linux containers. Then:

```bash
git clone https://github.com/edgedepthhq/edgedepth-terminal.git
cd edgedepth-terminal
docker compose up
```

Open **http://localhost:8080**. This runs the terminal and the community gateway
with Binance's public feed. No account or API key is needed; exchange access
still depends on your network and region.

Images are prebuilt. To compile inside Docker, use `docker compose up --build`.
For a file, replay pack or custom feed, see [data sources](DATA_SOURCES.md).

### Troubleshooting

- **Blank canvas:** the browser needs WebGL2, `SharedArrayBuffer` and cross-origin
  isolation. Check `crossOriginIsolated` in its console. If false, check that your
  proxy preserves the bundled server's COOP/COEP headers. Chrome and Firefox have
  boot checks recorded; Safari remains unverified.
- **Book moves, tape is empty:** inspect `docker compose logs gateway`. Older
  gateway images used a legacy Binance endpoint; current source separates trade
  and depth connections. Update to a published gateway revision and check network
  access before changing stream URLs.

## Pin a version

Terminal and gateway releases are independent. Choose published tags from their
[terminal](https://github.com/edgedepthhq/edgedepth-terminal/releases) and
[gateway](https://github.com/edgedepthhq/edgedepth-gateway/releases) release pages,
then change each `image:` in `docker-compose.yml`.

| Image tag | Meaning |
| --- | --- |
| `latest` | Moving default used by Compose. |
| `sha-<short>` | Build for a specific source commit. |
| `MAJOR.MINOR.PATCH` / `MAJOR.MINOR` | Release tags, **without the leading `v`**. Git tag `v0.5.1` produces image tag `0.5.1`. |

For an immutable pin, pull and read both image digests:

```bash
docker compose pull
docker inspect --format='{{index .RepoDigests 0}}' ghcr.io/edgedepthhq/edgedepth-terminal:latest
docker inspect --format='{{index .RepoDigests 0}}' ghcr.io/edgedepthhq/edgedepth-gateway:latest
```

Use each resulting `name@sha256:...` as its `image:` value. Keep the previous
values for rollback; version tags can be repointed, digests cannot.

## Build from source

The output is browser WebAssembly: `index.html`, `index.js`, `index.wasm` and
`index.data`, not a native desktop executable. Requirements:

- Emscripten **4.0.15+** for SDL3; the recipes pin 4.0.15.
- **`protoc` 21.x**; pin 21.12 (`libprotoc 3.21.12`). Do not use a newer family.
- CMake 3.15+, Ninja, Python and Git. CMake fetches the remaining dependencies.

Use the bundled `serve_threaded.py` or an equivalent server with COOP/COEP
headers. An ordinary static server will not enable threaded WebAssembly.

### Linux / WSL2

For WSL2, install Ubuntu using `wsl --install -d Ubuntu` in an elevated
PowerShell window, then run this inside Ubuntu. Keep source in the Linux
filesystem rather than `/mnt/c`. These tool downloads are for x86_64.

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake curl git ninja-build python3 unzip

mkdir -p "$HOME/.local/protoc-21.12"
curl -fsSL -o /tmp/protoc-21.12-linux-x86_64.zip \
  https://github.com/protocolbuffers/protobuf/releases/download/v21.12/protoc-21.12-linux-x86_64.zip
unzip -q /tmp/protoc-21.12-linux-x86_64.zip -d "$HOME/.local/protoc-21.12"
export PATH="$HOME/.local/protoc-21.12/bin:$PATH"

git clone https://github.com/emscripten-core/emsdk.git "$HOME/emsdk"
cd "$HOME/emsdk"
./emsdk install 4.0.15
./emsdk activate 4.0.15
source ./emsdk_env.sh

cd "$HOME"
git clone https://github.com/edgedepthhq/edgedepth-terminal.git
cd edgedepth-terminal
emcmake cmake -S . -B build-wsl -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-wsl --target c_based_trader_client --parallel
python3 serve_threaded.py 8000 build-wsl
```

Open **http://localhost:8000** (from Windows if using WSL2). Add
`?ws=ws://localhost:8080/ws` to use a feed listening on that address.

### Windows PowerShell

Install Git, CMake 3.15+, Ninja, Python 3.8+, and Visual Studio 2022 Build Tools
with **Desktop development with C++**. Open **Developer PowerShell for VS 2022**.
The WASM build uses Emscripten's Clang; the host compiler runs native tests.

Install the pinned tools for your user:

```powershell
$ToolsRoot = Join-Path $env:LOCALAPPDATA "EdgeDepth\tools"
$EmsdkRoot = Join-Path $ToolsRoot "emsdk"
$ProtocRoot = Join-Path $ToolsRoot "protoc-21.12"
$ProtocZip = Join-Path $ToolsRoot "protoc-21.12-win64.zip"
New-Item -ItemType Directory -Force -Path $ToolsRoot | Out-Null

git clone https://github.com/emscripten-core/emsdk.git $EmsdkRoot
Push-Location $EmsdkRoot
.\emsdk.ps1 install 4.0.15
.\emsdk.ps1 activate 4.0.15
. .\emsdk_env.ps1
Pop-Location

Invoke-WebRequest -Uri "https://github.com/protocolbuffers/protobuf/releases/download/v21.12/protoc-21.12-win64.zip" -OutFile $ProtocZip
Expand-Archive -LiteralPath $ProtocZip -DestinationPath $ProtocRoot -Force
$env:Path = "$(Join-Path $ProtocRoot 'bin');$env:Path"

emcc --version
protoc --version
```

Check for Emscripten 4.0.15 and `libprotoc 3.21.12`, then build in the same session:

```powershell
git clone https://github.com/edgedepthhq/edgedepth-terminal.git
Set-Location edgedepth-terminal
emcmake.bat cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows --target c_based_trader_client --parallel
python .\serve_threaded.py 8000 build-windows
```

Open **http://localhost:8000**. In later sessions, dot-source `emsdk_env.ps1` and
add the pinned `protoc` bin directory to `PATH` again.

MSYS2 is unverified and not in CI. Its path conversion can break native Windows
tool arguments; use PowerShell or WSL2 for the documented build paths.

For checks before contributing, follow [CONTRIBUTING](../CONTRIBUTING.md).
