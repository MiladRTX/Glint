# Glint payload HTTP API (port 7231)

The payload on the PS5 serves the console's H.264 encoder output, its audio and a
set of controls over HTTP. The desktop app and the browser dashboard are both
clients of this API.

Transport: plain HTTP/1.1 over TCP. Every response sets
`Connection: close` and `Cache-Control: no-store` (where applicable).
The request line is parsed as `%11s %255s`; only the method and path are used.
A request must be complete within 5 seconds (server-side receive timeout 1 s,
header deadline 5 s, 4095-byte limit).

## Routes

| Method | Path | Response |
|---|---|---|
| GET | `/` | `text/html; charset=utf-8`, the dashboard page |
| GET | `/banner.png` | `image/png`, the banner image |
| GET | `/config.json` | stream configuration |
| GET | `/status.json` | live statistics |
| GET | `/frames` | framed H.264 stream (endless) |
| GET | `/audio` | framed PCM stream (endless) |
| GET | `/rtp/video?port=N` | RTP/AVC stream to the caller's IP on UDP port N |
| GET | `/recordings` | JSON array of recordings |
| GET | `/recordings/<name>` | recording download |
| POST | `/record/start` | arm recording on the next IDR |
| POST | `/record/stop` | close and finalize the recording |
| POST | `/video/keyframe` | request an IDR from the encoder |
| POST | `/capture/profile?height=H` | change capture height |
| POST | `/capture/restart` | restart the capture session |
| POST | `/stop` | stop streaming and release capture |

Unknown routes return **HTTP 200** with the body `Unavailable route`
(`text/plain`).

## Video framing (`/frames`)

Each frame is:

```
offset  size  field
0       4     payload length, big-endian (max 2,097,152)
4       4     presentation timestamp, big-endian, 90 kHz units
8       N     Annex-B access unit (00 00 00 01 / 00 00 01 start codes)
```

The first frame after a connection is a keyframe (the server requests one on
connect). After that keyframes come from the encoder's own schedule, after a
skip (below), or on `POST /video/keyframe`; the server does not force periodic
ones, because each large IDR is a latency spike on a slow link (Wi-Fi). The PC
recorder requests one every 2 s while it records. Timestamps are 32-bit and
wrap; the dashboard unwraps the delta.

Ring: 64 slots. When a viewer falls more than 6 frames behind the server drops
to the newest available IDR and requests another keyframe. Streaming clients
are capped at 8; the 9th gets `HTTP/1.1 503 Busy` with an empty body.

## Audio framing (`/audio`)

```
offset  size  field
0       4     payload length, big-endian (max 65,536)
4       4     timestamp, big-endian (console clock)
8       N     PCM samples
```

Codecs advertised in `config.json`:

- `pcm_f32le` — 32-bit float, interleaved
- `pcm_s16le` — signed 16-bit
- `pcm_s32le` — signed 32-bit

Default console stream: `pcm_f32le`, 2 channels, 48000 Hz. The capture layer
emits one packet per audio frame batch, typically 8192 bytes (1024 stereo
float samples).

## `config.json`

```json
{
  "platform": "PS5",
  "width": 1920,
  "height": 1080,
  "fps": 60,
  "codec": "avc1.4d4032",
  "sps": "674d403291a01e0089f97016a020202800001f48000ea6044d0f1f50",
  "pps": "68ee3c80",
  "audio": {
    "codec": "pcm_f32le",
    "channels": 2,
    "rate": 48000,
    "bits": 32
  },
  "resolutions": [1080],
  "requested_height": 1080,
  "profile_changing": false,
  "profile_error": false
}
```

- `codec` is `avc1.%02x%02x%02x` from `sps[1..3]` (profile, constraints, level).
- `sps`/`pps` are lowercase hex without start codes.
- `audio` is `null` when no audio channel is open.
- Glint captures 1080p60 only (capture feature 12). On current firmware
  1440p `OpenVideo` returns `0x81950008` (profile unsupported), and a failed
  1440p attempt can leave the encoder in a state where `Start` returns
  `0x80c78000`, so the payload opens 1080p directly without probing. See
  `docs/CAPTURE-INTERNALS.md` for the full error-code table.

## `status.json`

```json
{
  "version": "1.2.1",
  "backend": "h264",
  "width": 1920,
  "height": 1080,
  "frames": 648,
  "bytes": 1354059,
  "fps": 60.3,
  "kbps": 1008.8,
  "clients": 1,
  "viewers": 1,
  "bitrate": 50000000,
  "recording": false,
  "recording_name": "",
  "recording_frames": 0,
  "recording_bytes": 0,
  "recording_seconds": 0.0,
  "recording_error": false,
  "audio_chunks": 508,
  "audio_bytes": 4161536,
  "audio_kbps": 3108.5,
  "audio_codec": "pcm_f32le"
}
```

