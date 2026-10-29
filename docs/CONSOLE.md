# Running Glint on the console

The payload is loaded through Payload Manager on a jailbroken PS5. The console
exposes three services the examples below use:

- `ftpsrv` on `:2121` (anonymous, no login).
- Payload Manager on `:8084`, with the `X-PLDMGR-Request: 1` header.
- `elfldr` on `:9021` (not used here, included for context).

Set `PS5` to the console's IP so the examples copy and paste cleanly:

```bash
export PS5=192.168.1.10          # replace with your console's IP
```

> **Tested on:** PS5 firmware `0x12600006`. Other firmware versions in the
> same family should work; `docs/CAPTURE-INTERNALS.md` lists the SCE error
> codes the payload handles explicitly.

## 1. Build

```bash
cd payload/ps5
PS5_PAYLOAD_SDK=/root/ps5-payload-sdk make
```

Both land in `payload/build/`:

- `glint-payload.elf` — console capture (the PS5's own H.264 encoder)
- `glint-payload-test.elf` — synthetic capture, for the protocol test

## 2. Upload

Give Glint its own folder under the Payload Manager tree, and do not write
over any other payload's folder:

```bash
curl -s --ftp-create-dirs -T payload/build/glint-payload.elf \
  ftp://$PS5:2121/data/pldmgr/payloads/local/glint/Glint-1.1-ps5.elf
```

Payload Manager only accepts files under its payload tree and a name shaped like
`Name-version-ps5.elf`.

After uploading, read the file back and compare the hash before loading — an
upload interrupted by the console sleeping can leave a truncated file:

```bash
curl -s ftp://$PS5:2121/data/pldmgr/payloads/local/glint/Glint-1.1-ps5.elf -o check.elf
sha256sum payload/build/glint-payload.elf check.elf
```

## 3. Load

```bash
curl -s -X POST -H "X-PLDMGR-Request: 1" \
  "http://$PS5:8084/loadpayload:/data/pldmgr/payloads/local/glint/Glint-1.1-ps5.elf"
```

The payload shows a banner on the console: "Glint loaded", then
"Glint ready on port 7231" once capture starts. If the encoder is busy it
shows "the PS5 refused to start video capture (0x80c78000)" — load it again.
This often happens on the first try after the PS5 starts; the second works.

## 4. Validate

```bash
node tests/payload_smoke.js http://$PS5:7231
```

or record with the app's probe (built from `app/tools/probe.cpp`):

```bash
probe.exe http://$PS5:7231 --record out.h264 --audio out.pcm --seconds 5
```

Expected: config `PS5`, resolutions `[1080]`, `pcm_f32le` stereo 48 kHz;
`out.h264` starts with `00 00 00 01`; `out.pcm` is non-empty.

## 5. Logs

```bash
curl -s ftp://$PS5:2121/data/glint/capture.log | tail -40
curl -s ftp://$PS5:2121/data/glint/recordings/
```

## 6. Stop

```bash
curl -s -X POST http://$PS5:7231/stop
```

Only this. Killing the process with `process_kill` mid-capture can lock the
console's encoder (`Start 0x80c78000`) until a reboot.

## Notes

- `glint-payload-test.elf` is built with `SL_TEST_CAPTURE`: its H.264 is a
  synthetic stream for protocol testing, not a real picture.
- Recordings are written to `/data/glint/recordings` and can be removed over
  FTP with `-Q "DELE /data/glint/recordings/<name>"`.
