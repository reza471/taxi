#!/usr/bin/env python3
"""
Mock server for the taxi meter (Zephyr firmware). FOR TESTING ONLY.

  python server.py --ip 203.0.113.7 --port 8443          (Windows: .\\start.ps1 -Ip ... -Port ...)

One HTTPS port carries both the meter calls and the admin page (https://127.0.0.1:<port>/admin).
Everything is stored in data/taxi.db (SQLite). See README.md.
"""
import argparse
import asyncio
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from taxi_mock.admin import create_admin  # noqa: E402
from taxi_mock.db import DB  # noqa: E402
from taxi_mock.httpd import HttpServer  # noqa: E402
from taxi_mock.meter import Meter  # noqa: E402
from taxi_mock.ntp import NtpServer  # noqa: E402
from taxi_mock.state import State  # noqa: E402


def migrate_old_state(data: Path, state: State):
    """version 1 of this server kept its settings in data/state.json"""
    old = data / "state.json"
    if old.exists():
        try:
            saved = json.loads(old.read_text())
            for k in ("tariff", "services", "wifi", "fw", "allowed_serials", "admin_token"):
                if k in saved:
                    state.s[k] = saved[k]
            state.save()
            old.rename(data / "state.json.imported")
            print("imported settings from the old state.json")
        except Exception as e:
            print("could not import old state.json:", e)


async def run(a):
    data = Path(a.data)
    data.mkdir(parents=True, exist_ok=True)
    fw_dir = data / "firmware"
    fw_dir.mkdir(exist_ok=True)
    db = DB(data / "taxi.db")
    state = State(db)
    migrate_old_state(data, state)

    tls = None
    if not a.no_tls:
        from taxi_mock.tlsprof import TlsProfiles
        print("TLS: preparing certificates (the first start takes a few seconds) ...", flush=True)
        tls = TlsProfiles(data / "tls", ["127.0.0.1"] + ([a.ip] if a.ip else []))

    ntp = None
    if a.ntp:
        ntp = NtpServer(db, state, a.bind, a.ntp_port)
        ntp.start()

    meter = Meter(db, state, fw_dir)
    host = a.ip or "127.0.0.1"
    app = create_admin({"db": db, "state": state, "tls": tls, "ntp": ntp, "fw_dir": fw_dir, "meter": meter,
                        "web_dir": HERE / "web", "host": host, "port": a.port})
    srv = HttpServer(meter, app, db, state, tls)
    server = await asyncio.start_server(srv.handle, a.bind, a.port, limit=64 * 1024)

    scheme = "http" if a.no_tls else "https"
    print("=" * 74)
    print(f"Admin page : {scheme}://127.0.0.1:{a.port}/admin          (on this PC, no token needed)")
    print(f"             {scheme}://{host}:{a.port}/admin?token={state['admin_token']}")
    print("Build the meter for this server:  west build ... -- \\")
    print(f"   -DCONFIG_TAXI_SERVER_HOST=\\\"{host}\\\" -DCONFIG_TAXI_SERVER_PORT={a.port}" +
          (f" -DCONFIG_TAXI_NTP_HOST=\\\"{host}\\\"" if a.ntp else ""))
    print("Data       :", data / "taxi.db")
    print("For testing only: the meter calls are open to anybody who reaches the port.")
    print("=" * 74, flush=True)
    async with server:
        await server.serve_forever()


def main():
    ap = argparse.ArgumentParser(description="Mock server for the taxi meter (testing only)")
    ap.add_argument("--ip", default="", help="your static IP (goes into the certificate and the hints)")
    ap.add_argument("--port", type=int, default=8443, help="the open TCP port (default 8443)")
    ap.add_argument("--bind", default="0.0.0.0", help="address to listen on (default: all)")
    ap.add_argument("--data", default=str(HERE / "data"), help="folder for the database, certificates, firmware")
    ap.add_argument("--ntp", action="store_true", help="also answer NTP (UDP) so the meter clock can be set")
    ap.add_argument("--ntp-port", type=int, default=123, help=argparse.SUPPRESS)
    ap.add_argument("--no-tls", action="store_true", help="plain HTTP (only for curl tests; the meter needs HTTPS)")
    a = ap.parse_args()
    if sys.version_info < (3, 11):
        sys.exit("Python 3.11 or newer is needed")
    try:
        asyncio.run(run(a))
    except KeyboardInterrupt:
        print("stopped")


if __name__ == "__main__":
    main()
