"""Answers to the meter: tariff, trip / service records, firmware. Also all record checks.

A handler returns a `Reply`. The HTTP layer (httpd.py) writes it, with the transport fault that the
admin page chose (chunked, cut connection, reset ...), then logs it in the database.
"""
import asyncio
import hashlib
import json
import random
import re
import struct
import time
from dataclasses import dataclass, field
from pathlib import Path

from . import fare
from .state import FW_SCENARIOS


@dataclass
class Req:
    method: str
    path: str
    query: dict
    headers: dict
    body: bytes
    remote: str
    tls: str = ""


@dataclass
class Reply:
    status: int = 200
    body: bytes = b""
    ctype: str = "application/json"
    headers: list = field(default_factory=list)
    transfer: str = "normal"      # normal | chunked | close_delimited | slow
    fault: str = ""               # "" | partial | drop | reset | stall
    log: dict = field(default_factory=dict)


def jreply(obj, status=200):
    return Reply(status=status, body=json.dumps(obj).encode(), ctype="application/json")


def treply(text, status, ctype="text/plain; charset=utf-8"):
    return Reply(status=status, body=text.encode(), ctype=ctype)


INT_RE = re.compile(r"^-?\d+$")
HM_RE = re.compile(r"^([01]\d|2[0-3]):[0-5]\d$")
END_FIELDS = ["driver_id", "serial", "bt_number", "start_time", "stop_time", "trip_distance", "trip_duration",
              "traffic_distance", "traffic_duration", "avg_speed", "max_speed", "trip_price", "traffic_price",
              "night_price", "total_price"]
END_NUMS = END_FIELDS[2:3] + END_FIELDS[5:]


def image_version(path: Path):
    try:
        with open(path, "rb") as f:
            h = f.read(32)
    except OSError:
        return None
    if len(h) < 32 or struct.unpack("<I", h[:4])[0] != 0x96F3B83D:
        return None
    return f"{h[20]}.{h[21]}.{struct.unpack('<H', h[22:24])[0]}", struct.unpack("<I", h[12:16])[0]


def normalize_tariff(obj):
    """turn whatever the server sent (wrapper, numbers as text ...) back into a flat dict of numbers"""
    if isinstance(obj, dict) and isinstance(obj.get("taxiFare"), dict):
        obj = obj["taxiFare"]
    out = {}
    for k, v in (obj or {}).items():
        if isinstance(v, bool):
            out[k] = int(v)
        elif isinstance(v, (int, float)):
            out[k] = int(round(v))
        elif isinstance(v, str):
            try:
                out[k] = int(round(float(v)))
            except ValueError:
                pass
    return out


