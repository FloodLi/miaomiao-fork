# HTTP Receive Host Tests

Compile the real `main/http_receive.c` with fake ESP-IDF time, receive, and socket
operations. No firmware build, IDF runtime, hardware, or network is needed.

Run both the helper and production-handler tests from the repository root on
Linux/WSL with Python 3 and a host C compiler:

```sh
python3 test/http_receive/run_tests.py
python3 test/http_receive/run_tests.py --sanitize
```

The runner extracts complete current definitions of the OTA, gallery upload,
canvas image upload, canvas layout, and gallery upload-state functions into a
temporary fixture. Function bodies are preserved, with source `#line` mappings;
there is no checked-in duplicate of their behavior. API stubs supply fake time,
socket receives, OTA calls, display calls, and mutexes. The real receive helper
and file promotion helper are compiled alongside them. `/spiffs` paths map to
an isolated host temporary directory. The filesystem shim rejects renaming over
an existing destination, matching SPIFFS. No root privileges are required.

To run only the helper suite:

```sh
cc -std=c11 -Wall -Wextra -Werror -Itest/http_receive/stubs -Imain \
  main/http_receive.c test/http_receive/test_http_receive.c \
  -o /tmp/msp-http-receive-test
/tmp/msp-http-receive-test
```

Coverage: 30-second inactivity, 60-second config and 300-second upload totals
under trickled progress, progress resets, exact deadline boundaries, fractional
last waits, expired calls, EOF and transport errors, socket-option failures,
keep-alive timeout restoration, terminal-state latching, shutdown before httpd
purge, empty/complete/partial drains, staging and backup rollback/preservation.
Filesystem tests use an isolated temporary directory and clean it on success.

Image, canvas layout, and OTA uploads all use the 300-second total deadline.
Ordinary configuration bodies and drains use 60 seconds. Both classes retain
the same 30-second no-progress limit.

Handler coverage:

- OTA idle/total deadlines, partial transfer stalls, EOF, receive errors, flash
  write failure, and allocation failure release the OTA handle via
  `esp_ota_abort`; no boot selection or reboot occurs. Validation failure at
  `esp_ota_end` also prevents boot selection. A complete successful transfer
  exercises boot selection and reboot scheduling as a positive control.
- Existing same-name gallery images survive receive interruption, failed upload
  writes/closes, gallery copy failure, panel incompatibility, conversion failure,
  and backup/staging-path conflicts. Upload state and display transactions are released.
- Existing same-name canvas images survive receive interruption, write/close
  failures, and backup conflicts. Successful uploads replace the file only after
  completion. Partial/stalled canvas layouts remove the temporary file and
  release the layout mutex without calling the commit API; layout trickle tests
  assert expiry at 300 seconds, while a stalled layout expires after 30 seconds.
- Every handler case checks open file and allocation counts. Failure cases check
  temporary-file removal and preservation of existing data and recovery backups.

These tests inject API/filesystem failures, not physical flash failures. Actual
lwIP FIN scheduling, ESP-IDF OTA internals, flash power loss, and EPD hardware
still require device validation. No firmware build or flash is performed.

## Recovery Paths

Gallery receive staging uses `<mount_path>/http_upload.tmp`; its backup is
`<mount_path>/http_upload.bak`, whose destination is the configured `upload_path`
(normally `/spiffs/upload.jpg`). The backup is only needed during promotion.

The named gallery image is copied to `<images_dir>/<name>.tmp`, checked for panel
compatibility, and converted before promotion. Its recovery backup is
`<images_dir>/<name>.bak`, unambiguously paired with `<images_dir>/<name>`.
Canvas images use `/spiffs/cimg/<name>.bin.tmp` and the recovery backup
`/spiffs/cimg/<name>.bin.bak`, paired with `/spiffs/cimg/<name>.bin`.

Normal receive/copy/conversion failures remove the temporary file and leave the
destination intact. A pre-existing named gallery staging file is preserved and
blocks the upload, since gallery names can themselves end in `.tmp`.
Failed promotion attempts restore the backup. If rollback
also fails, the backup remains; a pre-existing backup blocks further promotion
instead of being overwritten. To recover, stop uploads, inspect the destination
and backup, then restore the backup to its documented destination when needed.
Do not delete a backup until the destination is verified. The rename sequence
is not a power-loss transaction. Suffixes count toward SPIFFS's filename limit;
overlong staging or backup names fail without truncating the existing image.

Firmware integration: add `http_receive.c` to `main/CMakeLists.txt` SRCS. The
helper needs `esp_http_server`, `esp_timer`, and `lwip` component headers.
