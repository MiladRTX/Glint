# Glint desktop app

Windows app that connects to the Glint payload on a PS5, decodes the live
H.264 stream (D3D11VA or FFmpeg), renders it with Vulkan + Dear ImGui 1.91.9b,
plays the sound and drives the payload API.

## What works

- GLFW + Vulkan swapchain + ImGui UI.
- Payload client (libcurl): `config.json`, `status.json`, `/frames`,
  `/audio` transport, keyframe/record/profile/stop commands.
- H.264 decode with FFmpeg; SPS/PPS injected from `config.json` on every IDR.
- UI: the console picture fills the window (Fit/Fill/No upscaling). A glass
  status pill (console, picture, a frame strip of the last second with late
  frames in amber, hold and bitrate), round gallery / full screen / settings
  buttons and a command bar (sound + volume, screenshot, record,
  latency Lowest/Balanced/Smooth, hide) float over it and fade after 3 s without
  input. Recording and live stay shown at the top centre. Settings is a sheet
  that slides in from the right (Console, Video, Audio, Capture, Streaming,
  Help); the gallery is a full-window grid with thumbnails. Without a picture
  the window shows a connect screen with recent consoles.
- Dark (default) and light theme, cross-fading when you switch (Video page or T).
  Geist and Geist Mono are embedded in the exe (`vendor/fonts`, SIL OFL;
  regenerate `src/fonts.inc` with `node tools/embed_fonts.js`). Icons come from
  Segoe Fluent Icons (Windows 11); without that font the UI falls back to text.
  Shortcuts: M sound, S screenshot, R record, G gallery, P picture size,
  L latency, H hide controls, T theme, F11 full screen, Ctrl+, settings, Esc close.
- Console recording start/stop, keyframe, stop stream. Capture is 1080p60.
- Local PC recording to MKV (H.264 stream-copy + AAC), PNG screenshots, and a
  gallery (play, export MP4/MKV, delete). Recordings and screenshots open in
  a built-in viewer (sound, seek, volume; zoom and pan for pictures; Space,
  arrows, Page Up/Down, Esc). Default folder
  `%USERPROFILE%\Videos\Glint`.
- Live streaming is **switched off in this build** (`SL_LIVE_STREAMING` in
  `src/main.cpp`; build with `-DSL_LIVE_STREAMING=1` to bring it back). When on
  (Settings > Streaming, Go live in the command bar): up to 5 RTMP/RTMPS
  destinations at once (Twitch, YouTube, TikTok, custom), re-encoded with the
  GPU when possible (AMD AMF, Media Foundation, else x264), keyframe every 2 s.
  RTMPS goes through Windows TLS with certificate checks. Keys and tokens live
  in Windows Credential Manager. Optional Twitch/YouTube sign-in with your own
  registered app fetches the key for you.
- SDL2 audio playback of the payload `/audio` stream: Sound on/off, volume 0-4x,
  delay Auto (default, sized from measured network jitter) or manual 0-400 ms.
- A/V sync: video is held by the audio playout latency plus an A/V offset.
- Video page: GPU (D3D11VA) or CPU decoding, GPU (Vulkan compute) or CPU colour
  conversion, Latency Lowest/Balanced/Smooth, graphics card and driver info.
- Settings survive restarts in `%LOCALAPPDATA%\Glint`: `console.conf`,
  `audio.conf`, `video.conf`, `capture.conf` (folder), `window.conf` (incl. theme),
  `destinations.conf`, `oauth-apps.conf`. Rolling `glint.log` and a
  diagnostics zip (Help page) without secrets.
- Headless modes for testing; `tests/mock_payload.js` serves the payload API
  from a recording when no console is available.

## What is not implemented yet

- Re-encoding PC recorder, console recording library, image quality panel.
- Plugin manager and store.

Out of scope: Sony Remote Play and cloud sessions.

## Build (MSYS2 / MinGW-w64)

Dependencies installed with pacman:

```
base-devel                    # make, tar, diffutils for build_deps.sh
mingw-w64-x86_64-gcc
mingw-w64-x86_64-pkgconf
mingw-w64-x86_64-cmake        # builds curl, with ninja
mingw-w64-x86_64-ninja
mingw-w64-x86_64-nasm         # FFmpeg's x86 assembly
mingw-w64-x86_64-amf-headers  # AMD hardware encoder
mingw-w64-x86_64-x264
mingw-w64-x86_64-zlib
mingw-w64-x86_64-glfw
mingw-w64-x86_64-vulkan-loader
mingw-w64-x86_64-SDL2         # audio output
mingw-w64-x86_64-shaderc      # optional: glslc, rebuilds the compute shader
```

From an MSYS2 MinGW 64-bit shell:

