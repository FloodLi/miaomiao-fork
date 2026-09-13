# EPD Production Driver Fault Tests

This host executable includes `main/epd_stub.c` directly, with fake ESP-IDF GPIO,
SPI, DMA allocation, NVS, mutexes, clock and framebuffer/UI dependencies. No
driver algorithm is copied into the tests. Framebuffer/UI fakes are deliberately
minimal; these tests do not validate typography or the real panel's polarity,
waveforms, electrical timing or image quality.

Build and run sequentially with GCC or Clang on Linux/WSL:

```sh
cmake -S test/epd -B /tmp/msp-epd-tests -G Ninja
cmake --build /tmp/msp-epd-tests --parallel 1
ctest --test-dir /tmp/msp-epd-tests --output-on-failure
```

AddressSanitizer and UndefinedBehaviorSanitizer are enabled by default. Run this
separately from firmware builds. In PowerShell, `./test/epd/run.ps1` executes the
same commands in the installed Ubuntu WSL distribution.

Coverage includes init retries without duplicate SPI bus/device registration,
all panel callbacks, command and DMA transfer failures, every SPI transfer and
DMA allocation in the ID30 buffer path, absent/stuck BUSY and subsequent
recovery, cache invalidation/full-init retry, consuming framebuffer ownership,
file streaming, clear/test-pattern/repair aborts, and experimental refresh tests.
The framebuffer writer of every registered panel is exercised separately from
clear/fill. UC8176, UC8179 and UC8253 partial refresh tests verify failure and
recovery through the public APIs. Epoch tests change the request epoch while
acquiring the display mutex and require zero SPI/GPIO calls, zero completion
notifications, unchanged forced-refresh state, and release of owned buffers.

Without CMake, the same suite can be compiled directly:

```sh
gcc -std=gnu11 -I test/epd/include -I main \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  test/epd/test_epd.c -o /tmp/msp-epd-fault-tests
cd /tmp && ./msp-epd-fault-tests
```

ID30 defaults to idle-high but explicit NVS idle-low and idle-high values both
remain authoritative. The tests check four-bit UC8159 pixel packing without
changing the transmitted format.
