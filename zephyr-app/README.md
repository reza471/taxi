# Zephyr rewrite (work in progress)

Target: ESP32 DevKitC, Zephyr v4.2.0, Zephyr SDK 0.17.0 (`esp32_devkitc/esp32/procpu`).
Same pin map as the Arduino version (see `boards/esp32_devkitc_procpu.overlay`).

## Status

| Part | State |
|---|---|
| `core/fare_engine` (fare math, integer only) | done, host-tested |
| pulse counting (GPIO32 interrupt + glitch filter) | done, **not tested on hardware** |
| keys (10 ms scan, 30 ms debounce) | done, **not tested on hardware** |
| display (HT16K33 remap, dp support, WiFi LEDs) | done, **not tested on hardware** |
| pages: main, menu, single meter, 4-channel, pulse, off | done, **not tested on hardware** |
| samar / reserve pages | done, **not tested on hardware** |
| INFO, REPORT (today + last 10 trips), CAL (PM measure), error codes E-403 / E-001, speed on main page | done, host tested (report), text fit checked by `tests/lint_text.py` |
| WiFi (STA), NTP clock (ntp.day.ir, +03:30), tariff download (JSON), trip/service upload queue (10 slots) | done, **not tested** (builds only) |
| settings in flash (`storage` partition, Zephyr settings/NVS) | done, **not tested** |

## Reliability design

| Topic | What the code does | Verified |
|---|---|---|
| Power loss during a trip | Every 10 s (when no key is in use) all running channels are saved in flash with a CRC. After a restart the trip comes back on the same page with the same fare; the time without power is not counted. When a trip ends, the trip record is saved first, then the running trip is cleared at once, so an ended trip never comes back. At most ~10 s of the fare can be lost. A damaged or old-version save is ignored. | host test: save, restore, damaged data, ended trip |
| Unsent trips | FIFO queue (16 records) saved in flash. Slot is written before the meta, flash is written only when no key was used for 0.5 s. After a successful send the queue is saved at once. | host test with simulated flash, 6 power cycles, ring wrap-around |
| Order | Strictly oldest first (trip start before trip end). One failure stops the queue, retry with back-off 5 s ... 2 min. | host test |
| Queue full | The oldest start/service record is dropped first. Trip ends (they carry the money) are kept. | host test |
| Server down | Meter works fully offline. Records wait in flash for days. | design, host test |
| Duplicate send | A power loss between "server said OK" and the flash write (a few ms) can send one record twice. Ask the server owner if the API is idempotent. | design |
| Hang / crash | Task watchdog on the `keys`, `meter`, `ui`, `display`, `net` threads, plus the hardware watchdog as fallback. A fatal error reboots at once. The reset cause is logged at boot. | builds, **not tested on hardware** |
| Stack overflow | Thread analyzer prints stack use of every thread in the log every 2 min. Read it on the first long run. | builds |
| Flash wear | Tariff is written only if it changed. Queue writes are 1-2 small entries per record. | host test |
| Server JSON | Own reader `core/json_lite.c`. Accepts the formats real servers send (true/false, `1318.5`, `"46"`, null, nested, escapes). Every value is range checked, a wrong update is refused as a whole. | gcc tests + 300k fuzz cases under ASAN, native_sim tests |
| Requests to the server | `core/urlq.c` percent-encodes every value and detects overflow (a cut record is never sent). Same parameter order as the old firmware, including the `?&serial=` quirk of StartService. | gcc tests |
| Clock not set | Day rates, display shows `----`, backlight stays on. Never charges the night rate by mistake. | host test |

## Speed (key press to display)

Key scan 10 ms, debounce 30 ms, UI loop 20 ms, display redraws at once when the text changes
(I2C frame about 2 ms). Expected worst case 60-80 ms (old firmware: up to about 350 ms). **Not measured on hardware.**
A flash write can stall the system for a few ms, so it is done only when no key is in use.

## OTA (firmware update over WiFi)

**Status:** builds, the logic is host tested, the signature check was verified with `imgtool`.
**The swap/rollback itself was never run on a real ESP32. Bench test it (with a serial cable to recover) before any field use.**

How it works
1. The tariff JSON may carry two extra fields: `"fw_version":"0.2.0"` and `"fw_url":"https://host/path/taxi-0.2.0.signed.bin"`.
2. A **newer** version that did not fail before is downloaded into slot 1. Only when the meter is idle: no running trip and no key
   for 60 s. The download stops at once if the driver touches a key or starts a trip (and tries again later).
3. The image header is checked while it arrives (MCUboot magic, version equals the offer, size fits the slot).
4. `boot_request_upgrade(TEST)`, then a reboot when the meter is idle (running trips are saved first).
5. **MCUboot checks the ECDSA P-256 signature** and swaps the images (swap mode with the scratch partition).
   An unsigned or damaged file is refused and the old firmware keeps running.
6. The new firmware must reach the server (`HOST UP`) and run 2 minutes without a reset, then it confirms itself.
   Any reset before that (crash, watchdog, power loss) makes MCUboot put the **old** firmware back; that version is then blocked.
   A version is also blocked after 3 failed download attempts.

Safety facts
- TLS is not verified (see risks), but that is **not** a problem for firmware: only an image signed with your private key is accepted.
  The worst a fake server can do is waste bandwidth (a version that fails is not tried again).
