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
| WiFi (STA), NTP clock (ntp.day.ir, +03:30), tariff download (JSON), trip/service upload queue (10 slots) | done, **not tested** (builds only) |
| settings in flash (`storage` partition, Zephyr settings/NVS) | done, **not tested** |

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

## Host tests (fare engine)

```
gcc -Wall -Wextra -O1 -o /tmp/test_fare core/fare_engine.c tests/test_fare.c && /tmp/test_fare
```