class Meter:
    def __init__(self, db, state, fw_dir: Path):
        self.db, self.st, self.fw_dir = db, state, fw_dir

    # ---------------------------------------------------------------- helpers
    def resolve(self, serial):
        """Everything the server knows about this meter: driver, prices, services, WiFi, and if it may work.
        Order of the prices: main tariff -> the driver's tariff profile -> the driver's own changes -> device override."""
        t = dict(self.st["tariff"])
        services = list(self.st["services"])
        wifi = dict(self.st["wifi"])
        drv = self.db.driver_by_serial(serial)
        source = ["main tariff"]
        block = None
        if drv:
            prof = self.db.profile(drv.get("profile_id"))
            if prof:
                try:
                    t.update(json.loads(prof["tariff"] or "{}"))
                    source.append(f"profile '{prof['name']}'")
                except ValueError:
                    pass
            for col, target in (("overrides", t),):
                if drv.get(col):
                    try:
                        target.update(json.loads(drv[col]))
                        source.append("driver's own prices")
                    except ValueError:
                        pass
            if drv.get("services"):
                try:
                    services = [int(x) for x in json.loads(drv["services"])]
                except (ValueError, TypeError):
                    pass
            if drv.get("wifi"):
                try:
                    w = json.loads(drv["wifi"])
                    if w.get("wifi"):
                        wifi = {"wifi": str(w["wifi"]), "wifiPass": str(w.get("wifiPass", ""))}
                except ValueError:
                    pass
            if not drv.get("active", 1):
                block = (403, f"driver '{drv['name']}' is switched off")
            elif drv.get("license_until") and drv["license_until"] < time.strftime("%Y-%m-%d"):
                block = (403, f"license of '{drv['name']}' ended on {drv['license_until']}")
        elif serial:
            mode = self.st.s.get("unregistered", "default")
            if mode == "expired":
                block = (403, "no driver registered for this meter")
            elif mode == "refuse":
                block = (404, "no driver registered for this meter")
        dev = self.db.device(serial) if serial else None
        if dev and dev.get("tariff_override"):
            try:
                t.update(json.loads(dev["tariff_override"]))
                source.append("device override")
            except ValueError:
                pass
        if dev and dev.get("expired") and not block:
            block = (403, "device marked expired")
        return {"tariff": t, "services": services, "wifi": wifi, "driver": drv, "block": block, "source": source}

    def effective_tariff(self, serial):
        return self.resolve(serial)["tariff"]

    def transport_for(self, kind):
        sc = self.st["scenario"]
        target = sc.get("transport_target", "all")
        group = {"TARIFF": "tariff", "START": "records", "END": "records", "SERVICE": "records",
                 "NOBAT": "records", "FIRMWARE": "firmware"}.get(kind, "")
        mode = sc.get("transport", "normal")
        if mode == "normal" or (target != "all" and target != group):
            return "normal", ""
        if mode in ("chunked", "close_delimited", "slow"):
            return mode, ""
        return "normal", mode  # partial | drop | reset | stall

    async def prelude(self, kind):
        sc = self.st["scenario"]
        if sc.get("delay_s", 0) > 0:
            await asyncio.sleep(min(float(sc["delay_s"]), 120))
        if sc.get("fail_percent", 0) > 0 and random.random() * 100 < sc["fail_percent"]:
            return treply("random failure (scenario)", 503)
        return None

    def finish(self, r: Reply, kind, serial, params, problems=None, notes=None, trip=None):
        r.transfer, r.fault = self.transport_for(kind)
        if r.fault:
            notes = (notes or []) + [f"transport: {r.fault}"]
        elif r.transfer != "normal":
            notes = (notes or []) + [f"transport: {r.transfer}"]
        r.log = {"kind": kind, "serial": serial, "params": params, "problems": problems or [],
                 "notes": notes or [], "trip": trip}
        return r

    def allowed(self, serial):
        al = self.st["allowed_serials"] or []
        return not al or serial in al

    # ---------------------------------------------------------------- tariff
    async def tariff(self, q: Req):
        p = dict(q.query)
        serial = p.get("serial", "")
        notes = [] if serial else ["no serial in the request"]
        if not self.allowed(serial):
            return self.finish(treply("unknown serial", 404), "TARIFF", serial, p, notes=["serial not in the allowed list"])
        pre = await self.prelude("TARIFF")
        if pre:
            return self.finish(pre, "TARIFF", serial, p, notes=["scenario: random failure"])
        info = self.resolve(serial)
        sc = self.st["scenario"]["tariff"]
        drv = info["driver"]
        if drv:
            notes.append(f"driver: {drv['name']}")
        if info["block"] or sc == "expired":
            code, why = info["block"] or (403, "scenario: expired")
            body = jreply({"error": why}, code) if code == 403 else treply("unknown meter", 404)
            return self.finish(body, "TARIFF", serial, p, notes=notes + [why])
        if sc == "error500":
            return self.finish(treply("internal error", 500), "TARIFF", serial, p, notes=notes + ["scenario: 500"])
        if sc == "html502":
            return self.finish(treply("<html><body><h1>502 Bad Gateway</h1></body></html>", 502, "text/html"),
                               "TARIFF", serial, p, notes=notes + ["scenario: 502"])
        if sc == "redirect302":
            r = treply("moved", 302)
            r.headers.append(("Location", "/api/taxi/TaxiFareNew?" + "&".join(f"{k}={v}" for k, v in p.items())))
            return self.finish(r, "TARIFF", serial, p, notes=notes + ["scenario: 302"])

        t = dict(info["tariff"])
        t["service"] = list(info["services"])
        if info["wifi"].get("wifi"):
            t["wifi"], t["wifiPass"] = info["wifi"]["wifi"], info["wifi"]["wifiPass"]
        fw = self.st["fw"]
        if fw["offer"] and fw["file"] and fw["version"]:
            t["fw_version"] = fw["version"]
            t["fw_url"] = f"https://{q.headers.get('host', 'localhost')}/fw/{fw['file']}"
        if sc == "weird_types":
            t.update(PM=str(t["PM"]), pri_cost=float(t["pri_cost"]), Cost_200=t["Cost_200"] + 0.5,
                     samar=bool(t["samar"]), korsi=bool(t["korsi"]), darbast=True, nobat="1")
        elif sc == "wrapper":
            t = {"taxiFare": t}
        elif sc == "many_services":
            t["service"] = list(range(201, 216))
        elif sc == "out_of_range":
            t["pri_cost"] = -5
        elif sc == "huge_number":
            t["pri_cost"] = 99999999999999
        elif sc == "pm_zero":
            t["PM"] = 0
        elif sc == "empty":
            t = {}
        body = json.dumps(t)
        if sc == "bad_json":
            body = body[: len(body) // 2]
        else:
            self.db.x("INSERT INTO served_tariffs(ts, serial, tariff) VALUES(?,?,?)", (time.time(), serial, body))
        if sc != "ok":
            notes.append(f"scenario: {sc}")
        return self.finish(Reply(200, body.encode()), "TARIFF", serial, p, notes=notes)

    # ---------------------------------------------------------------- records
    def _served_tariff(self, serial):
        row = self.db.one("SELECT tariff FROM served_tariffs WHERE serial=? ORDER BY ts DESC LIMIT 1", (serial,))
        if row:
            try:
                t = normalize_tariff(json.loads(row["tariff"]))
                if t.get("PM"):
                    return t, "tariff last sent to this meter"
            except ValueError:
                pass
        return self.effective_tariff(serial), "current tariff (none was sent to this meter yet)"

    def check_end(self, p, serial):
        problems, notes = [], []
        for k in END_FIELDS:
            if k not in p:
                problems.append(f"missing {k}")
        v = {}
        for k in END_NUMS:
            if k in p:
                if not INT_RE.match(p[k]):
                    problems.append(f"{k} is not an integer: {p[k]!r}")
                elif int(p[k]) < 0:
                    problems.append(f"{k} is negative")
                else:
                    v[k] = int(p[k])
        for k in ("start_time", "stop_time"):
            if k in p and not HM_RE.match(p[k]):
                problems.append(f"{k} is not HH:MM: {p[k]!r}")
        if "bt_number" in v and not 1 <= v["bt_number"] <= 5:
            problems.append("bt_number must be 1..5")
        if all(k in v for k in ("trip_price", "traffic_price", "total_price")) and \
                v["trip_price"] + v["traffic_price"] != v["total_price"]:
            problems.append("total_price != trip_price + traffic_price")
        if "night_price" in v and "trip_price" in v and v["night_price"] > v["trip_price"]:
            problems.append("night_price is bigger than trip_price")
        if "avg_speed" in v and "max_speed" in v and v["avg_speed"] > v["max_speed"]:
            problems.append("avg_speed is bigger than max_speed")

        trip = None
        need = ("bt_number", "trip_distance", "trip_duration", "traffic_duration", "avg_speed", "max_speed",
                "trip_price", "traffic_price", "night_price", "total_price")
        if all(k in v for k in need) and 1 <= v["bt_number"] <= 5:
            r = {"bt": v["bt_number"], "distance": v["trip_distance"], "duration": v["trip_duration"],
                 "wait": v["traffic_duration"], "avg_speed": v["avg_speed"], "max_speed": v["max_speed"],
                 "trip_price": v["trip_price"], "traffic_price": v["traffic_price"],
                 "night_price": v["night_price"], "total": v["total_price"]}
            t, source = self._served_tariff(serial)
            fc = fare.check(t, r)
            problems += fc["problems"]
            fp = hashlib.sha1(json.dumps(sorted(p.items())).encode()).hexdigest()
            dup = self.db.one("SELECT id FROM trips WHERE fingerprint=? ORDER BY id LIMIT 1", (fp,))
            if dup:
                problems.append(f"duplicate: the same trip arrived before (trip #{dup['id']})")
            nid = None
            if "nobat_id" in p:
                if not INT_RE.match(p["nobat_id"]):
                    problems.append(f"nobat_id is not an integer: {p['nobat_id']!r}")
                else:
                    nid = int(p["nobat_id"])
                    row = self.db.one("SELECT * FROM nobat WHERE id=?", (nid,))
                    if not row:
                        problems.append(f"nobat_id {nid} is unknown")
                    elif row["serial"] != serial:
                        problems.append(f"nobat_id {nid} belongs to another meter ({row['serial']})")
                    elif row["state"] != "boarded":
                        problems.append(f"nobat_id {nid}: trip end, but the queue entry is '{row['state']}' (no NobatBoard)")
                    if v["bt_number"] != 5:
                        problems.append("nobat_id on a 4-channel trip (only the single meter is used for the queue)")
            trip = dict(r, serial=serial, start_time=p.get("start_time", ""), stop_time=p.get("stop_time", ""), nobat_id=nid,
                        expected_low=fc["low"], expected_high=fc["high"],
                        fare_check=f"{fc['verdict']}: {fc['text']} [{source}]",
                        duplicate_of=dup["id"] if dup else None, fingerprint=fp)
        # order: a trip end needs a start on the same channel
        bt = p.get("bt_number", "")
        if serial and INT_RE.match(bt or "x"):
            o = self.db.one("SELECT ts FROM open_trips WHERE serial=? AND bt=?", (serial, int(bt)))
            if not o and not (trip and trip["duplicate_of"]):
                notes.append("END without START (the meter drops old START records when its queue is full)")
            self.db.x("DELETE FROM open_trips WHERE serial=? AND bt=?", (serial, int(bt)))
        return problems, notes, trip

    def check_start(self, p, serial):
        problems, notes = [], []
        for k in ("serial", "bt_number"):
            if k not in p:
                problems.append(f"missing {k}")
        bt = p.get("bt_number", "")
        if not (INT_RE.match(bt or "x") and 1 <= int(bt) <= 5):
            problems.append("bt_number must be 1..5")
        elif serial:
            o = self.db.one("SELECT ts FROM open_trips WHERE serial=? AND bt=?", (serial, int(bt)))
            if o:
                problems.append(f"START again on channel {bt} without an END (first START at "
                                f"{time.strftime('%H:%M:%S', time.localtime(o['ts']))})")
            self.db.x("INSERT INTO open_trips(serial, bt, ts) VALUES(?,?,?) ON CONFLICT(serial, bt) DO UPDATE SET ts=excluded.ts",
                      (serial, int(bt), time.time()))
        return problems, notes

    def check_service(self, p, serial):
        problems = []
        for k in ("serial", "serviceNum", "start"):
            if k not in p:
                problems.append(f"missing {k}")
        if "start" in p and p["start"] not in ("1", "2"):
            problems.append("start must be 1 or 2")
        notes = []
        if "serviceNum" in p and not INT_RE.match(p["serviceNum"]):
            problems.append("serviceNum is not an integer")
        elif serial and "serviceNum" in p and p.get("start") in ("1", "2"):
            num = int(p["serviceNum"])
            o = self.db.one("SELECT ts FROM open_services WHERE serial=? AND service=?", (serial, num))
            if p["start"] == "1":
                if o:
                    problems.append(f"service {num} started again without an end (first start at "
                                    f"{time.strftime('%H:%M:%S', time.localtime(o['ts']))})")
                self.db.x("INSERT INTO open_services(serial, service, ts) VALUES(?,?,?) ON CONFLICT(serial, service)"
                          " DO UPDATE SET ts=excluded.ts", (serial, num, time.time()))
            else:
                if not o:
                    notes.append(f"service {num} ended without a start (the meter may have dropped it, or rebooted)")
                else:
                    notes.append(f"service {num} took {int(time.time() - o['ts'])} s")
                self.db.x("DELETE FROM open_services WHERE serial=? AND service=?", (serial, num))
        return problems, notes

    async def record(self, q: Req, kind):
        p = dict(q.query)
        serial = p.get("serial", "")
        if not self.allowed(serial):
            return self.finish(treply("unknown serial", 404), kind, serial, p, notes=["serial not in the allowed list"])
        pre = await self.prelude(kind)
        if pre:
            return self.finish(pre, kind, serial, p, notes=["scenario: random failure"])
        sc = self.st["scenario"]["post"]
        if sc != "ok":
            # the meter will send this record again: do not run order / duplicate checks on it now
            code = {"redirect301": 301, "error500": 500, "bad_request": 400}[sc]
            r = treply({301: "moved", 500: "internal error", 400: "bad request"}[code], code)
            if code == 301:
                r.headers.append(("Location", "/"))
                # 301 counts as "sent" on the meter, so it is a real delivery: check it
            else:
                return self.finish(r, kind, serial, p, notes=[f"scenario: {sc}"])
        trip = None
        if kind == "END":
            problems, notes, trip = self.check_end(p, serial)
        elif kind == "START":
            problems, notes = self.check_start(p, serial)
        else:
            problems, notes = self.check_service(p, serial)
        if sc == "redirect301":
            return self.finish(r, kind, serial, p, problems, notes + ["scenario: 301"], trip)
        return self.finish(jreply({"ok": True}), kind, serial, p, problems, notes, trip)

    # ---------------------------------------------------------------- nobat (queue at a station)
    NOBAT_STALE_S = 90  # a car that did not ask for 90 s has left (power off, out of range)

    def nobat_advance(self):
        """Drop cars that stopped asking; give the turn to the first car (auto mode)."""
        now = time.time()
        self.db.x("UPDATE nobat SET state='expired', done_at=? WHERE state IN ('wait','turn') AND last_seen < ?",
                  (now, now - self.NOBAT_STALE_S))
        auto = float(self.st["nobat"].get("auto_s", 0) or 0)
        if auto <= 0 or self.db.one("SELECT id FROM nobat WHERE state='turn'"):
            return
        head = self.db.one("SELECT * FROM nobat WHERE state='wait' ORDER BY joined, id LIMIT 1")
        if not head:
            return
        last = self.db.one("SELECT max(done_at) AS t FROM nobat WHERE state!='wait' AND state!='turn'")["t"] or 0
        if now - max(head["joined"], last) >= auto:
            self.db.x("UPDATE nobat SET state='turn', turn_at=? WHERE id=?", (now, head["id"]))

    def nobat_give_turn(self):
        if self.db.one("SELECT id FROM nobat WHERE state='turn'"):
            return None
        head = self.db.one("SELECT * FROM nobat WHERE state='wait' ORDER BY joined, id LIMIT 1")
        if head:
            self.db.x("UPDATE nobat SET state='turn', turn_at=? WHERE id=?", (time.time(), head["id"]))
        return head

    def nobat_queue(self):
        rows = self.db.q("SELECT * FROM nobat WHERE state IN ('wait','turn') ORDER BY state='turn' DESC, joined, id")
        for i, r in enumerate(rows):
            r["position"] = i + 1
        return rows

    def nobat_info(self, serial):
        for r in self.nobat_queue():
            if r["serial"] == serial:
                return {"state": r["state"], "position": r["position"], "nobat_id": r["id"]}
        return {"state": "none", "position": 0, "nobat_id": 0}

    async def nobat(self, q: Req, action):
        p = dict(q.query)
        serial = p.get("serial", "")
        params = dict(p, action=action)
        if not self.allowed(serial):
            return self.finish(treply("unknown serial", 404), "NOBAT", serial, params, notes=["serial not in the allowed list"])
        pre = await self.prelude("NOBAT")
        if pre:
            return self.finish(pre, "NOBAT", serial, params, notes=["scenario: random failure"])
        problems, notes = [], []
        if not serial:
            problems.append("missing serial")
            return self.finish(jreply({"error": "missing serial"}, 400), "NOBAT", serial, params, problems)
        res = self.resolve(serial)
        if res.get("block"):
            code, why = res["block"]
            return self.finish(treply("not allowed", code), "NOBAT", serial, params, notes=[why])
        now = time.time()
        self.nobat_advance()
        mine = self.db.one("SELECT * FROM nobat WHERE serial=? AND state IN ('wait','turn')", (serial,))
        if mine:
            self.db.x("UPDATE nobat SET last_seen=? WHERE id=?", (now, mine["id"]))
        if action == "ready":
            if mine:
                notes.append(f"already in the queue (#{mine['id']}): answered with its place")
            else:
                self.db.x("INSERT INTO nobat(serial, joined, last_seen, state) VALUES(?,?,?,'wait')", (serial, now, now))
                self.nobat_advance()
            info = self.nobat_info(serial)
            notes.append(f"queue #{info['nobat_id']}: place {info['position']}, {info['state']}")
            return self.finish(jreply(info), "NOBAT", serial, params, problems, notes)
        if action == "status":
            info = self.nobat_info(serial)
            if info["state"] == "none":
                notes.append("not in the queue: the meter should send NobatReady again")
            return self.finish(jreply(info), "NOBAT", serial, params, problems, notes)
        # board / leave come through the meter's upload queue: they can arrive late or twice
        nid = p.get("nobat_id", "")
        row = None
        if nid:
            if not INT_RE.match(nid):
                problems.append(f"nobat_id is not an integer: {nid!r}")
            else:
                row = self.db.one("SELECT * FROM nobat WHERE id=?", (int(nid),))
                if not row:
                    problems.append(f"nobat_id {nid} is unknown")
                elif row["serial"] != serial:
                    problems.append(f"nobat_id {nid} belongs to another meter ({row['serial']})")
                    row = None
        else:
            row = mine
            notes.append("no nobat_id: used the meter's open queue entry")
        if action == "board":
            if row and row["state"] == "turn":
                self.db.x("UPDATE nobat SET state='boarded', done_at=? WHERE id=?", (now, row["id"]))
                notes.append(f"queue #{row['id']}: passenger is in, after {int(now - row['joined'])} s in the queue")
            elif row and row["state"] == "wait":
                self.db.x("UPDATE nobat SET state='boarded', done_at=? WHERE id=?", (now, row["id"]))
                problems.append(f"queue #{row['id']}: boarded before its turn")
            elif row:
                notes.append(f"queue #{row['id']} is already '{row['state']}' (sent twice?)")
            elif not problems:
                problems.append("board without a queue entry")
        else:  # leave
            if row and row["state"] in ("wait", "turn"):
                self.db.x("UPDATE nobat SET state='left', done_at=? WHERE id=?", (now, row["id"]))
                notes.append(f"queue #{row['id']}: the car left the queue")
            elif row:
                notes.append(f"queue #{row['id']} is already '{row['state']}'")
        self.nobat_advance()
        return self.finish(jreply({"ok": True}), "NOBAT", serial, params, problems, notes)

    # ---------------------------------------------------------------- firmware
    async def firmware(self, q: Req, name):
        safe = Path(name).name
        path = self.fw_dir / safe
        sc = self.st["scenario"]["fw"]
        params = {"file": safe}
        pre = await self.prelude("FIRMWARE")
        if pre:
            return self.finish(pre, "FIRMWARE", "", params, notes=["scenario: random failure"])
        if sc == "not_found" or not path.is_file() or safe != name:
            return self.finish(treply("not found", 404), "FIRMWARE", "", params,
                               notes=["scenario: 404"] if sc == "not_found" else ["no such file"])
        if sc == "redirect302":
            r = treply("moved", 302)
            r.headers.append(("Location", f"/fw-moved/{safe}"))
            return self.finish(r, "FIRMWARE", "", params, notes=["scenario: 302"])
        data = path.read_bytes()
        if sc == "truncated":
            data = data[: len(data) // 2]
        elif sc == "corrupt":
            b = bytearray(data)
            for i in range(len(b) // 2, min(len(b) // 2 + 64, len(b))):
                b[i] ^= 0xFF
            data = bytes(b)
        elif sc == "wrong_magic":
            b = bytearray(data)
            b[0] ^= 0xFF
            data = bytes(b)
        params["bytes"] = len(data)
        return self.finish(Reply(200, data, "application/octet-stream"), "FIRMWARE", "", params,
                           notes=[] if sc == "ok" else [f"scenario: {sc}"])

    def store_trip(self, request_id, trip, problems):
        if not trip:
            return
        self.db.x(
            "INSERT INTO trips(request_id, ts, serial, bt, start_time, stop_time, distance, duration, wait, avg_speed,"
            " max_speed, trip_price, traffic_price, night_price, total, expected_low, expected_high, fare_check,"
            " duplicate_of, problems, fingerprint, driver_id, nobat_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (request_id, time.time(), trip["serial"], trip["bt"], trip["start_time"], trip["stop_time"],
             trip["distance"], trip["duration"], trip["wait"], trip["avg_speed"], trip["max_speed"],
             trip["trip_price"], trip["traffic_price"], trip["night_price"], trip["total"],
             trip["expected_low"], trip["expected_high"], trip["fare_check"], trip["duplicate_of"],
             json.dumps(problems), trip["fingerprint"], (self.db.driver_by_serial(trip["serial"]) or {}).get("id"),
             trip.get("nobat_id")))
        if trip.get("nobat_id") and not trip.get("duplicate_of"):
            tid = self.db.one("SELECT max(id) AS id FROM trips")["id"]
            self.db.x("UPDATE nobat SET trip_id=? WHERE id=? AND trip_id IS NULL", (tid, trip["nobat_id"]))
