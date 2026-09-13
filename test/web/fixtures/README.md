# Public Image Fixtures

`reference.png` is a deterministic 1280x960 chart with neutral, pale warm,
saturated and cool regions plus exact black/white edges. `qr.png` is an opaque
400x300 black/white frame containing the payload `MiaooAim regression fixture`.
`invalid.png` intentionally contains plain text, to exercise decode failures.
These are project-generated test data, not user photos or measured panel colors.

The PNG files are checked in, so normal tests need no image-generation packages.
To regenerate, create a 232px QR with `qrcode@1.5.4`, then compile `generate.c`
with the repository's `main/lodepng.c`. Its three arguments are the output chart,
input QR PNG and output framed QR. QR generation uses the tool's default margin.
Example from the repository root on Linux/WSL:

```sh
mkdir -p output/fixtures
npx --yes --package qrcode@1.5.4 qrcode -o output/fixtures/qr-source.png -w 232 "MiaooAim regression fixture"
cc -Imain test/web/fixtures/generate.c main/lodepng.c -o output/fixtures/generate
output/fixtures/generate test/web/fixtures/reference.png output/fixtures/qr-source.png test/web/fixtures/qr.png
```

The project-generated image data may be used without restriction (CC0-1.0).
The generator source follows the repository license; LodePNG and qrcode retain
their own licenses. No font or personal photograph is embedded in these fixtures.
