# Mock server for the taxi meter

A test server for your own PC (Windows, Linux, macOS). It answers like the real server, keeps everything in a
database, checks every record the meter sends (also the fare), registers drivers, serves firmware for OTA, sets the
meter's clock, and can break things on purpose: HTTP errors, cut connections, broken JSON, other TLS setups.
Admin page with charts, dark mode, works on a phone. **For testing only.**

## Start on Windows

1. Install Python **3.11 or newer** from <https://www.python.org/downloads/> (tick *py launcher* and *Add to PATH*).
2. PowerShell in this folder (in VS Code: *Terminal > Run Task > Mock server: start (Windows)*):

```powershell
.\open-firewall.ps1 -Port 8443          # once, PowerShell as Administrator
.\start.ps1 -Ip 203.0.113.7 -Port 8443  # your static IP and the open TCP port
```

Notes for Windows:
- The `.\` before the script name is required. Without it PowerShell does not find the script.
- If you see *running scripts is disabled on this system*, run this in the same window first (it lasts until you close the window):
  `Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass`
- If the package install times out (`ReadTimeoutError ... pypi.org`), install by hand with a longer timeout, then start again:
  `.\.venv\Scripts\python.exe -m pip install --timeout 120 --retries 10 -r requirements.txt`
  If it keeps failing, try another network (for example a phone hotspot).

The first start installs the packages (`fastapi`, `cryptography`) and makes the certificates. It prints the admin page
address, the admin token and the build flags for the meter. Open **https://127.0.0.1:8443/admin** (accept the
certificate warning: the certificate is made by the server itself).

Linux / macOS: `pip install -r requirements.txt` then `python3 server.py --ip <ip> --port 8443`.

## Build the meter for this server

```
west build -b esp32_devkitc/esp32/procpu zephyr-app -- -DCONFIG_TAXI_SERVER_HOST=\"203.0.113.7\" -DCONFIG_TAXI_SERVER_PORT=8443
```

Add `-DCONFIG_TAXI_NTP_HOST=\"<pc ip>\"` to use the server's clock (start it with `-Ntp`). The normal build still talks
to `node.raxitaxi.ir:443`.

## No board yet? Fake meters

```
python simulate_meter.py --port 8443 --meters 3 --trips 8 --bad 1 --dup 1
```

downloads the tariff like a meter and sends trips (one wrong fare and one duplicate per meter on purpose), so every page
has data.

## Pages

| Page | What it does |
|---|---|
| Dashboard | Meters online, calls per minute (chart), calls by type, trips and income per day, latest problems |
| Drivers | Register drivers: name, phone, plate, car, device serial, license end date, tariff profile, own prices, own service numbers, own WiFi, on/off. Shows exactly what the meter gets when it connects. What a meter **without** a driver gets: main tariff, 403 or 404 |
| Devices | Every meter that called: online, IP, TLS, calls, trips, problems; per-device price override or "expired"; **Register** button |
| Trips | All trips with driver, distance, time, waiting, fare, the **expected** fare range and the result of the fare check; duplicates marked; CSV |
| Live log | Every call (live), filters, red = record with an error, yellow = note; details of each call; CSV |
| Tariff | Main prices as a form or JSON, service numbers, WiFi, and **tariff profiles** (e.g. airport, city) for drivers |
| Trouble makers | What the server does wrong on purpose (see below). A yellow bar on every page shows what is switched on |
| Firmware (OTA) | Drop `zephyr.signed.bin`, the version is read from the image; offer it in the tariff |
| Time (NTP) | What time the meter gets. One click: 21:58 (night soon), 05:58, 23:59 or any time; shift by minutes |
| TLS | 7 server setups (RSA, EC, TLS 1.2 only, TLS 1.3 only, strict modern, long certificate chain, expired), what the meter agreed on, failed handshakes |
| Settings | Build flags, admin link with token, "only answer these meters", CSV export, clear the log |

## How the meter gets its settings

When a meter asks for the tariff, the server looks up the driver by the serial (the meter's MAC, on its INFO page) and
builds the answer: **main tariff -> the driver's tariff profile -> the driver's own prices -> device override**, plus
the driver's service numbers and WiFi (or the main ones). A switched-off driver, an ended license or a device marked
expired gets **HTTP 403**, so the meter shows `E-403`. A meter without a driver gets what you choose on the Drivers page.

## Trouble makers

| Group | Choices |
|---|---|
| Tariff answer | ok, 403 expired, 500, 502 HTML, 302 redirect, cut JSON, numbers as text / true / 1318.5, wrapped JSON, 15 services, negative price, huge number, PM 0, empty |
| Record answer | 200, 301, 500, 400 |
| Transport (all calls, or only tariff / records / firmware) | normal, **chunked**, no length (end = closed connection), slow, **half answer then cut**, **closed without answer**, **TCP reset**, **stalled** |
| Firmware download | ok, cut at 50 %, changed bytes, wrong first byte, 302, 404 |
| Slow / unreliable | delay every answer 0..30 s, fail N % of the calls with 503 |

## What the server checks in every record

- Fields present, numbers are integers, times are `HH:MM`, channel 1..5.
- `total_price = trip_price + traffic_price`, `night_price <= trip_price`, average speed = distance / time.
- **Fare check**: recomputes the fare from the tariff this meter really downloaded (driver, profile, night rates,
  free distance and time, rounding, tariff 3). Tested against 3000 random trips of the meter's own fare engine with no
  false alarm; it catches most errors above about 5 %.
- **Duplicates** (the same trip arrives twice) and **order** (a trip end without a start, a second start on a channel).

## Data

`data/taxi.db` (SQLite) holds everything: settings, drivers, tariff profiles, devices, every call, trips, served
tariffs, NTP answers. It survives a restart. `data/tls/` holds the certificates, `data/firmware/` the uploaded images.
All of `data/` is in `.gitignore`.

## Security

- The meter calls are open to anybody who can reach the port (like the real server). Run the server only while you test,
  then stop it (`Ctrl+C`) and `.\open-firewall.ps1 -Remove`.
- The admin page and its API answer only `127.0.0.1` or someone with the admin token (printed at start, kept in the database).
- Everything that comes from the meter or the internet is escaped before it is shown on the admin page.
- A browser that rejects the self-signed certificate makes failed TLS connections; the server ignores those alerts
  so they do not look like meter problems.

## Tests

```
python -m unittest discover -s tests -v     # 19 tests, needs gcc (tariff JSON is also fed to the meter's C reader)
```

They start the real server and cover: tariff and record answers, record and fare checks, duplicates and order, all
transport faults, firmware scenarios, the 7 TLS profiles, drivers and profiles, unregistered meters, NTP time shift,
restart with the same database, admin protection, CSV.

Not tested: the `.ps1` scripts (written without a Windows PC) and a real meter against this server.
