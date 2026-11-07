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
| samar / reserve pages | placeholder (need network) |
| WiFi, NTP clock, tariff download, trip upload queue, settings storage | **not written yet** (stubs in `src/main.c`) |

## Threads

| Thread | Period | Job |
|---|---|---|
| `meter` | 1 s | takes pulses, updates fare of active channels |
| `keys` | 10 ms | debounce, edge flags |
| `ui` | 50 ms | page state machine, never blocks |
| `display` | 100 ms | I2C frame to the HT16K33 |
| `beep` | on demand | buzzer (software tone) |

## Fare rules (the "correct" behavior chosen with the owner)

- Distance is always charged (after the free distance).
- Waiting time is charged only when speed is below `Max_stop_km` (after the free time).
- Night % applies to start fare, distance and time (no hard-coded 10 %). Night is decided at trip start.
- Speed = pulses per second * 36 / PM (km/h). All math is integer.
- Distance on the screen is km with one decimal (`1.2`).

## Build

```
pip3 install west   # needs docopt-ng, see notes below
west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.2.0 zp
cd zp && west update hal_espressif mbedtls cmsis_6 picolibc littlefs
export ZEPHYR_BASE=$PWD/zephyr ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR=<sdk dir>
west build -b esp32_devkitc/esp32/procpu <this folder>
west flash
```

Notes: K1 is on GPIO34 (input only, no internal pull-up), it needs an external pull-up.
`pip install west` can fail building `docopt`; install `docopt-ng` and `pykwalify --no-deps` first.

## Host tests (fare engine)

```
gcc -Wall -Wextra -O1 -o /tmp/test_fare core/fare_engine.c tests/test_fare.c && /tmp/test_fare
```
