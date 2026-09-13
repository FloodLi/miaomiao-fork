# Image quantization host tests

These tests include the entire current `main/image_convert.c` in the test
translation unit and link the real `main/lodepng.c`. ESP allocation, logging,
timer, and panel selection are stubbed. JPEG ROM decoding is unavailable on
the host; its production output callbacks and band flushing are tested with
decoded MCU blocks. No quantizer implementation is copied into the tests.

## Profiles

| Exact panel | Chromatic BW threshold | Chromatic FS gain | Red gain | Yellow gain |
| --- | ---: | ---: | ---: | ---: |
| JD79665 ID34 | 104 | 192/256 | 280/256 | 288/256 |
| JD79668 ID37 | 104 | 256/256 | 256/256 | 256/256 |

Each has its own explicit profile. Both use source RGB max-minus-min <=20 for
neutral classification, threshold 128 and full 256/256 serpentine FS for neutral
tones. Target profiles retain Q4 residual precision and mirror 7/3/5/1 diffusion
on odd destination rows. Exact source black and white absorb incoming error and
remain exact. The Bayer fallback uses bin-centered thresholds `bayer8 + 2` for
neutrals, without the legacy -40 or chromatic BW offset. Color planes are exclusive.

Exact prequantized PNGs are packed before quantization with no second dither.
For these two IDs only, non-extreme neutrals no longer qualify as prequantized,
and the loose BW threshold heuristic is bypassed. Other IDs retain their old
profile and PNG heuristics; E6 retains its palette and separate FS/nearest paths.

## Run

From the repository root in Windows PowerShell, with GCC, CMake and Ninja
installed in WSL Ubuntu:

```powershell
wsl -d Ubuntu --exec sh test/image_quant/run.sh
```

Or from Linux/WSL in the repository:

```sh
sh test/image_quant/run.sh
```

The runner uses a temporary host build and enables AddressSanitizer and UBSan.
Assertions stay enabled even in a Release CMake build. This does not build
firmware or use the ESP-IDF build tree.

## Coverage

- All 256 grayscale patches: monotonic white-dot density and simulated luminance
  within 3/255 of source, including exact all-black and all-white endpoints.
- Explicit ID34/ID37 gains, 104/128 judgment, source-chroma boundary at 20/21,
  mirrored diffusion weights and undiminished neutral error gain.
- Near-neutral ramps adjacent to saturated red/yellow, sharp black/white borders,
  incoming error, stale accent bits, exclusive planes and saturated color coverage.
- Real encoded PNG memory/file conversion at 800x480 and 400x300, including gray
  extremes previously flattened by PNG heuristics, a mostly-white image with a
  tonal ramp that satisfies the old threshold-only heuristic, and exact
  prequantized planes.
- Injected FS allocation failures verify the actual PNG and BMP Bayer fallback.
- Real BMP decoding and production JPEG MCU callbacks across odd band boundaries.
- Pre-change production output fingerprints for every other numeric panel ID at
  identical 400x300 geometry/capabilities, including E6 FS and nearest output.
  The fixture combines a gray ramp, saturated red/yellow, borders and seeded RGB.

The dot-density luminance model assumes ideal black=0 and white=255; these are
software regression checks, not measured reflectance or a guarantee of panel
quality. Waveforms, voltages and physical panel testing are outside this change.

The yellow-message regression also checks the browser ink RGB `[255,232,0]`
on ID34/37/43/44 with thin stroke masks. Real encoded PNGs are decoded through
both the raw-file and `image_convert_to_epd_fb()` production paths, then all
available bit planes are compared. Framebuffer allocation is a host stub;
packing and decoding are not copied. ASan/UBSan cover cleanup on completion.

## Shared integration

This standalone CMake/CTest project is already included in
`test/run_host_tests.sh`. No firmware CMake integration is needed because
`main/image_convert.c` is already compiled by firmware.

The shared runner, main CMake, EPD/LUT/voltage, UI, web and checker files are not
modified by this work.
