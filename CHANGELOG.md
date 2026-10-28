# Changelog


## [1.0] - 2026-10-28

Screenlink is now **Glint**: `Glint.exe`, `glint-payload.elf` (upload it as
`/data/pldmgr/payloads/local/glint/Glint-1.0-ps5.elf`), and `GLINT_*`
environment variables. Nothing is lost on the way over:

- Settings in `%LOCALAPPDATA%\Screenlink` are copied to `%LOCALAPPDATA%\Glint`
  on the first start (the old folder is left alone); settings files saved by
  Screenlink still load.
- The default recordings folder `Videos\Screenlink` is renamed to `Videos\Glint`.
- On the console, `/data/screenlink` (log and recordings) is renamed to
  `/data/glint` the first time the payload starts.
- Stream keys saved in Credential Manager under `Screenlink/...` are still found.
- The browser dashboard keeps the theme you picked.

New logo: a black and white lightning bolt. It is the exe's icon (Explorer,
taskbar, title bar), sits above the connect screen's title and in the Settings
footer, is the dashboard's tab icon (black in a light browser, white in a dark
one) and makes up the payload's 960x400 banner. `tools/make_brand.py` renders
every size from one traced bolt into `docs/brand/`, `app/res/glint.ico` and
`payload/assets/banner.png`. `Glint.exe` also carries its version in
Properties.

### Desktop app (Windows)

- `Glint.exe` is a single self-contained file: `app/build_deps.sh`
  builds a minimal static FFmpeg and curl (Schannel TLS), and `build.sh`
  links everything statically. No MSYS2 DLLs or `PATH` setup are needed to
  run it. RTMPS always goes through the Windows TLS relay.
- Recording and streaming place frames on an exact 1/fps timeline
  (`frame_grid.h`) instead of the console's jittery read times: no more
  timestamp collisions in recordings or dropped frames when streaming.
- Stopping a stalled stream no longer waits for the idle timeout.
- New **Balanced** latency (Video page, `L` cycles all three): paces frames
  like Smooth but holds only the 99th percentile of the last 10 s of
  jitter (~15-30 ms measured on Wi-Fi) and does not wait for the sound.
- Paced frames (Balanced and Smooth) wake on a high-resolution timer:
  condition-variable waits overshot by 2-15 ms, so about half of all paced
  frames were shown late. Now under 1 % on the same timing.
- `--latency lowest|balanced|smooth` for `--headless`, which also reports
  the hold and the number of late frames.
- New look: zinc neutrals, Geist type (embedded), glass chrome over the
  picture, a status pill with a frame strip (late frames in amber), a command
  bar with the latency switch, a settings sheet that slides in from the right,
  a full-window gallery with thumbnails, and a connect screen with recent
  consoles. Animated hover, press, switches, segments, sheet and toasts.
- Dark and light theme (dark by default): Video page, `T`, or `--theme`.
  Saved in `window.conf`.
- `--gallery` opens the gallery at start; `G` toggles it.
- Live streaming is switched off in the client: the Streaming tab, Go live,
  the live badge, account sign-in and the streaming test modes are compiled
  out behind `SL_LIVE_STREAMING` (the engine in `src/stream/` is kept).
- Settings: a square sheet (no rounded corners) with eased wheel scrolling
  and no scrollbar (the gallery grid too); tabs spread evenly with an
  underline (no rounded pills); a row
  whose control is wide (address, folder, sliders, long segments) puts the
  control under its description instead of squeezing the text; Medium-weight
  labels for tabs, segments, section headings and buttons; larger titles.
  Stop capture warns that it can crash the console.
- Gallery: the Play/Open, Export and Delete buttons work (the grid used to
  take their clicks).
- Built-in viewer: Play or a double-click on a gallery item opens it inside
  the app. Recordings play with sound, seek bar (drag to scrub), pause,
  volume and mute; screenshots open fitted, with wheel zoom, drag to pan and
  100 %. Arrows / Page Up / Page Down step through the gallery; Space pauses,
  Left/Right seek 5 s, Up/Down volume, Esc closes. "Open in another app"
  hands the file to Windows. `--view <file>` opens a file directly.
- No console window: the exe is a Windows app (`-mwindows`). Started from a
  terminal, `--version` and the test modes still print there.
- Version 1.0 (window title, Settings footer, `--version`, log, payload
  banner and `status.json`).
- Removed: the screenshot watermark (setting, `--watermark`, capture.conf
  keys; old files still load). FFmpeg no longer carries the JPEG and BMP
  decoders it needed.

### Browser dashboard

- Rebuilt in the desktop app's look, dark by default with a light theme the
  browser remembers. Chrome fades when idle; the frame strip, volume control,
  a recordings sheet and a stop confirmation (warns that stopping can crash the
  console) are new.

### Payload (PS5)

- A client that connects and closes without sending a request no longer
  makes the server read an uninitialised buffer.
- `/frames` no longer forces a keyframe every 2 s (one on connect and after
  a skip only). On Wi-Fi each 250-350 KB IDR was a latency spike: measured
  in gameplay, p99 transport delay fell from 39 to 20 ms. The PC recorder
  requests a keyframe every 2 s while it records, so files stay seekable.


## [0.1.0]

Initial release.

### Payload (PS5)

- H.264 capture through libSceAvcap2, driven inside SceRemotePlay via a
  ptrace trampoline (`inject.c`).
- 1080p60 only; profile and encoder error codes handled explicitly (see
  `docs/CAPTURE-INTERNALS.md`).
- HTTP server on port 7231: `/config.json`, `/status.json`, `/frames`,
  `/audio`, RTP/AVC, recordings list and download, record start/stop,
  keyframe, stop. Full API in `docs/API.md`.
- Recording to `/data/glint/recordings` with Annex-B SPS/PPS prefix,
  started on the first IDR after `/record/start`.
- Console notification banners on load, ready, encoder-busy, failed-start
  and stop.
- 64 video / 256 audio ring slots. Up to 8 streaming clients.

### Desktop app (Windows)

- Live view: D3D11VA (or FFmpeg) H.264 decode, Vulkan compute YUV->RGBA,
  Vulkan + Dear ImGui UI.
- SDL2 audio playback of `/audio`; sound on/off, 0–4× volume, Auto delay
  (sized from measured jitter) or manual 0–400 ms; A/V sync holds video to
  the audio playout.
- Local recording to MKV (H.264 stream-copy + AAC), PNG screenshots (optional
  watermark), gallery (play, export MP4/MKV, delete).
- Live streaming: up to 5 RTMP/RTMPS destinations (Twitch, YouTube, TikTok,
  custom), re-encoded with AMF / Media Foundation / x264, keyframe every 2 s.
  RTMPS through Windows TLS with certificate checks. Keys and tokens held in
  Windows Credential Manager.
- Optional Twitch/YouTube sign-in with your own OAuth client.
- Settings persisted under `%LOCALAPPDATA%\Glint`. Rolling
  `glint.log` and a diagnostics zip (no secrets).

### Browser dashboard

- Served at `/` by the payload. Live view via Media Source Extensions
  (fragmented MP4; WebCodecs needs a secure context), status readout,
  snapshot, record start/stop, recordings list with download, and stop.
