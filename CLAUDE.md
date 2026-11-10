# CLAUDE.md — Taxi Meter (Taxiwifi)

Firmware for a WiFi taxi meter ("taximeter") on an ESP32. It counts wheel pulses, calculates the fare,
shows it on a 12-digit 7-segment display, reads 5 keys, and talks to a cloud server over WiFi.

Language note: code comments and some names are Persian/Finglish. See the glossary at the end.

## Build and tools

- PlatformIO, Arduino framework. Env: `esp32-devkitlipo` (`platformio.ini`).
- CPU 160 MHz, upload 921600, monitor 115200 (`esp32_exception_decoder` filter), partitions `default.csv`.
- `CORE_DEBUG_LEVEL=1`, `-ggdb3`.
- Libraries: `ArduinoJson ^7.2.1` (used for settings), `Arduino_JSON ^0.2.0` (used only in `TripData::toJsonString`, which is unused).
- Code uses the **arduino-esp32 2.x API** (`timerBegin(0, 80, true)`, `driver/pcnt.h`). The `platform` is not pinned.
  Core 3.x will not compile. Pin the platform version if you rebuild.
- Build: `pio run`. Upload: `pio run -t upload`. Monitor: `pio device monitor`.
- No tests. No CI.
- `Taxiwifi.code-workspace` expects a sibling folder `../ht16` (old HT16K33 driver work). It is not part of this repo.
- `src/Archive/` holds old `.rar/.zip` snapshots. `src/vs_BuildTools.exe` is a stray binary. Ignore them. Do not edit them.

## Zephyr rewrite (`zephyr-app/`, merged into master)

A full rewrite for **Zephyr v4.2, ESP32 DevKitC** (`esp32_devkitc/esp32/procpu`, SDK 0.17.0) lives in `zephyr-app/`.
The Arduino code in `src/` stays as the behavior reference. Do not edit `src/` for the rewrite.
Status: builds without warnings and has host tests. **Not tested on real hardware yet.**
Details, risks and the reliability table: `zephyr-app/README.md`.

### Build and test

```
# one time: zephyr-app/tools/setup.sh   (west workspace with MCUboot, SDK, WiFi radio blobs) then `source <ws>/env.sh`, `cd $ZEPHYR_BASE`
export ZEPHYR_BASE=<ws>/zephyr ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR=<sdk>
west build -b esp32_devkitc/esp32/procpu zephyr-app          # firmware
gcc -O1 -fsanitize=address,undefined -o /tmp/tf zephyr-app/core/fare_engine.c zephyr-app/tests/test_fare.c && /tmp/tf   # fare engine
gcc -O1 -fsanitize=address,undefined -o /tmp/tj zephyr-app/core/json_lite.c zephyr-app/tests/test_json.c -lm && /tmp/tj   # JSON reader + fuzz
gcc -O1 -fsanitize=address,undefined -o /tmp/tu zephyr-app/core/urlq.c zephyr-app/tests/test_urlq.c && /tmp/tu   # request builder
gcc -O1 -fsanitize=address,undefined -o /tmp/to zephyr-app/core/ota_logic.c zephyr-app/tests/test_ota.c && /tmp/to   # OTA version/header/url logic
zephyr-app/tests/native/run.sh                               # queue, flash, trip, report, tariff JSON (native_sim)
python3 zephyr-app/tests/lint_text.py zephyr-app/src/ui.c     # every UI text fits the 7-seg and is readable
BUILD_DIR=<fw build> MBEDTLS_DIR=<ws>/modules/crypto/mbedtls zephyr-app/tests/tls/check_tls.sh   # TLS suites + SNI against openssl servers
(cd zephyr-app/tools/mock-server && python3 -m unittest discover -s tests -v)   # test server (needs fastapi, cryptography, gcc)
```

Run all of them after any change. The native test runs the same binary several times on one simulated flash file
(= several power cycles). If you add a test that needs a reboot, add a new `phase` case in `tests/native/src/main.c`.

### Code map (`zephyr-app/`)