```bash
cd app
./build_deps.sh     # once: static FFmpeg and curl -> app/deps/prefix
./build.sh          # -> app/build/Glint.exe, app/build/probe.exe
```

`build_deps.sh` downloads pinned FFmpeg and curl sources (checked by SHA-256)
and builds them with only what Glint uses. `build.sh` links them and
everything else statically, so `Glint.exe` is a single file: it needs no
MSYS2 DLLs, only Windows and the graphics driver's Vulkan loader.

## Run

Double-click `build/Glint.exe` (no console window opens; started from a
terminal, `--version` and the test modes print there), or pass your console's address to
`run.cmd` (or set `GLINT_URL`), or run it manually. The examples below use
`<ps5-ip>` as a placeholder; replace it with your console's address.

```bash
build/Glint.exe --url http://<ps5-ip>:7231
build/Glint.exe --url http://<ps5-ip>:7231 --fullscreen
build/Glint.exe --url http://<ps5-ip>:7231 --headless 60
build/Glint.exe --version
build/Glint.exe --url http://<ps5-ip>:7231 --audio-soak 600    # headless audio soak
build/Glint.exe --url http://<ps5-ip>:7231 --audio-soak 60 --audio-exercise
build/Glint.exe --url http://<ps5-ip>:7231 --audio-cycles 5    # start/stop thread check
build/Glint.exe --url http://<ps5-ip>:7231 --record-test 60 --record-dir C:\tmp\rec
build/Glint.exe --gallery-test C:\tmp\rec                      # list / export / delete
build/Glint.exe --url http://<ps5-ip>:7231 --page 4            # open Settings on a tab (1-5)
build/Glint.exe --url http://<ps5-ip>:7231 --gallery           # open the gallery
build/Glint.exe --url http://<ps5-ip>:7231 --theme light       # dark (default) or light
build/Glint.exe --view C:\tmp\rec\clip.mkv                     # open a recording or screenshot in the viewer
build/Glint.exe --url http://<ps5-ip>:7231 --broadcast-test 20 --broadcast-url rtmp://127.0.0.1:1935/live/key   # streaming builds only
build/Glint.exe --tls-probe live.twitch.tv:443                 # TLS handshake only (streaming builds)
build/Glint.exe --decode-file rec.mkv gpu                      # decoder check on a file
```

Tests that touch settings or the vault honour `GLINT_SETTINGS_DIR` and
`GLINT_VAULT_PREFIX`, so they never see your real destinations (`--store-test`
and `--oauth-test` exist in streaming builds only):

```bash
GLINT_SETTINGS_DIR=/tmp/sl GLINT_VAULT_PREFIX=Glint-selftest build/Glint.exe --store-test
GLINT_OAUTH_TEST_BASE=http://127.0.0.1:18080 build/Glint.exe --oauth-test twitch   # local mock
```

The payload must be running on the console first. `--headless N` decodes N
frames and exits with status 0/1; it is the quickest regression check.

## Layout

```
src/main.cpp                  Vulkan + ImGui bootstrap, video texture, YUV converter, UI, headless modes
src/core/app.cpp              payload session, decode thread, status thread
src/core/payload_client.cpp   libcurl HTTP client (include/glint/payload_client.h)
src/core/settings.cpp         saved settings, migration from the old name
src/core/log.cpp              log file, diagnostics zip
src/media/decoder.cpp         H.264 decode (D3D11VA / FFmpeg)
src/media/audio.cpp           SDL2 audio: /audio thread -> ring -> callback
src/media/recorder.cpp        local MKV recorder (H.264 copy + AAC, writer thread)
src/media/media.cpp           screenshots, gallery, export
src/media/player.cpp          built-in viewer's player (demux, decode, SDL sound clock)
src/stream/broadcast.cpp      live encoder + per-destination RTMP writers
src/stream/tls_relay.cpp      RTMPS through Windows TLS (Schannel)
src/stream/destinations.cpp   stream-destinations.conf, Credential Manager vault
src/stream/oauth.cpp          Twitch device flow, YouTube loopback + PKCE
src/shaders/                  YUV -> RGBA compute shader (+ generated SPIR-V)
tools/probe.cpp               headless transport probe / recorder
vendor/imgui/                 Dear ImGui 1.91.9b
```

## Live test checklist (streaming builds only)

1. Use a throwaway or unlisted channel.
2. Settings > Live streaming > Add a destination > Twitch / YouTube / Custom,
   paste the stream key, Save. (Or sign in: Twitch needs a Client ID from
   dev.twitch.tv/console; YouTube a Google "Desktop app" OAuth client ID and
   secret with the YouTube Data API v3 enabled.)
3. Pick quality (1080p60 6000 kbps is a good start), click Go live twice.
4. Watch the destination rows: Connecting -> Live, kbps, any error text.
5. End broadcast; the platform should show the stream ending normally.
