# Status

What works today, how it was checked, and what is still open. Verified against
a jailbroken PS5 (firmware `0x12600006` family) and the host / mock payloads
unless noted.

## Payload (PS5)

- **Capture**: drives the console's H.264 encoder through libSceAvcap2, called
  inside SceRemotePlay via a ptrace trampoline. 1080p60 only — this console
  refuses 1440p in `OpenVideo` (`0x81950008`), and a failed 1440p attempt can
  leave `Start` refusing with `0x80c78000`, so capture opens 1080p directly.
- **HTTP server** (`:7231`): config/status JSON, `/frames`, `/audio`, RTP,
  recordings list and download, record start/stop, keyframe, stop. See
  `docs/API.md`. Protocol smoke test passes on the host build and the console.
- **Recording** to `/data/glint/recordings`, Annex-B with an SPS/PPS prefix,
  started on the first IDR after `/record/start`.
- **Console banners** on load, ready, encoder-busy, failed-start and stop.
- **Rings**: 64 video / 256 audio slots; a viewer that falls too far behind is
  resynced to the newest IDR. Up to 8 streaming clients.

Open: capture depends on SceRemotePlay being present; the encoder can need a
minute between sessions before it will start again.

## Desktop app (Windows)

- **Live view**: D3D11VA (or FFmpeg) H.264 decode, Vulkan compute YUV→RGBA,
  Vulkan + Dear ImGui UI. About 19% of one CPU core for the whole app on live
  1080p60. Latency Lowest (default), Balanced (holds the 99th percentile of
  the measured jitter, ~15-30 ms) or Smooth (larger buffer, synced to sound).
- **Audio**: SDL2 playback of `/audio`; sound on/off, 0–4× volume, Auto delay
  (sized from measured jitter) or manual 0–400 ms; A/V sync holds video to the
  audio playout.
- **Local recording** to MKV (H.264 stream-copy + AAC), **screenshots** (PNG),
  and a **gallery** (grid with thumbnails: play, export MP4/MKV, delete).
  Default folder `%USERPROFILE%\Videos\Glint`.
- **Live streaming**: switched off in the client (`SL_LIVE_STREAMING`, code kept).
  When built in: up to 5 RTMP/RTMPS destinations (Twitch, YouTube, TikTok,
  custom), re-encoded on the GPU when possible (AMF / Media Foundation / x264),
  keyframe every 2 s. RTMPS goes through Windows TLS with certificate checks.
  Keys and tokens are held in Windows Credential Manager, never in files or logs.
  Optional Twitch/YouTube sign-in with your own OAuth client fetches the key.
- **Settings** in `%LOCALAPPDATA%\Glint` (`console.conf`, `audio.conf`,
  `video.conf`, `capture.conf`, `window.conf`, `destinations.conf`,
  `oauth-apps.conf`). Rolling `glint.log` and a diagnostics zip (no secrets).
- **Built-in viewer** for the gallery: recordings play with sound, seek and
  volume; screenshots zoom and pan. `--view <file>` opens one directly.
- **One file, no console window**: static exe built as a Windows app; version
  1.0 (app and payload).
- **Video page**: GPU/CPU decode, GPU/CPU colour conversion, Latency, graphics
  info. The top chip shows the fps and Mbit/s actually received over the last 2 s.

Verified by hand: live view, audio, recording, screenshots, gallery, settings
persistence, the Video page switches. Streaming built and tested to a private
destination.

## Browser dashboard

Served at `/` by the payload, in the same look as the desktop app (dark by
default, light on the theme button or T, remembered by the browser). Live view
via Media Source Extensions (each access unit wrapped as fragmented MP4;
WebCodecs needs a secure context, which plain HTTP on a LAN address is not),
status pill with the frame strip, sound with volume, screenshot, console
record start/stop, a recordings sheet with downloads, and stop capture behind a
confirmation (stopping can crash the console; restart it before loading
Glint again). Geist loads from Google Fonts when the viewing device is
online, with system fonts as the fallback. Status, audio and the
control endpoints were checked against the host build; the live picture needs a
real console stream (the host build's synthetic stream is not a decodable image).

## Not done yet

- Re-encoding PC recorder (frame-rate / bitrate options).
- Console recording library in the app (browse/download console recordings).
- Image-quality panel (upscale / sharpen / denoise).
- Plugin manager.

Out of scope: Sony Remote Play and cloud sessions.
