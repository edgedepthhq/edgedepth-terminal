# Setup and source builds

[Back to the README](../README.md). Commands run from the repository root.

## Quick start

The terminal and a live market data feed, both on your machine:

```bash
git clone https://github.com/edgedepthhq/edgedepth-terminal.git
cd edgedepth-terminal
docker compose up
```

Then open **http://localhost:8080**. No API key, no account, no signup. The
feed is [edgedepth-gateway](https://github.com/edgedepthhq/edgedepth-gateway),
a small MIT-licensed Go service that bridges Binance's public WebSocket
streams into this terminal's wire format.

Images are pulled prebuilt; startup time depends on your connection. To compile the
WebAssembly from source instead, `docker compose up --build` (that pulls the
Emscripten toolchain and takes a while).

**Browsers.** The canvas is threaded WebAssembly, so it needs WebGL2,
`SharedArrayBuffer` and a cross-origin-isolated page; the bundled nginx sends the
COOP and COEP headers that buys. Verified booting cross-origin isolated on
2026-08-15: **Chrome 149** and **Firefox 146**. Safari is untested rather than
supported: it has the pieces on paper, but nobody has run it, so treat it as
unknown. If the canvas never appears, check `crossOriginIsolated` in the
console: `false` means something upstream (a proxy, an extension) stripped the
headers.

**If the book moves but the tape is empty**, inspect `docker compose logs gateway`
and verify the gateway revision. Binance now separates `/market` trade streams
from `/public` depth streams. The gateway source uses both;
older images using the legacy combined endpoint can show a moving book with no
trades. Do not treat a trade-stream override as a general repair for an old
image. Regional/network availability still applies.

## Versions and pinning

The image workflows support three kinds of tag:

- `:latest` moves when the default-branch image workflow runs
- `:sha-<short>` names the commit used for an image build
- `:MAJOR.MINOR.PATCH` and `:MAJOR.MINOR` are published when a release is tagged

One gotcha worth stating plainly, because the failure looks like the tag is
missing: the leading `v` is not part of the image tag. The git tag `v0.4.0`
publishes the images `0.4.0` and `0.4`, so `:v0.4.0` fails with
`manifest unknown` while `:0.4.0` is there.

The two images version independently, so choose a published tag for each repository. Check
[the releases](https://github.com/edgedepthhq/edgedepth-terminal/releases) and
[the gateway's](https://github.com/edgedepthhq/edgedepth-gateway/releases) for
what is current, or ask the registry directly, which needs no login and no
Docker:

```bash
curl -s "https://ghcr.io/token?scope=repository:edgedepthhq/edgedepth-terminal:pull&service=ghcr.io" \
  | sed -n 's/.*"token":"\([^"]*\)".*/\1/p' \
  | xargs -I{} curl -s -H "Authorization: Bearer {}" \
      https://ghcr.io/v2/edgedepthhq/edgedepth-terminal/tags/list
```

`docker-compose.yml` uses the moving `:latest` tag, so the quick start
is current without editing anything. Pin a published version when you want a build that
does not move under you. This illustrates the syntax; check the release lists
above before choosing tags:

```yaml
services:
  gateway:
    image: ghcr.io/edgedepthhq/edgedepth-gateway:0.1.0
  terminal:
    image: ghcr.io/edgedepthhq/edgedepth-terminal:0.4.0
```

A digest is the strongest pin, because a version tag can in principle be
repointed while a digest cannot:

```bash
docker compose pull
docker inspect --format='{{index .RepoDigests 0}}' ghcr.io/edgedepthhq/edgedepth-terminal:latest
docker inspect --format='{{index .RepoDigests 0}}' ghcr.io/edgedepthhq/edgedepth-gateway:latest
```

Put the resulting `name@sha256:...` in `docker-compose.yml` and you have both a
pin and a rollback target: keep the previous digest and you can go back to it.

## Building and platform support

The build target is WebAssembly, not a native operating-system executable. A
successful source build produces `index.html`, `index.js`, `index.wasm`, and
`index.data`. The build is threaded, so the server must return COOP and COEP
headers for `SharedArrayBuffer`; the bundled `serve_threaded.py` does this.

Source builds require:

- [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) **4.0.15 or newer**. SDL3 support is unavailable in Emscripten 3.x.
- **`protoc` 21.x**. The instructions below pin 21.12, which reports itself as `libprotoc 3.21.12`. Do not substitute a newer release family.
- CMake 3.15+ and Ninja.

All other dependencies are fetched and pinned by CMake. There are no
submodules or additional system libraries.

### Docker Desktop quick start

On Windows or macOS, install Docker Desktop and use Linux containers. Then:

```text
git clone https://github.com/edgedepthhq/edgedepth-terminal.git
cd edgedepth-terminal
docker compose up
```

Open `http://localhost:8080`. This pulls prebuilt images. To compile the
terminal from source inside the Linux build container, run
`docker compose up --build`. Docker Desktop is an alternative build path; it
does not exercise the native Windows toolchain described below.

### WSL2 source build

Install Ubuntu under WSL2 with `wsl --install -d Ubuntu` from an elevated
PowerShell window, then run the rest inside Ubuntu. Keeping the clone in the
WSL Linux filesystem avoids unnecessary `/mnt/c` filesystem overhead.

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake curl git ninja-build python3 unzip

mkdir -p "$HOME/.local/protoc-21.12"
curl -fsSL \
  -o /tmp/protoc-21.12-linux-x86_64.zip \
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
for artifact in index.html index.js index.wasm index.data; do
  test -s "build-wsl/$artifact"
done
ls -l build-wsl/index.html build-wsl/index.js build-wsl/index.wasm build-wsl/index.data

cmake -S tests/native -B build-native-tests -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-native-tests --config Release --parallel
cmake -E chdir build-native-tests ctest -C Release --output-on-failure

python3 serve_threaded.py 8000 build-wsl
```

Open `http://localhost:8000` from Windows. Add
`?ws=ws://localhost:8080/ws` to connect to your own feed.

The same commands are the supported Linux source-build path outside WSL2.

### Native Windows PowerShell and Ninja source build

Install Git, CMake 3.15+, Ninja, Python 3.8+, and Visual Studio 2022 Build
Tools with the Desktop development with C++ workload. Start a Developer
PowerShell for VS 2022 so the host compiler is available for the native tests.
The WASM build itself uses Emscripten's Clang.

From that PowerShell window, install the pinned tools for the current user:

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
ninja --version
```

The version checks must show Emscripten 4.0.15 and `libprotoc 3.21.12`.
Clone and build the terminal in the same Developer PowerShell session:

```powershell
git clone https://github.com/edgedepthhq/edgedepth-terminal.git
Set-Location edgedepth-terminal

emcmake.bat cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows --target c_based_trader_client --parallel

$Artifacts = @("index.html", "index.js", "index.wasm", "index.data") |
  ForEach-Object { Join-Path "build-windows" $_ }
$Invalid = $Artifacts | Where-Object {
  -not (Test-Path -LiteralPath $_ -PathType Leaf) -or (Get-Item -LiteralPath $_).Length -eq 0
}
if ($Invalid) { throw "Missing or empty build artifacts: $($Invalid -join ', ')" }
Get-Item -LiteralPath $Artifacts

cmake -S tests/native -B build-native-tests -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-native-tests --config Release --parallel
cmake -E chdir build-native-tests ctest -C Release --output-on-failure

python .\serve_threaded.py 8000 build-windows
```

Open `http://localhost:8000`. For later PowerShell sessions, dot-source
`emsdk_env.ps1` again and add the 21.12 `bin` directory to `PATH` before
configuring a new build directory.

### MSYS2 status and caveats

MSYS2 is not in the supported or CI-tested matrix. It may work, but no MSYS2
build has been reproduced for this project, so the project does not claim
support yet. In particular, combining MSYS-style paths with native Windows
Emscripten, CMake, Ninja, or `protoc.exe` can trigger automatic path conversion
and produce malformed compiler, preload-file, or protobuf arguments.

Use native PowerShell/CMD for the Windows toolchain, or WSL2 for a consistent
Linux toolchain. If you experiment with MSYS2, keep every tool and path model
consistent and include the exact shell and tool versions in any build report.
