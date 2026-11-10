#!/usr/bin/env python3
"""Pretend to be one or more taxi meters, to see the mock server work without a board.

  python simulate_meter.py --port 8443                       # 1 meter, 5 trips, all correct
  python simulate_meter.py --port 8443 --meters 3 --trips 8 --bad 2 --dup 1
  python simulate_meter.py --port 8443 --meters 3 --nobat        # 3 cars wait in the station queue

It downloads the tariff like the meter (same URL), computes each fare with the meter's rules, and sends
SetTaxiStart / SetTaxiEnd / StartService. --bad sends trips with a wrong total, --dup sends some trips twice.
--nobat: every meter joins the station queue (NobatReady), asks for its place every 2 s (NobatStatus), and when
it gets its turn takes a passenger (NobatBoard) and sends one trip with the queue id. Give the turn on the
admin page (Queue & Samar), or set "give the turn by itself".
Uses only the Python standard library.
"""
import argparse
import json
import random
import ssl
import time
import urllib.error
import urllib.request

CTX = ssl._create_unverified_context()  # like the meter: no certificate check


def call(base, method, path, timeout=15):
    req = urllib.request.Request(base + path, method=method, data=b"" if method == "POST" else None,
                                 headers={"Content-Length": "0"} if method == "POST" else {})
    try:
        with urllib.request.urlopen(req, context=CTX, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:  # cut connection, reset, timeout ...
        return 0, str(e).encode()


def tariff_numbers(t):
    if isinstance(t.get("taxiFare"), dict):
        t = t["taxiFare"]
    out = {}
    for k, v in t.items():
        try:
            out[k] = int(round(float(v))) if not isinstance(v, list) else v
        except (TypeError, ValueError):
            pass
    return out


def fare(t, bt, dist, wait):
    if bt == 5:
        pri, c200, cstop = t.get("pri_cost", 0), t.get("Cost_200", 0), t.get("Cost_stop", 0)
    else:
        pri, c200, cstop = t.get("pri2_cost", 0), t.get("Cost2_200", 0), t.get("Cost2_stop", 0)
    d = max(0, dist - t.get("free_trip", 0)) * c200 / 200
    w = max(0, wait - t.get("free_time", 0)) * cstop / 60
    frac = t.get("fraction", 1) or 1
    rial = int(d + w)
    rial -= rial % frac if frac > 1 else 0
    return pri + rial, int(w)


def main():
    ap = argparse.ArgumentParser(description="fake taxi meters for the mock server")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--meters", type=int, default=1)
    ap.add_argument("--trips", type=int, default=5, help="trips per meter")
    ap.add_argument("--bad", type=int, default=0, help="trips per meter with a wrong total")
    ap.add_argument("--dup", type=int, default=0, help="trips per meter sent twice")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--nobat", action="store_true", help="use the station queue instead of normal trips")
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    base = f"https://{a.host}:{a.port}"
    if a.nobat:
        return nobat(base, a, rnd)
    for m in range(a.meters):
        serial = "".join(rnd.choice("0123456789ABCDEF") for _ in range(12))
        st, body = call(base, "GET", f"/api/taxi/TaxiFare?driver_type=1&serial={serial}")
        print(f"meter {serial}: tariff -> HTTP {st}")
        try:
            t = tariff_numbers(json.loads(body))
        except ValueError:
            print("   tariff is not JSON, using defaults")
            t = {"PM": 46, "pri_cost": 26200, "Cost_200": 1318, "Cost_stop": 8000, "fraction": 50,
                 "pri2_cost": 5000, "Cost2_200": 384, "Cost2_stop": 334}
        bad = set(rnd.sample(range(a.trips), min(a.bad, a.trips)))
        dup = set(rnd.sample(range(a.trips), min(a.dup, a.trips)))
        for i in range(a.trips):
            bt = 5 if rnd.random() < 0.6 else rnd.randint(1, 4)
            dur = rnd.randint(300, 2400)
            dist = int(dur * rnd.uniform(3, 11))
            wait = rnd.randint(0, dur // 3)
            total, traffic = fare(t, bt, dist, wait)
            if i in bad:
                total += rnd.choice([-1, 1]) * max(2000, total // 8)
            h = rnd.randint(7, 20)
            start, stop = f"{h:02d}:{rnd.randint(0, 59):02d}", f"{h + 1:02d}:{rnd.randint(0, 59):02d}"
            call(base, "POST", f"/api/taxi/SetTaxiStart?serial={serial}&bt_number={bt}")
            if rnd.random() < 0.3:
                svc = (t.get("service") or [211])[0]
                call(base, "POST", f"/api/taxi/StartService?&serial={serial}&serviceNum={svc}&start=1")
            q = (f"/api/taxi/SetTaxiEnd?driver_id={serial}&serial={serial}&bt_number={bt}&start_time={start}"
                 f"&stop_time={stop}&trip_distance={dist}&trip_duration={dur}&traffic_distance=0"
                 f"&traffic_duration={wait}&avg_speed={dist * 36 // (dur * 10)}&max_speed={rnd.randint(50, 90)}"
                 f"&trip_price={total - traffic}&traffic_price={traffic}&night_price=0&total_price={total}")
            st, _ = call(base, "POST", q)
            print(f"   trip {i + 1}: ch {bt}, {dist / 1000:.1f} km, {total // 10:,} Toman"
                  f"{' (WRONG on purpose)' if i in bad else ''} -> HTTP {st}")
            if i in dup:
                st2, _ = call(base, "POST", q)
                print(f"   trip {i + 1}: sent again (duplicate on purpose) -> HTTP {st2}")
            time.sleep(0.05)


def nobat(base, a, rnd):
    cars = {}
    for _ in range(a.meters):
        serial = "".join(rnd.choice("0123456789ABCDEF") for _ in range(12))
        st, body = call(base, "POST", f"/api/taxi/NobatReady?serial={serial}")
        try:
            j = json.loads(body)
        except ValueError:
            print(f"car {serial}: NobatReady -> HTTP {st}, not JSON")
            continue
        cars[serial] = j
        print(f"car {serial}: in the queue, place {j.get('position')} (#{j.get('nobat_id')})")
    print("waiting for turns (give them on the admin page). Ctrl+C to stop.")
    try:
        while cars:
            time.sleep(2)
            for serial in list(cars):
                st, body = call(base, "GET", f"/api/taxi/NobatStatus?serial={serial}")
                try:
                    j = json.loads(body)
                except ValueError:
                    continue
                if j.get("state") != "turn":
                    if j.get("position") != cars[serial].get("position"):
                        print(f"car {serial}: place {j.get('position')}")
                    cars[serial] = j
                    continue
                nid = j.get("nobat_id")
                print(f"car {serial}: TURN -> passenger in (#{nid})")
                call(base, "POST", f"/api/taxi/NobatBoard?serial={serial}&nobat_id={nid}")
                call(base, "POST", f"/api/taxi/SetTaxiStart?serial={serial}&bt_number=5")
                t = tariff_numbers(json.loads(call(base, "GET", f"/api/taxi/TaxiFare?driver_type=1&serial={serial}")[1] or b"{}"))
                dur = rnd.randint(300, 1800)
                dist = int(dur * rnd.uniform(3, 11))
                wait = rnd.randint(0, dur // 3)
                total, traffic = fare(t, 5, dist, wait)
                q = (f"/api/taxi/SetTaxiEnd?driver_id={serial}&serial={serial}&bt_number=5&start_time=10:00"
                     f"&stop_time=10:30&trip_distance={dist}&trip_duration={dur}&traffic_distance=0"
                     f"&traffic_duration={wait}&avg_speed={dist * 36 // (dur * 10)}&max_speed={rnd.randint(50, 90)}"
                     f"&trip_price={total - traffic}&traffic_price={traffic}&night_price=0&total_price={total}&nobat_id={nid}")
                st, _ = call(base, "POST", q)
                print(f"car {serial}: trip {total // 10:,} Toman -> HTTP {st}")
                del cars[serial]
    except KeyboardInterrupt:
        for serial in cars:
            call(base, "POST", f"/api/taxi/NobatLeave?serial={serial}")
        print("stopped: the cars left the queue")


if __name__ == "__main__":
    main()
