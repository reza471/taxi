# Mock server for the taxi meter

A small server for **testing the meter on your own PC**. It answers like the real server, checks every record the
meter sends, serves firmware for OTA and can fake all kinds of problems with one click.
For testing only. Do not leave it running on the internet.

## What you need (Windows)

- Python 3.10 or newer from <https://www.python.org/downloads/> (tick **py launcher** and **Add to PATH**).
- Your static IP and one open **TCP** port (below: `203.0.113.7` and `8443`, use your own).
- The meter and the PC must be able to reach each other: the meter goes to `203.0.113.7:8443` over WiFi.
  If the router forwards that port to this PC, it works from anywhere; on the same network use the PC's local IP.

## Start (PowerShell, in VS Code: Terminal > Run Task > "Mock server: start (Windows)")

```powershell
cd zephyr-app\tools\mock-server
.\open-firewall.ps1 -Port 8443          # once, PowerShell as Administrator
.\start.ps1 -Ip 203.0.113.7 -Port 8443  # first start installs the packages and makes a certificate
```

It prints three things: the **admin page** address, the **admin token** and the **build flags** for the meter.
Open `https://127.0.0.1:8443/admin` in a browser (accept the certificate warning, it is self-signed).

## Build the meter for this server

```
west build -b esp32_devkitc/esp32/procpu zephyr-app -- \
    -DCONFIG_TAXI_SERVER_HOST=\"203.0.113.7\" -DCONFIG_TAXI_SERVER_PORT=8443
```

The default build still talks to `node.raxitaxi.ir:443`. For a firmware update test use `west build --sysbuild ...`
(see `zephyr-app/README.md`, OTA) and upload `build/zephyr-app/zephyr/zephyr.signed.bin` on the admin page.

NTP: the meter takes the time from `ntp.day.ir`. For a test on a local network add `-Ntp` to `start.ps1`
(PowerShell as administrator, UDP 123) and `-DCONFIG_TAXI_NTP_HOST=\"<pc ip>\"` to the build. A single open TCP port is not enough for NTP.

## What to test and what the meter must do

| On the admin page | What the meter must do |
|---|---|
| Tariff `ok` | Downloads it, shows prices, `INFO` page: `HOST UP` |
| Tariff `expired` (403) | Main page shows `E-403`, K1/K4 refuse with a low beep. Menu still works |
| Tariff `error500`, `html502`, `bad_json`, `timeout` | Keeps the old tariff, retries later, `HOST BAD` |
| Tariff `weird_types`, `wrapper`, `many_services` | **Accepts** it (numbers as text, `true/false`, wrapper object, long list) |
| Tariff `out_of_range`, `huge_number`, `pm_zero`, `empty` | **Refuses the whole tariff**, keeps the old prices |
| Change a price, press `TARF` in the menu | New price is used at the next trip |
| `korsi` or `darbast` = 0 | K1 / K4 show `E-001` |
| Records: `error500`, `bad_request`, `timeout` | Trip stays in the queue (`INFO`: `PEND n`), sent later, oldest first |
| Delay 15 s, or fail 50 % | Meter keeps working, queue drains when the server answers |
| Switch the server off, drive 3 trips, switch on | All 3 arrive in order. Pull the power in between: they still arrive |
| Firmware: upload image, tick "offer" | After 10 min of uptime and 60 s without key presses: downloads, reboots, new version runs |
| Firmware `corrupt`, `truncated`, `wrong_magic` | Update fails, old firmware keeps running, `INFO` shows `UPD FAIL` |
| Offer with a manual **older** or **same** version | Meter ignores it |

The record table shows every call. A red row means the record has a problem (sums that do not match, missing field,
wrong time format ...). For a trip: `total_price = trip_price + traffic_price` and `night_price <= trip_price`.

## Security

- The calls of the meter are open to everybody who can reach the port (like the real server). Use it only while testing,
  and stop it afterwards (`Ctrl+C`, `.\open-firewall.ps1 -Remove`).
- The **admin page and its API** are only for `127.0.0.1` or for someone who knows the token (printed at start,
  stored in `data/state.json`). The files in `data/` (token, certificate key) are in `.gitignore`.
- "Only answer these serials" on the admin page makes the server ignore every other device.
- The meter does not check the certificate (same as with the real server).

## Tests of the server itself

```
cd zephyr-app/tools/mock-server
python -m unittest discover -s tests -v        # needs gcc (it also feeds every tariff to the meter's own JSON reader)
```

The TLS settings of the meter are checked separately by `zephyr-app/tests/tls/check_tls.sh`
(also against this server: `./client 127.0.0.1 8443` from that script's output folder).

Not tested: the `.ps1` scripts (no PowerShell where this was written), and the meter itself against this server (no board).