- `sysbuild.cmake` refuses to build without your own key (the MCUboot project's test key is public).
- The key must stay offline and out of git (`.gitignore` blocks `*.pem`). Lose the key = no more OTA for already installed meters.
- A meter running the old non-MCUboot build can not be updated over the air once: the first install of the bootloader needs a cable.

Build and release
```
python3 <zephyr-ws>/bootloader/mcuboot/scripts/imgtool.py keygen -k taxi_ota_key.pem -t ecdsa-p256   # once, keep it safe
west build --sysbuild -b esp32_devkitc/esp32/procpu zephyr-app -- -DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=\"$PWD/taxi_ota_key.pem\"
west flash --build-dir build        # first install with a cable: bootloader + app
# release: raise the number in zephyr-app/VERSION, build again, publish
#   build/zephyr-app/zephyr/zephyr.signed.bin   on an https server, and put fw_version + fw_url in the tariff
```
The version in `zephyr-app/VERSION` is also the version in the image header and the text on the INFO page. They can not differ.

Server owner needs to know: serve the file with a plain `200` and `Content-Length` (redirects are not followed), and make the
`fw_version` in the tariff equal to the image version, otherwise the meter refuses the file. Remove the two fields to stop an update.

INFO page 7 shows the state: `UPD ---`, `UPD  42` (percent), `UPD DONE` (waiting for an idle moment to reboot),
`UPD TEST` (new firmware on test), `UPD FAIL`, `UPD  OFF` (built without MCUboot).

## Known risks (read before flashing)

- **RAM**: fixed by `CONFIG_ESP32_REGION_1_NOINIT=y` (stacks, kernel heap and net buffers go to the free DRAM1 region).
  Build now: DRAM0 83 %, DRAM1 49 %, IRAM 36 %. TLS heap is 56 KB, TLS record buffer 8 KB.
  Still check the first HTTPS call on hardware (big certificate chain).
- **No TLS certificate check** (same as the old Arduino code). See `https_request()` in `src/net.c`.
- Server JSON must use plain numbers (`1`, not `true`) and integers (no `1318.5`). Zephyr's JSON parser is strict.
- The WiFi radio blobs are downloaded with `west blobs fetch hal_espressif`. If `github.com/.../raw` is blocked,
  download them from `raw.githubusercontent.com` and check the SHA256 (see notes below).
- Buzzer uses a software tone (busy wait in a low priority thread). Replace with PWM if it sounds bad.
- Trip upload fields: `trip_price` = route price (start fare + distance), `night_price` = the part of it charged at night rates
  (full night amount, 0 for a day-only trip), `traffic_price` = waiting-time price, `total_price` = trip + traffic. `avg_speed`, `max_speed`,
  distance, duration, waiting seconds are real. `traffic_distance` is still 0 (not measured).
- If the power is cut right after a trip ends but before it is cleared (a few ms), the old trip can reappear once.
- Tariff and clock refresh every 10 min (old firmware: every 30 s). Menu "TARF" forces a reload.

## Threads

| Thread | Period | Job |
|---|---|---|
| `meter` | 1 s | takes pulses, updates fare of active channels |
| `keys` | 10 ms | debounce, edge flags |
| `ui` | 50 ms | page state machine, never blocks |
| `display` | 100 ms | I2C frame to the HT16K33 |
| `beep` | on demand | buzzer (software tone) |
| `net` | 300 ms | WiFi connect, NTP, tariff, upload queue (blocking is OK here) |

## Fare rules (the "correct" behavior chosen with the owner)

- Distance is always charged (after the free distance).
- Waiting time is charged only when speed is below `Max_stop_km` (after the free time).
- Night has its own (higher) prices: `Percent_night` applies to start fare, distance and waiting (no hard-coded 10 %).
  Night is checked **every second**, so the rates change live if night starts or ends during a trip.
  The start fare uses the night rate only if the trip started at night.
- Speed = pulses per second * 36 / PM (km/h). All math is integer.
- Distance on the screen is km with one decimal (`1.2`).
- Price split for the server: traffic = waiting cost; trip = total - traffic (route: start fare + distance);
  night = the part of the route price charged at night rates (full amount, included in trip).

## Build

```
pip3 install west   # needs docopt-ng, see notes below
west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.2.0 zp
cd zp && west update hal_espressif mbedtls cmsis_6 picolibc littlefs
export ZEPHYR_BASE=$PWD/zephyr ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR=<sdk dir>
west build -b esp32_devkitc/esp32/procpu <this folder>
west flash
```

Blobs: `west blobs fetch hal_espressif` (all chips are checked by the build, not only ESP32).

Notes: K1 is on GPIO34 (input only, no internal pull-up), it needs an external pull-up.
`pip install west` can fail building `docopt`; install `docopt-ng` and `pykwalify --no-deps` first.

## Host tests

Fare engine (plain gcc):

```
gcc -Wall -Wextra -O1 -fsanitize=address,undefined -o /tmp/test_fare core/fare_engine.c tests/test_fare.c && /tmp/test_fare
```

JSON reader (with a 300k-case fuzz run), request builder and OTA logic (plain gcc):

```
gcc -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -o /tmp/tj core/json_lite.c tests/test_json.c -lm && /tmp/tj
gcc -O1 -fsanitize=address,undefined -o /tmp/tu core/urlq.c tests/test_urlq.c && /tmp/tu
gcc -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -o /tmp/to core/ota_logic.c tests/test_ota.c && /tmp/to
```

Queue, flash persistence and tariff JSON (Zephyr `native_sim`, simulated flash, runs the binary
several times = several power cycles):

```
export ZEPHYR_BASE=<zephyr dir>
tests/native/run.sh
```