| File | Role |
|---|---|
| `core/fare_engine.c/.h` | Pure C fare math, integer only (micro-Rial). No OS calls. Host tested |
| `core/json_lite.c/.h` | Own tolerant JSON reader (no heap): any key order, nesting, `true`/`1.5`/`"46"` numbers, escapes, depth and size limits. Host tested + 300k-case fuzz |
| `core/urlq.c/.h` | Safe builder for `path?key=value` requests: percent-encoding, overflow detection |
| `core/ota_logic.c/.h` | OTA pure logic: version compare, MCUboot header check, https URL check, "download this offer?" |
| `src/main.c` | Boot order, fatal-error handler (reboots), restores a running trip |
| `src/meter.c/.h` | `meter` thread (1 s tick), channels, tariffs, live speed, export/import of trips |
| `src/pulse.c` | GPIO32 interrupt counter with a 100 us glitch filter |
| `src/keys.c` | 10 ms scan, 30 ms debounce, edge flags, hold/rest time |
| `src/display.c` | HT16K33 driver, font, digit remap, dp, WiFi LEDs; redraws at once when text changes |
| `src/ui.c` | All pages and menus (20 ms loop, never blocks) |
| `src/net.c` | `net` thread: WiFi, NTP, tariff GET, upload, checkpoint of running trip |
| `src/postq.c` | FIFO upload queue (16 records) saved in flash |
| `src/trip_store.c` | Running trip saved every 10 s, restored after power loss |
| `src/report.c` | Today total/count and last 10 trips, saved in flash |
| `src/ota.c` | OTA state machine (download into slot 1, request upgrade, reboot when idle, confirm or roll back). Compiled only when MCUboot is in the build (`CONFIG_TAXI_OTA`) |
| `src/persist.c`, `src/tariff.c` | Settings in flash. `tariff.c` reads the server JSON with `json_lite` and checks every number against a range; on any problem it refuses the **whole** update and changes nothing (a price never comes from half-understood data). A bad WiFi name/password is ignored, the prices are still applied |
| `src/clock.c`, `src/beep.c`, `src/wdog.c` | Clock (NTP + uptime), software-tone buzzer, task watchdog + reset cause |
| `boards/esp32_devkitc_procpu.overlay` | Pin map (same GPIOs as the Arduino version) |
| `tools/mock-server/` | Test server for the PC: own asyncio HTTPS server (byte control for chunked / cut / reset faults) + FastAPI admin API on one port, SQLite (`data/taxi.db`), admin UI in `web/` (plain JS, SVG charts, no CDN, dark mode, phone layout). Drivers + tariff profiles (meter gets its driver's settings by serial; ended license = 403), independent fare check (`taxi_mock/fare.py`), duplicate / order checks, 7 TLS profiles, NTP with time shift, `simulate_meter.py`. Windows: `start.ps1`, `open-firewall.ps1`. Meter side: Kconfig `TAXI_SERVER_HOST`, `TAXI_SERVER_PORT`, `TAXI_NTP_HOST`. Read its README |
| `tests/tls/` | Builds an mbedTLS client with the firmware's real settings and tries openssl servers (ECDHE-only, ECDSA cert, x25519, SNI). Found that the first TLS config could not talk to modern servers |
| `tools/setup.sh` | One-command setup of west, Zephyr workspace (+ MCUboot), WiFi blobs, SDK. Idempotent. Does not create the OTA key |
| `VERSION`, `Kconfig`, `sysbuild.conf`, `sysbuild.cmake` | App version, `TAXI_OTA` option, MCUboot settings (the MCUboot source itself is NOT in this repo: west fetches it, sysbuild builds it) |
| `tests/` | `test_fare.c` (gcc), `native/` (native_sim), `lint_text.py` |

Threads: `keys` 10 ms (prio 6), `meter` 1 s (5), `ui` 20 ms (7), `display` on change / 250 ms (8), `net` 300 ms (9), `beep` (12).
All five important threads feed the task watchdog. Flash is written only by the `net` thread, and only when no key
was used for 0.5 s (a flash write can stall the system for a few ms).

### Fare rules (decided with the owner, differ from the Arduino code)

- Distance is always charged (after the free distance). Waiting time is charged only below `Max_stop_km` (after the free time).
- Night has its own prices: `Percent_night` applies to start fare, distance and waiting (no hard-coded 10 %).
  Night is checked **every second**: rates change live if night starts or ends during a trip. If the clock was never set it is **day**.
- Speed = pulses per second * 36 / PM. Distance is shown as km with one decimal (`1.2`).
- Server fields at trip end: `traffic_price` = waiting cost, `trip_price` = total - traffic (start fare + distance),
  `night_price` = the part of `trip_price` charged at night rates (full amount), `total_price`. Real `avg_speed`, `max_speed`,
  distance, duration, waiting seconds. `traffic_distance` is still 0.

### Screens on the 12 digits (big field 8 digits, small field 4 digits)

A `.` in a text lights the dp of the digit before it and uses no digit (`1.2` = 2 digits). Letters **K Q V W X** are not readable
on 7 segments (V looks like U, X like H): never use them. `R` is only kept in the old labels `SAMR`, `TARF`, `SR-`.
`tests/lint_text.py` checks this for every text in `ui.c`.

**Main** (big `--------`, small = clock `1305` with a blinking colon, `----` if the clock is not set)

| Key | Action |
|---|---|
| K1 | opens the 4-channel page on channel 1 (needs `korsi`). Nothing starts yet |
| K2 | pulse counter page |
| K3 | press: small field clock <-> speed. Hold 10 s: reset WiFi to `Rad` and reboot |
| K4 | single meter (needs `darbast`) |
| K5 | hold 3 s then release: MENU. Hold 5 s: OFF |

Errors (big field, with one long low beep): `E-403` = license expired (stays on the main page until the server accepts again),
`E-001` = that meter mode is switched off in the tariff. Menu and OFF still work when expired.

**MENU** has two pages. K1 switches the page at once (small shows `P-1` / `P-2`). For the other keys: press once = name, press again = enter. K5 = back.

| Page | K2 | K3 | K4 |
|---|---|---|---|
| 1 | `SAMR` service numbers | `NOBT` queue (looks for AP `Usefi`) | `TARF` reload tariff |
| 2 | `INFO` | `rEPT` report | `CAL ` PM measure |

**INFO** (K2 next, K3 previous, K5 back): `NET  ON`/`NET OFF`, `HOST UP`/`HOST BAD`/`HOST ---` (last HTTPS result),
`PEND  n` (records waiting to be sent), serial (first 8 hex chars big + last 4 small), `SOFT 0.1`, `TIME SET`/`TIME ---`. Small field shows `n-6`.

**REPORT** (K2 next, K3 previous, K5 back): total of today in Toman (small `SUM `), number of trips today (`CNT `),
then the last 10 trips newest first (big = fare in Toman, small = start time `HHMM`). "Today" = local calendar day, needs the clock.

**CAL**: drive exactly 100 m. K2 start, K2 stop. Shows `PM` = pulses / 10. K3 switches to the PM of the server tariff (`PS`). K4 = again.
It only **measures**: the tariff PM comes from the server, so tell the office the number.

**Trip screens**: single meter (K2 cycles clock / speed / km / waiting minutes, K3 pause, K4 twice = end),
4 channels, SAMR, NOBT, pulse page, OFF.

**4 channels** (each passenger has his own fare, tariff 2). The page shows one channel; K1..K4 pick it:
another channel than the shown one = only switch (no blink, no start); the shown channel idle = start (base fare 2 s, then the running fare);
the shown channel active = blink 4 s, press again while blinking = end the trip. An idle channel shows its number (`1-  ----`).
No running channel: the page closes after 10 s without a key, or with K5.

**SAMR** (school bus: each service number = one child): K1 next, K2 previous, K3 toggles the shown number
(1st press `start=1` = child called, 2nd press `start=2` = child arrived). Many children can be on the way at once;
small field `A  n` = how many. The state stays when you leave the page (it is lost at a reboot). Hold K5 3 s: menu.

**Tariff 3** (long trips): single meter only. After `trip3` pulses the trip is charged with `Cost3_200` / `Cost3_stop`
(distance and waiting). Not used in the 4-channel mode (same as the Arduino code).

### OTA (firmware update) - read `zephyr-app/README.md` before touching it

- Needs a sysbuild build with MCUboot, swap mode, ECDSA P-256 signature: `west build --sysbuild ... -DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=\"<your key>\"`.
  Without MCUboot the app still builds and `ota.c` is a no-op (INFO shows `UPD  OFF`). `sysbuild.cmake` refuses to build without your own key.
- The tariff JSON carries `fw_version` + `fw_url` (https). Download only when idle, stop at once on a key press, MCUboot checks the signature,
  the new image confirms itself after reaching the server for 2 min, otherwise MCUboot swaps the old one back and that version is blocked.
- The version number lives in `zephyr-app/VERSION` only (image header, INFO page, comparison).
- **Never commit a private key** (`.gitignore` has `*.pem`). The swap/rollback is **not tested on hardware**.

### Reliability (all in `zephyr-app/README.md`, summary)

- Upload queue is a FIFO in flash: oldest first, back-off 5 s ... 2 min, survives power loss. Full queue drops old start/service records, never trip ends.
- A running trip is saved every 10 s and resumes after a power loss (time without power is not counted). An ended trip is cleared at once.
- Task watchdog + hardware watchdog + reboot on fatal error. Reset cause and stack use of every thread are in the log.
- TLS: ECDHE + AES-GCM + SNI are enabled in `prj.conf` on purpose (the default mbedTLS config of Zephyr only has static RSA and no SNI, which modern servers refuse). Do not remove them. TLS 1.2 only.
  The TLS record buffer is 16 KB (`CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN`): with 8 KB a server with a ~10 KB certificate chain failed. Heap peak then ~46 KB of the 56 KB TLS heap (measured by `tests/tls/check_tls.sh`, which uses the firmware's own fixed heap).
- Known open points: no TLS certificate check, a duplicate upload is possible after a power loss in a few-ms window,
  DRAM is 85 % used (stacks and net buffers are in DRAM1), key-to-screen latency (expected 60-80 ms) is not measured on hardware.

## Source map (`src/`)

| File | Role |
|---|---|
| `main.cpp/.h` | Globals, `setup()`, `loop()`, the 1 ms hardware timer ISR (`onTimer`), fare math, `init_ch`, `night_proc`, `beep` |
| `Odometer.cpp/.h` | PCNT (pulse counter) setup and read |
| `Key.cpp/.h` | 5 keys: scan, edge flags, hold time |
| `Display.cpp/.h` | HT16K33 driver, 7-seg font, digit/segment remap, LED indicators |
| `Menu.cpp/.h` | All pages (state machine) and key handling |
| `Network.cpp/.h` | WiFi, NTP, HTTP GET tariff, HTTP POST queue |
| `TSetting.h` | Tariff and device settings, JSON load/save to SPIFFS |
| `TripData.h` | Trip record, query-string builder |
| `Sync/Sync.cpp/.h` | 5 software timeouts (1 ms tick) |
| `TachoData.cpp` | Unused class (dead code) |
| `led_matrices_LEDBP056_sch.png` | Adafruit 0.56" HT16K33 QT backpack schematic (reference only, see Display notes) |
| `output.png` | Plot of an LC filter (10 µH, 1 µF, 100 µF), resonance near 5 kHz. Hardware analysis, not used by code |

## Hardware and pin map (ESP32)

| Function | GPIO | Notes |
|---|---|---|
| Pulse input (speed sensor) | **32** | `INPUT_PULLUP`; also the PCNT pulse pin. Defined twice (`main.cpp` `pulse_inputPin`, `Odometer.cpp` `pulse_gpio`). |
| PCNT control pin | 5 | Set as `ctrl_gpio_num`, but both control modes are `KEEP`, so it has no effect. GPIO5 is a boot strapping pin. |
| Display backlight output | **2** | `Bk_OutputPin`, driven LOW at boot. GPIO2 is a strapping pin. |
| Buzzer | **4** | `tone()` / `noTone()`, blocking `delay()` |
| Key K1 | **34** | Active LOW. GPIO34 is **input-only and has no internal pull-up**. `INPUT_PULLUP` does nothing here. It needs an external pull-up. |
| Key K2 | 16 | Active LOW, internal pull-up |
| Key K3 | 27 | Active LOW, internal pull-up |
| Key K4 | 26 | Active LOW, internal pull-up |
| Key K5 | 25 | Active LOW, internal pull-up |
| I2C SDA | **21** | `Wire.begin(21, 22, 100000)` |
| I2C SCL | **22** | 100 kHz |

I2C device: one **HT16K33** LED driver at address `0x70`.

### Pulse counting (`Odometer.cpp`)

- PCNT unit 0, channel 0. Counts rising edges only (`pos_mode = INC`, `neg_mode = DIS`). Limits ±32767.
- Glitch filter value is `100`. The comment says "100 ms", but the unit is APB clock cycles (80 MHz), so it is about 1.25 µs.
- `pcnt_get_counter()` pauses, reads, clears, and resumes the counter. It is called once per second from the ISR.
- `pcnt_init()` in `main.cpp` is declared but never used (`Odometer_Init()` is the real one).

### Timer

- Hardware timer 0, prescaler 80, alarm 1000 µs, so `onTimer()` runs every **1 ms** (the comment says "1 second", which is wrong).
- Everything time-based hangs on this ISR: `Sync_hdlr` (timeouts), 500 ms blink, the 1 s fare tick, `Menu_Sync_1ms` (blink / `Is_Blink`), `Key_Sync` (hold and rest time in ms).

## Display (`Display.cpp`)

- 12 digits = `DispStr1[8]` (main number, left) + `DispStr2[4]` (small field, usually the clock, right).
  Write them with `sprintf` in the page functions. `Disp_Main_Str[12]` is the joined copy.
- Font: `SevenSegmentASCII[96]`, index = char − 32. Bit 0..6 = segments a..g, bit 7 = dp.
- Clock colon: `Disp_Data[9]` bit 7 (dp of the 2nd char of `DispStr2`). It blinks with `Sec_tick` when `ShowClock` is true.
- `Display_Proc()` is a **state machine** that does one small step per `loop()` pass (states 1..8: copy strings, convert, remap, set LEDs, I2C begin / write / end).
  A new frame starts every **300 ms**. This keeps `loop()` short. Do not add blocking code in it.
- HT16K33 init commands: `0x21` oscillator on, `0x81` display on / no blink, `0xE0` brightness level 0 (lowest, range `0xE0..0xEF`).
- RAM write: start address `0x00`, 16 bytes.

### Digit remap (`Seg_Transform`)

Logical index `s[0..11]` (0..7 = `DispStr1`, 8..11 = `DispStr2`) goes to physical position `temp[]`:

```
temp[0..4]  = s[7], s[8], s[9], s[10], s[11]     -> lower group (5 digits)
temp[5..11] = s[3], s[2], s[0], s[1], s[4], s[5], s[6]   -> upper group (7 digits)
temp[12]    = s[12]   (LED slot, not used: s has only 12 entries)
```

### Buffer layout (`map7SegToBuffer`)

- For each segment `n` (0..7, a..dp): byte `2n` = lower group `<< 3` (digits on ROW3..ROW7), byte `2n+1` = upper group (digits on ROW8..ROW14).
- So **segments are on COM0..COM7 and digits are on ROW pins**.
  This is the opposite of the Adafruit reference schematic in the repo (there, COM = digit, ROW = segment).
  The board is custom. Trust the code, not the PNG.
- Status LEDs live on ROW15 (bit 7 of the high byte): `HT16k33_buffer[1] bit7` and `[3] bit7` (COM0 and COM1).
  - WiFi connected: `[1]` on, `[3]` off.
  - WiFi not connected: `[1]` off, `[3]` blinks with `blink_500ms` (inverted: ON when `blink_500ms` is 0).

## Keys (`Key.cpp`)

- `scankey()` runs every 50 ms from `loop()`. It reads K1..K5 (active LOW).
- **Only one key at a time**: if several are down, the highest number wins.
- Per key flags in `KeyB.Kx`: `pressed` (one scan, on press edge), `still_pressed`, `released` (one scan, on release edge).
- `KeyB.Hold_Time`: ms the key has been held (counted in `Key_Sync`, 1 ms ISR). `KeyB.RestTime`: ms with no key.
- `Clear_Key()` wipes all flags. Pages call it before blocking `delay()` calls.
- No hardware debounce in the code. Debounce is just the 50 ms scan.

## Pages and menus (`Menu.cpp`)

`Active_Page` (enum `Page_e` in `Menu.h`): `main_bm=0, rep_bm, speed_bm, pulse_bm, menu_bm, fareCh1_bm, fareCh4_bm, samar_bm, reserve_bm, off_bm`.
`Menu_Proc()` (50 ms) dispatches to the page function. While `Net_Busy`, it shows `  Busy  ` + clock and skips the page.

### Main page (`main_page`) — shows `--------` and the clock

| Key | Action |
|---|---|
| K1 | Start **4-channel** meter (`fareCh4_bm`), only if `Korsi` setting is on (shared-ride "korsi") |
| K2 | Pulse counter page (`pulse_bm`) |
| K3 | Nothing on press. **Hold 10 s**: reset WiFi to `Rad` / `123456789`, save, restart ESP |
| K4 | Start **single** meter (`fareCh1_bm`), only if `Darbast` setting is on |
| K5 | Hold 3 s: beep, then **release** opens MENU. Hold 5 s: beep, goes to OFF page |

All starts need `Authorized` (set to 0 when the server returns HTTP 403 = subscription expired).

### MENU page — shows `MENU    `

Press a key once to see its name, press again to enter.

| Key | Label | Action |
|---|---|---|
| K1 | `----` | Back to main |
| K2 | `SAMR` | Samar (service) page, if `Samar` setting is on |
| K3 | `NOBT` | Reserve ("nobat", queue) page, if `Nobat` setting is on |
| K4 | `TARF` | Reload tariff from server (`get_SettingData(1)`, double beep), then main |
| K5 | | Back to main |

### Single meter (`farech1_page`)

- First 2 s (`Timeout 1`): shows base fare (`pri_cost / 10`).
- Display: fare = `Final_Cost[0] / 10`. Right field cycles with **K2**: clock, speed, km, stop minutes.
- **K3**: pause / resume (shows `  STOP  `, `IsPause`).
- **K4** release: starts a 4 s blink (`Zero_counter = Blink_C`). K4 release again during blink: **end trip**, `Collect_Trip_Data(5)`, `Send_Trip_Data()`, back to main.

### 4-channel meter (`farech4_page`)

- Channels 1..4 = keys **K1..K4**. Each channel has its own `Final_Cost[ch]`, `IsChx_Start`.
- Release key on idle channel: start it (shows base fare, `delay(500)`, `init_ch`, `Start_Trip(n)`).
- Release key on the shown channel while blinking: end it (`Collect_Trip_Data(n)`, `Send_Trip_Data()`).
- Release of any of K1..K4 starts the blink. When all channels are idle, back to main.
- Base fare in this mode is `pri2_cost`, with `Cost2_*` tariff (also set by `init_ch(ch, 1)`).

### Other pages

- **Samar** (`samar_bm`): K1 next / K2 previous service number, K3 select (POST `start=1`), K5 after select (POST `start=2`), hold K5 3 s = back to MENU. Service numbers come from the tariff JSON (`service[10]`).
- **Reserve** (`reserve_bm`): scans WiFi each pass for SSID `Usefi`. If found shows `211  ---`. Hold K5 3 s = back to MENU.
- **Pulse** (`pulse_bm`): shows `Tpulse` (total pulses since entering). K2 clears, K5 exits. Used for PM calibration.
- **Off** (`off_bm`): blank display. K5 press = wake to main. (Backlight pin is not switched here, the line is commented out.)
- **Speed / Report / test_page / Samar1_Page**: `speed_page` is not reachable by key, `report_page` has no way in (`K3` in main is commented out), `test_page` and `Samar1_Page` are never called.

## Fare logic (`main.cpp`)

Units: money is in **Rial** internally. The display shows `/ 10` (Toman). `PM` = pulses per 10 m.

- Per pulse cost: `x = Cost_200 / (20 * PM)` (a `Cost_200` price covers 200 m = `20 * PM` pulses).
- Per second cost (waiting): `y = Cost_stop / 60`.
- Night: if `IsNight`, `x` and `y` are raised by `Percent_night` %. `Pri_Cost` is raised by a **hard-coded 10 %**.
- `night_proc()` picks the tariff set: single = `Cost_200/Cost_stop`, 4-ch = `Cost2_*`, and `Cost3_*` when `Trip3_Active`. It runs only **when a trip starts**.
- `init_ch(ch, is4ch)`: resets sums, sets `Final_Cost[ch]` to `pri_cost` or `pri2_cost`, sets `kasri`, `IsFirst`, resets free counters.
- Every second in `onTimer`:
  - pulses = `pcnt_get_counter()`; `Calc_CostPm = pulses * Cost_1PM`; `speed = (10 / pulses) * 3.6 * vpulse`.
  - Distance cost is added to all 4 `Sum_Cost[]` when not paused **and `speed < Max_stop_km`** (see known issues).
  - Time cost `Cost_1s` is added every second when not paused. `StopSec[]` counts seconds.
  - Free distance (`free_trip * PM / 10` pulses) and free time (`free_time` s) are subtracted at the start of the trip.
- `round()` (in `Menu.cpp`): moves `fraction` Rial at a time from `Sum_Cost[i]` to `Final_Cost[i]`, so the shown fare steps in `fraction` units. Wraps to 0 above 99,999,999 (ch 0) or 999,999 (others).
- `Trip_KMeter[i]` grows by 1 every `10 * PM` pulses (in `processPulse`, which is **never called**, see issues).

## Settings (`TSetting.h`)

Stored in SPIFFS file `/mysetting.conf` as JSON (read at boot, written after every successful tariff download). Same keys come from the server.

| Key | Meaning |
|---|---|
| `PM` | Pulses per 10 m |
| `pri_cost`, `pri2_cost` | Start fare, single / 4-ch (Rial) |
| `Cost_200`, `Cost2_200`, `Cost3_200` | Price per 200 m |
| `Cost_stop`, `Cost2_stop`, `Cost3_stop` | Waiting price per minute |
| `Start_night`, `Stop_night` | Night hours. Day = `[Stop_night, Start_night)` |
| `Percent_night` | Night surcharge % |
| `Max_stop_km` | Speed limit (km/h) below which the car counts as "stopped/slow" |
| `free_trip` (m), `free_time` (s) | Free distance and time |
| `fraction` | Rounding step (Rial) |
| `trip3` | Pulse threshold to switch to tariff 3 (0 = off) |
| `taxi`, `samar`, `darbast`, `korsi`, `nobat` | Feature flags: type, services, single meter, 4-ch meter, queue |
| `wifi`, `wifiPass` | WiFi credentials (server can change them) |
| `service[10]` | Service numbers for the Samar page |

Note: `gettrip3(int)`, `getCost3_200(int)`, `getCost3_stop(int)` are really **setters** with getter names.

## Network (`Network.cpp`)

- WiFi: `WiFi.begin(settings wifi, wifiPass)`. Empty SSID falls back to `Rad` / `123456789`.
  `ssid` / `password` constants at the top of the file are only printed, not used.
- `NetWork_Proc()` runs every loop, but checks WiFi status only **every 30 s**. On `WL_CONNECTED` it:
  1. sets `WiFi_State = 1`, 2. `Get_Time()` (NTP, updates `Hours`/`Min`), 3. sets `SERIAL_NUMBER` = MAC without colons, 4. `get_SettingData(0)`.
  This is also how the clock stays correct: it is re-synced every 30 s.
- NTP: `ntp.day.ir`, GMT offset +12600 s (Iran, +03:30), no DST.
- Server: `https://node.raxitaxi.ir/api/taxi/`

| Call | Endpoint |
|---|---|
| GET tariff | `TaxiFare?driver_type=1&serial=<SERIAL>` — 200 = OK, 403 = `Authorized = 0` |
| POST trip start | `SetTaxiStart?serial=<S>&bt_number=<n>` |
| POST trip end | `SetTaxiEnd?driver_id=..&serial=..&bt_number=..&start_time=..&stop_time=..&trip_distance=..&...&total_price=..` |
| POST service | `StartService?&serial=<S>&serviceNum=<n>&start=<1\|2>` |

  All POSTs have an **empty body**; data is in the query string.
- **Send queue**: `PostDataQueue[10]` (strings). `SetToQueue()` stores the URL in the first free slot (dropped silently if full).
  `Network_Send_Proc()` sends one slot per call, in a ring, **only when no key was pressed for 3 s** (`KeyB.RestTime >= 3000`) and WiFi is up.
  HTTP 200 or 301 = sent (slot cleared). Anything else = retry later. `Net_Busy` is set during the send, which shows the `Busy` page.
- All HTTP calls are blocking, in the main loop.
- `bt_number`: 1..4 = channel, 5 = single meter.

## Trip data (`TripData.h`)

Holds the fields sent at trip end (`toQueryString()`). `Collect_Trip_Data(bt)` in `Menu.cpp` fills it.
Most fields are placeholders (distance, duration, speeds, night price are 0 or 1). Only start/stop time and the three prices are real.

## Known issues and traps (found in review)

Fix with care. Some may be hidden "features" the owner relies on. Ask before changing fare behavior.

1. **10 s block in `onTimer` never runs.** The test `currentMillis - previousMillis1000 >= 10000` comes right after `previousMillis1000` is reset, so it is always false.
   Effects: `IsNight` is never updated (always `false`), the backlight (GPIO2) never switches on. Also it sets `Sec = 10` instead of counting.
2. **`processPulse()` is never called.** So `Trip_KMeter`, `kpulse`, `vpulse`, `Trip3_Active` and per-pulse free logic do not run. `vpulse` stays 0, so `speed` is always 0 (and `10.0 / pulse` divides by zero when there are no pulses).
3. **Distance cost is added only when `speed < Max_stop_km`.** Likely inverted, or speed is always 0 because of issue 2, so cost is always added. Check against the original product spec.
4. **Cost math in the ISR**: floats, `uint32_t` truncation, and the free-distance logic (`freepulse -= pulse`) look wrong. Also it should not do this much work in an ISR.
5. `Sum_Cost1PM[]` and `Sum_Cost1s[]` are never updated, so `trip_price` and `traffic_price` sent to the server are always 0.
6. `Collect_Trip_Data` uses a **hard-coded serial and driver id** `08A6F7A6A30C` instead of `SERIAL_NUMBER`.
7. `speed_page()` and `test_page()` declare local `int Hours, Min;`, which shadow the globals and are uninitialized.
8. Blocking calls in the main loop: `delay(500)`, `beep()` (`delay`), HTTP, `WiFi.scanNetworks()` (Reserve page runs it on every pass), `Get_Time()` has an endless `while` wait.
9. `get_SettingData` returns early on a JSON error without `http.end()`.
10. Secrets are in source and git history: `Network.cpp` (WiFi passwords) and the default `Rad` / `123456789`. The reset-by-hold-K3 feature also uses it. Move to a config not in git.
11. No TLS certificate is set for the HTTPS calls (no CA pinning).
12. `StaticJsonDocument<1024>` assigned to `JsonDocument` is ArduinoJson 6 style on a v7 library (deprecated).
    The 1024-byte limit may be small for large tariff JSON.
13. `TSetting` constructor calls `SPIFFS.begin()` before `setup()` runs (global object). `EEPROM_SIZE` is unused.
14. `round()` in `Menu.cpp` shadows the libm function name.
15. `K1` on GPIO34 needs an **external** pull-up (see pin table). GPIO2 and GPIO5 are strapping pins and can affect boot if driven by external hardware.
16. `Trip_KMeter` unit: one step per `10 * PM` pulses = 100 m if `PM` is pulses per 10 m. The display code prints it as a 2-digit number.
17. Dead code: `TachoData.cpp`, `Samar1_Page`, `test_page`, `Send_Trip_Data1`, `pcnt_init`, `main.h` `#define` fare constants (commented).

## Glossary (Persian terms)

- **Darbast** (دربست): private ride, single meter. **Korsi** (کرسی): shared ride, 4 channels.
- **Samar** (سمر): service / dispatch numbers. **Nobat** (نوبت): queue / reserve.
- **Tarf** (تعرفه): tariff. **Kasri** (کسری): fraction remainder. **Pri_Cost**: start fare ("pardakht avalie").
- **PM**: pulses per 10 meters. **Rial / Toman**: 1 Toman = 10 Rial.

## Conventions

- Keep pages non-blocking. Do small work per call.
- New pages: add to the `Page_e` enum, `Menu_Proc()` switch, and a prototype at the top of `Menu.cpp`.
- Use `Start_TimeOut(i, ms)` / `IsTimeOut(i)` (5 slots, ids 0..4; 0 and 1 are used by the fare pages).
- Write display text only through `DispStr1` (8 chars) and `DispStr2` (4 chars), always padded to full length.
