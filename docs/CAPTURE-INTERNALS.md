# Capture internals

Reference for the PS5 capture backend (`payload/src/capture_ps5.c`). This
document describes the libSceAvcap2 interface the payload talks to, the data
layouts the capture loop builds and parses, and the error codes it treats
specially.

The code in `capture_ps5.c` is a small driver over this interface; everything
below is the shape of the interface itself.

## Host process

The console gates the H.264 encoder to a single system process (`SceRemotePlay`).
The payload borrows the encoder by:

1. Finding the pid of `SceRemotePlay` through `KERN_PROC_PROC` sysctl output
   (`sl_find_pid`).
2. Attaching with `PT_ATTACH` under a debugger authid (`SL_PTRACE_AUTHID`)
   plus full capability caps, restored on detach (`inj_ptrace`).
3. Running each libSceAvcap2 call on the remote stack through the trampoline
   in `inject.c` (`sl_inject_call_fast`).
4. Shuttling the H.264 and PCM buffers back through `PT_IO`.

Resolving a symbol inside the remote process uses
`kernel_dynlib_handle` + `kernel_dynlib_dlsym`, wrapped by `sl_resolve_in`.

## libSceAvcap2 call sequence

For a single capture session the payload issues the following calls, in order,
with the remote process stopped between each:

```
sceAvcap2Initialize()
sceAvcap2GetVideoOutMode(&video_out_mode)
sceResourceArbitratorClearCallback(11)       # clean previous session
sceResourceArbitratorClearCallback(12)
sceAvcap2Terminate()                         # only when a previous session
                                             # was reported stale
# per profile attempt:
sceAvcap2Initialize()
sceResourceArbitratorSetCallback(id, getpid)
sceResourceArbitratorChangeState(id, "RUNNING")
sceAvcap2EnableScanInEncode()
sceAvcap2OpenVideo(&params, &out_handle)
# audio:
sceAvcap2OpenAudio(&audio_params, &audio_handle)
malloc(0x10000)                              # audio buffer in the remote heap
sceAvcap2Start(audio_handle)
# video:
sceAvcap2SetVideoBitrate(session, &rate_block)
sceAvcap2Start(session)
sceAvcap2GetNonVclNalUnitStream(session, &blob)
sceAvcap2RequestKeyFrame(session)
# live loop:
sceAvcap2ReadVideo(session, buf, 0x100000, &meta, 0)
sceAvcap2ReadAudio(audio_handle, abuf, 0x10000, &meta, 1, 0)
# shutdown:
sceAvcap2Stop(session) / sceAvcap2Stop(audio_handle)
sceAvcap2Close(session) / sceAvcap2Close(audio_handle)
sceResourceArbitratorClearCallback(11, 12, feature)
```

