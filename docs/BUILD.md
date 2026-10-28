# Building Glint

Glint ships two artifacts:

- `app/` — the Windows desktop app (C++17, GLFW + Vulkan + ImGui, FFmpeg, SDL2).
- `payload/` — the PS5 payload (C11) and a POSIX host build that serves the
  same HTTP API with a synthetic capture, for protocol tests.

The two are built independently and connect over the network.

Dear ImGui (`app/vendor/imgui`, v1.91.9b) is a git submodule: clone with
`git clone --recursive`, or run `git submodule update --init` in an existing
clone.

## Prerequisites

### Windows desktop app (MSYS2 / MinGW-w64)

Install [MSYS2](https://www.msys2.org/) and, from an **MSYS2 MinGW 64-bit**
shell:

```bash
pacman -S --needed \
  base-devel \
  mingw-w64-x86_64-gcc \
  mingw-w64-x86_64-pkgconf \
  mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-nasm \
  mingw-w64-x86_64-amf-headers \
  mingw-w64-x86_64-x264 \
  mingw-w64-x86_64-zlib \
  mingw-w64-x86_64-glfw \
  mingw-w64-x86_64-vulkan-loader \
  mingw-w64-x86_64-SDL2 \
  mingw-w64-x86_64-shaderc   # optional: rebuilds the YUV->RGBA compute shader
```

FFmpeg and curl are not taken from MSYS2: `app/build_deps.sh` builds them (see
below).

### PS5 payload

Install the [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) and set
`PS5_PAYLOAD_SDK` to its root:

```bash
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
```

The payload is built with `prospero-clang` from that SDK and links against
`libSceAvcap2.sprx` / `libSceResourceArbitrator.sprx` / `libSceLibcInternal.sprx`
at runtime (resolved through `kernel_dynlib_handle`/`kernel_dynlib_dlsym`).

### Host test build

Any recent `cc` that handles C11 and has pthreads works:

```bash
sudo apt install build-essential   # Debian / Ubuntu
# or
pacman -S --needed base-devel      # MSYS2
```

## Building

### Desktop app

From an MSYS2 MinGW 64-bit shell:

```bash
cd app
./build_deps.sh                 # once, a few minutes: static FFmpeg + curl -> app/deps/prefix
./build.sh                      # -> app/build/Glint.exe, app/build/probe.exe
```

`build_deps.sh` downloads pinned FFmpeg and curl sources, checks their SHA-256,
and builds static libraries with only what the app uses:

- FFmpeg: the H.264 decoder with D3D11VA, the AAC, PNG and H.264 encoders
  (AMF, Media Foundation, x264), MKV/MP4/FLV output, MKV/MP4/image input,
  and plain RTMP. RTMPS goes through the app's Windows TLS relay
  (`src/stream/tls_relay.cpp`), so FFmpeg needs no TLS library. x264 makes
  the FFmpeg build GPL, as MSYS2's own FFmpeg already is.
- curl: HTTP and HTTPS only, with Schannel (Windows TLS and certificate store),
  built with CMake + Ninja (about 20 seconds; FFmpeg takes most of the time).

`build.sh` then links everything statically (`-static`), so `Glint.exe`
runs on its own: the only DLLs it loads are Windows' own and the graphics
driver's `vulkan-1.dll`. Delete `app/deps` and run `build_deps.sh` again to
rebuild the libraries.

The build script embeds the SPIR-V for the YUV->RGBA compute shader; the
generated `.spv.inc` is kept in the repo so a machine without `glslc` still
builds.

### Payload (PS5 ELF)

```bash
cd payload/ps5
make                             # -> payload/build/glint-payload.elf
                                 # -> payload/build/glint-payload-test.elf
```

The test ELF (`-DSL_TEST_CAPTURE`) uses the host synthetic capture backend
and is only useful for exercising the HTTP server from a real console.

### Payload (host test build)

```bash
cd payload
make                             # -> payload/build/glint-payload (objects in build/obj)
node ../tests/run_smoke.js 19099 # starts the host build itself, then runs the test
```

`run_smoke.js` launches `glint-payload` on the given port, so do not start
one yourself first or the port will already be taken.

The host build serves the full HTTP API with a synthetic stream, so the
server, ring buffers, recording and web assets can be tested without a
console.

## Loading on the console

See [`docs/CONSOLE.md`](CONSOLE.md) for upload, load, validate and stop
instructions.

## Capture internals

See [`docs/CAPTURE-INTERNALS.md`](CAPTURE-INTERNALS.md) for the libSceAvcap2
call sequence, parameter-block layout, and the error codes the capture loop
treats specially.
