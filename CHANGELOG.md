# Changelog

## [1.3] - 2026-10-31

Upload the payload as `/data/pldmgr/payloads/local/glint/Glint-1.3-ps5.elf`.

### Payload (PS5)

- **Lower latency:** the payload now sees each finished capture call within
  0.1 ms instead of checking every 5 ms. It makes several such calls per
  frame, so frames and sound leave the console sooner and more evenly.
- **35 Mbit/s by default** (was 50). The encoder caps every frame at
  bitrate / 60; over Wi-Fi a busy game at 50 or more stalled, 35 stays smooth
  at 60 fps. With the PS5 on a cable, `bitrate=N` (up to 100 Mbit/s) keeps
  busy scenes sharper.

### Desktop app

- **Sound much closer to the picture:** about 55-80 ms behind instead of
  95-130 ms, from a smaller output buffer and a tighter automatic delay.
- **Sound recovers after a console freeze:** the backlog is skipped instead of
  playing up to a second behind for half a minute, and one freeze no longer
  holds the sound delay at 400 ms for a minute.
- Pictures go to the screen at the next refresh without queueing when the
  graphics driver supports it (mailbox present mode). Settings and the
  gallery scroll at the monitor's full refresh rate.
- **New look:** centred connect screens with a larger logo, and new fonts:
  Space Grotesk for headings, Inter for text, JetBrains Mono for numbers.
  The Picture tab now has its heading like the other tabs.
- `glint.log` gets a `stats:` line every 10 s while connected: received fps,
  network lateness, decode time, sound buffer, frame sizes and encoder QP.
- Clearer wording in Settings, for example **Picture fit** and **Game audio**.
- Live streaming, already switched off since 1.0, is removed.

### Browser dashboard

- **Smoother on macOS:** the page no longer copies SPS, PPS and AUD into the
  video samples (they belong in the init segment). Safari and Chrome on a Mac
  decode with VideoToolbox, which froze and fell behind on the in-band
  copies.

## [1.2.1] - 2026-10-30

Upload the payload as `/data/pldmgr/payloads/local/glint/Glint-1.2.1-ps5.elf`.

### Payload (PS5)

- Fixed: on some TVs, Glint said "the PS5 switched to 120 Hz output" at
  60 Hz and didn't start. The 120 Hz check assumed one TV's video-out mode
  value (0x10); it now says 120 Hz only for the 120 Hz value (0x40) or when
  the mode changes while Glint runs. Those consoles now also get the
  automatic retry on the first start after a restart.

## [1.2] - 2026-10-29

Upload the payload as `/data/pldmgr/payloads/local/glint/Glint-1.2-ps5.elf`.

> **Glint only works while the PS5 outputs 60 Hz.** When a game switches the
> console to 120 Hz, the encoder stops until the PS5 restarts. Glint now turns
> **120 Hz Output** off while it runs and puts your setting back when it
> stops. **Start Glint from the home screen, before you start a game.** I'm
> working on a way to make it work in 120 Hz mode too.

### Payload (PS5)

- **120 Hz Output off while Glint runs:** Glint sets Settings → Screen and
  Video → Video Output → 120 Hz Output to Off when it starts and restores it
  when it stops. If Glint is closed from Payload Manager, the setting is put
  back the next time Glint stops normally.
- **Starts on the first try after a restart:** the encoder refuses the first
  start after the PS5 boots; Glint now tries again by itself (up to 3 times)
  instead of asking you to load it again.
- When a game has switched the console to 120 Hz, the console message says
  so and what to do, instead of a generic "refused to start".


## [1.1] - 2026-10-29

Upload the payload as `/data/pldmgr/payloads/local/glint/Glint-1.1-ps5.elf`.

### Desktop app (Windows)

- **Picture enhancement**, a new Settings tab: GPU passes between the stream
  and the screen. Profiles Off, Light, Balanced (FSR 1), Enhanced (AI) and
  Custom; upscaling with Bilinear, Lanczos, AMD FidelityFX FSR 1 or FSRCNNX
  (a small neural network, from 1.3x); contrast-adaptive sharpening;
  edge-preserving denoise, debanding and dithering; exposure, saturation and
  shadow detail. Measured on an RX 9070 at 4K output: 0.14 ms (Bilinear) to
  1.23 ms (AI) per frame. "Protect frame time" steps the profile down when
  the passes go over a GPU budget. Off costs nothing.
- **My presets:** save the current values under a name, apply, update,
  revert and delete. The applied preset is marked (Applied, or Changed after
  an edit) and stays applied across restarts. All in `picture.conf`.
- **Side-by-side compare** (C, or Settings → Picture): the original and the
  enhanced picture next to each other; scroll zooms (up to 8x) around the
  pointer, drag pans both, double-click resets. X opens a tweak panel over the
  picture without dimming it.
- **One viewer at a time:** connecting takes the stream from a browser or
  another PC; when one takes it from the app, the app says "Watching
  elsewhere" instead of an error.
- **Sound delay** about 190 → 100 ms: the automatic buffer still grows on a
  choppy network, but starts lower (Lowest 40, Balanced 60, Smooth 120 ms).
- Gallery: follows the folder (files deleted or added in Explorer show up),
  screenshots show their size instead of 0x0, Fit/100% is a proper switch,
  and a zoomed screenshot pans with press-and-drag.
- Fixed: a crash when zooming fast (a recreated image could reuse a freed
  handle), and a crash opening the gallery (the UI's texture pool held 8).

### Payload (PS5)

- **50 Mbit/s** encoder bitrate (was 60; about 35-38 Mbit/s in a busy game):
  sharp in fast motion, and as smooth as lower rates on a clean Wi-Fi. The
  bitrate can only be set when capture starts (docs/API.md, Bitrate).
- **One viewer at a time:** the newest viewer gets the stream; one that was
  pushed out is refused (409) while the new one watches, so two can't fight.
  `status.json` gains `viewers` and `bitrate`.
- Process name **Glint.elf** (listed with the other payloads in Payload
  Manager). The previous run's log is kept as `capture.prev.log`; a refused
  start quits at once and says "Load Glint again".

### Browser dashboard

- Fixed: the picture froze after ~8 s (the buffer was trimmed past the only
  keyframe).
- Lower latency: about 94 → 35 ms from arrival to screen, a jitter buffer for
  sound, and no video download while the tab is hidden.
- Sound is on by default (after the browser's required first click).
- One viewer at a time, with "Watch here" to take the stream back.


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
every size from the designs in `docs/brand/source/` (the bolt traced to a
vector, the lettering cleaned into a mask) into `docs/brand/`,
`app/res/glint.ico` and `payload/assets/banner.png`. `Glint.exe` also carries its version in
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