The resource arbitrator callbacks (ids 11 and 12 plus the per-feature id)
are the biggest footgun: an un-cleared callback from a previous session
makes `SetCallback` fail with [`SL_SCE_SESSION_STALE`](#error-codes), which
needs a `sceAvcap2Terminate()` to recover.

## OpenVideo parameter block

`sceAvcap2OpenVideo` takes a single 0x2e0-byte struct and writes:

- the 32-bit "video" handle at `*out_handle`,
- the 64-bit session handle in the first qword of the parameter block itself.

The parameter block is a flat buffer with sub-blocks at fixed offsets. The
offsets below are the ones used by `build_video_params`:

| Offset  | Size | Field | Value for 1080p60 |
|--------:|-----:|-------|-------------------|
| `+0x08` | u32  | total block size | `0x2e0` |
| `+0x10` | u32  | header value | `0x28` |
| `+0x1c` | u32  | usage class (2 = encode) | `2` |
| `+0x20` | u32  | profile FPS selector | `4` |
| `+0x24` | u32  | FPS flag | `1` |
| `+0x30` | u32  | feature id | `12` |
| `+0xb8` | block | rate control (see below) | |
| `+0x1e8`| block | format sub-block (`{0xf8, 0x28}`) | |
| `+0x208`| u32 at `+8` | width | `0x780` (1920) |
| `+0x20c`| u32 at `+8` | resolution | `0x438` (1080) |
| `+0x218`| block | mask: 0x80 bytes of 0xff | |
| `+0x2a0`| u32 at `+8` | VideoOutMode | from `GetVideoOutMode` |
| `+0x2a4`| u32 at `+8` | audio companion flag (`2`) | |
| `+0x2b4`| u32 | enable flag | `1` |
| `+0x2bc`| u8  | mode flag | `1` |

### Rate control sub-block (`+0xb8`)

| Offset in block | Size | Field |
|---------------:|-----:|-------|
| `+0x08` | u64 | 0xe0 |
| `+0x10` | u64 | 0x20 |
| `+0x18` | u32 | bitrate |
| `+0x1c` | u32 | budget (= bitrate) |
| `+0x20` | u32 | per-frame bitrate (= `bitrate * 1001 / 60000`) |
| `+0x24` | u32 | min QP |
| `+0x28` | u32 | 0x33 |

### 1080p60 profile

Only one profile is used by the payload. Any other height sent via
`/capture/profile?height=` is rejected by the HTTP layer with HTTP 400.

| Field | Value |
|-------|-------|
| capture feature id | `12` |
| width (`+0x210`) | `0x780` (1920) |
| resolution (`+0x214`) | `0x438` (1080) |
| fps_mode (`+0x20`) | `4` |

## GetNonVclNalUnitStream blob

`sceAvcap2GetNonVclNalUnitStream` fills a buffer with the SPS and PPS NAL
units for the current session. The relevant fields:

| Offset | Size | Field |
|-------:|-----:|-------|
| `+0x10` | u32 | SPS length |
| `+0x18` | bytes | SPS (NAL header type 7) |
| `+0x98` | u32 | PPS length |
| `+0xa0` | bytes | PPS (NAL header type 8) |

A minimum size of `0xa4` bytes is required for the two length fields and the
PPS header byte to all be present. The payload polls this blob after `Start`
until the SPS is 28 bytes long and starts with a type-7 NAL header (an
asynchronously populated blob can briefly carry the previous session's SPS
during a profile change).

## Audio companion

`sceAvcap2OpenAudio` is called with a parameter block laid out starting at
`scratch+0x400`:

| Offset in audio block | Size | Field | Value |
|---------------------:|-----:|-------|-------|
| `+0x08` | u64 | block size | `0x38` |
| `+0x10` | u64 | header | `0x18` |
| `+0x1c` | u32 | format | `2` (PCM float) |
| `+0x24` | u32 | feature id | matches video feature |
| `+0x28` | u32 | channel mask | `0x10` (stereo) |
| `+0x30` | u32 | sample rate | `0x1f400` (128000, interpreted as 48 kHz) |

The default console stream is `pcm_f32le`, 2 channels, 48000 Hz, delivered
in ~1024-frame chunks of 8192 bytes.

## Error codes

The payload interprets three SCE error codes explicitly (see `enum
sl_sce_error` in `capture_ps5.c`):

| Code | Name | Where it appears | Meaning |
|------|------|------------------|---------|
| `0x81950008` | `SL_SCE_OPEN_VIDEO_UNSUPPORTED` | `sceAvcap2OpenVideo` | The requested profile is not supported on this firmware (notably 1440p). The whole `0x8195000x` range is treated as "profile is not supported" and triggers the next candidate. |
| `0x80c78000` | `SL_SCE_START_LOCKED` | `sceAvcap2Start` | The encoder is locked in a state only a reboot clears. No retries, the payload stops. |
| `0x819b0007` | `SL_SCE_SESSION_STALE` | `sceResourceArbitratorSetCallback` | A callback from a previous session is still registered. `sceAvcap2Terminate()` is called before the next attempt to clear it. |

## Ptrace trampoline

The remote call primitive (`inj_call_fast`) is a small int3 trampoline:

1. Pick a writable page inside the remote process near `rip` (`pick_scratch`).
2. Write an `int3` at `scratch + 0xf00`, mark the page `PROT_EXEC`, and
   overwrite the top of the remote stack so the function's return goes to the
   trampoline.
3. Set `rip` to the target function, set the SysV argument registers from
   the caller-supplied `args[6]`, continue the remote process.
4. Wait for `SIGTRAP` at `trap + 1`; the return value is `rax`.
5. Restore the trap byte, stack word, registers, FP state (optional) and
   page protection on the way out.

The remote process is kept stopped between calls so a stall never leaves the
system in an inconsistent state.
