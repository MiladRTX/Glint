# Third-party shader code

Used by the Picture settings (picture enhancement passes).

| File | From | License |
|---|---|---|
| `ffx_a.h`, `ffx_fsr1.h` | AMD FidelityFX Super Resolution 1, https://github.com/GPUOpen-Effects/FidelityFX-FSR (`ffx-fsr/`) | MIT, see `LICENSE-FidelityFX-FSR.txt` |
| `FSRCNNX_x2_8-0-4-1.glsl` | FSRCNNX by igv, https://github.com/igv/FSRCNN-TensorFlow release 1.1 | LGPL-3.0 (header of the file) |

The files are unchanged. `enhance_easu.comp` includes the FSR headers;
`tools/gen_fsrcnnx.js` turns the FSRCNNX mpv shader into the compute passes in
`../fsrcnnx/`.