`fps` and `kbps` are frames and bytes since the payload started divided by the
time since the server last (re)started, so they overcount after an encoder
reopen. The app shows its own rate over the last 2 s instead.
`clients` counts every open request socket (including status polling).
`viewers` is how many people are watching: 1 while the current viewer has a
stream open, else 0 (see [One viewer at a time](#one-viewer-at-a-time)).

## Recording

- Directory: `/data/glint/recordings` (created on demand).
- Trigger: the first IDR after `POST /record/start`.
- File name: `glint-<unix-time>-<frame-count>.h264`.
- Contents: `[BE u32 len][SPS] [BE u32 len][PPS]` followed by Annex-B access
  units. A new SPS/PPS pair is written after a discontinuity.
- `POST /record/stop` finalizes the file and fills `recording_name`,
  `recording_frames`, `recording_bytes`, `recording_seconds`.
- A recording error sets `recording_error` and closes the file.
- Listing entries: `{"name":"...","bytes":123}`.
- Download names are restricted to `[A-Za-z0-9.-]`, must end in `.h264`,
  must not start with `.`, must not contain `..`, and are at most 96 chars.

## Profile changes

`POST /capture/profile?height=H` accepts only 1080 (anything else is 400).

- `400 Unsupported resolution` — bad height or trailing characters.
- `409 Stop recording before changing resolution`.
- `409 A resolution change is already in progress`.
- `200 Resolution unchanged` — same as current.
- `200 Resolution change requested` — capture loop reopens avcap2 and bumps
  the stream generation, which disconnects current viewers.

Response body shape: `{"message":"..."}` over `HTTP/1.1 <code> Profile`.

## RTP

`GET /rtp/video?port=N` (accepted range 1024–65535):

1. Sends `HTTP/1.1 200 OK ... RTP\n`.
2. Opens a UDP socket to the caller's address with the requested port.
3. Packetizes Annex-B NAL units as RTP/AVC (payload type 96, 90 kHz clock).
4. SPS and PPS are prepended to every keyframe, each with a 4-byte start code.
5. NALs larger than 1400 bytes use FU-A fragmentation; the marker bit is set on
   the last packet of each access unit.

## Limits and concurrency

| Resource | Value |
|---|---|
| Client slots | 16 |
| Streaming slots (`/frames`, `/audio`, `/rtp`) | 8 |
| Video ring | 64 entries |
| Audio ring | 256 entries |
| Request header | 4095 bytes |
| Header deadline | 5 s |
| Send timeout | 5 s |
| Receive timeout | 1 s |
| Max video frame | 2 MiB |
| Max audio packet | 64 KiB |

## One viewer at a time

`/frames`, `/audio` and `/rtp/video` belong to one viewer at a time. A viewer
is named by `?viewer=<id>` (letters, digits, `-`, `_`; the dashboard makes a
new one per page) or, without it, by the client's IP address (the desktop app).

- A different viewer connecting takes over: every stream of the current viewer
  ends.
- A viewer that was pushed out is refused with **HTTP 409** while the one that
  pushed it is still watching (or reconnecting within 5 s), so two clients that
  reconnect on their own can't take turns kicking each other.
- `&take=1` takes over anyway (the dashboard's **Watch here** button).
- Once the current viewer has been gone for 5 s, anyone can connect again.

## Bitrate

The encoder runs at **50 Mbit/s** (`bitrate` in `status.json`; a `bitrate=N`
start argument changes it). In a busy game that comes out at about 35-38
Mbit/s; a calm home screen needs far less. Measured over Wi-Fi (about 40 s
of GTA each): 20 Mbit/s looked blocky in fast motion; 50 was as smooth as 20
on a clean stretch, and hitched more only when the Wi-Fi itself was choppy.

It is set once, before `Start`, and can't change while Glint runs (tested on a
PS5, 13.60):

- `SetVideoBitrate` on a running session returns ok, but every `ReadVideo`
  after it fails with 0x80c78000 and the encoder stays locked until a restart.
- Closing the session cleanly (Stop, Close, arbitrator cleared) and opening a
  new one gets through `OpenVideo` and `SetVideoBitrate`, then `Start` is
  refused with 0x80c78000. The encoder takes one capture session per boot.
- Every frame is a reference frame (`nal_ref_idc` 2), so a cap after the
  encoder can't drop frames without breaking the picture.
