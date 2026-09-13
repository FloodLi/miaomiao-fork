# Display Transaction Host Tests

Run on Linux/WSL with a C11 compiler and POSIX threads:

```sh
IDF_PATH=/path/to/esp-idf sh test/startup/host/run.sh
```

On this workstation:

```powershell
wsl.exe -d Ubuntu --exec sh /mnt/c/Users/43060/Desktop/msp/test/startup/host/run.sh
```

The runner compiles the actual production `display_policy.c`, `display_mode.c`,
`message_board.c`, and `canvas_board.c`, and reuses the Unity tests in
`../main/test_startup_state.c`. It also executes `test_boards.c` against the real
message and canvas render workers. No firmware build or EPD driver is involved.

Set `CC`, `UNITY_ROOT` (Unity's `src` directory), and `CJSON_ROOT` as needed.
The default ESP-IDF path is the workstation's `/mnt/d/espidf/Espressif/frameworks/esp-idf-v5.5.4`.
Build products use a temporary directory and are removed on exit.

The stubs replace only platform services, drawing, and EPD I/O. Canvas reads use
temporary streams; no SPIFFS files are read or written. cJSON parsing is real.
RTOS tasks start deterministically when the test pumps them, allowing multiple
requests to arrive before the worker starts. A separate pthread test exercises
concurrent user and background acquisition against the production policy lock.

Coverage includes conditional boot acquisition and fallback, clock-to-slideshow
background transitions, startup/quick-refresh guards, rollback and content ownership,
pending requests, stale epoch
rejection, duplicate completion, result expiration, missing layouts, framebuffer
and layout allocation failures, invalid JSON, export/EPD errors, task creation
failure, queue replacement before worker entry, cleared queued content, busy
canvas rejection, and supersession during EPD refresh.

These tests verify transaction and result behavior, not panel timing, raster
appearance, or the complete HTTP server. Firmware integration still needs its
normal build and device checks.
