"""Admin page and its JSON API (FastAPI). Only for 127.0.0.1 or with the admin token."""
import csv
import datetime
import io
import json
import re
import secrets
import time
from pathlib import Path

from fastapi import FastAPI, Request
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse, PlainTextResponse, Response
from fastapi.staticfiles import StaticFiles

from .meter import image_version
from .state import (FW_SCENARIOS, POST_SCENARIOS, TARIFF_FIELDS, TARIFF_SCENARIOS, TLS_PROFILES,
                    TRANSPORT_MODES, TRANSPORT_TARGETS, UNREGISTERED_MODES)


def create_admin(ctx):
    db, st, tls, ntp = ctx["db"], ctx["state"], ctx["tls"], ctx["ntp"]
    fw_dir: Path = ctx["fw_dir"]
    web: Path = ctx["web_dir"]
    app = FastAPI(title="Taxi meter mock server", docs_url="/admin/docs", redoc_url=None,
                  openapi_url="/admin/openapi.json")
    app.mount("/admin/static", StaticFiles(directory=str(web)), name="static")

    # ---------------------------------------------------------------- protection
    def admin_ok(request: Request):
        host = request.client.host if request.client else ""
        if host in ("127.0.0.1", "::1"):
            return True
        tok = (request.headers.get("x-admin-token") or request.cookies.get("admin_token")
               or request.query_params.get("token") or "")
        return bool(tok) and secrets.compare_digest(tok, st["admin_token"])

    app.state.admin_ok = admin_ok

    @app.middleware("http")
    async def guard(request: Request, call_next):
        p = request.url.path
        if p.startswith("/admin/api") or p in ("/admin", "/admin/", "/admin/docs", "/admin/openapi.json"):
            if not admin_ok(request):
                return PlainTextResponse("admin: forbidden (open it from this PC, or add ?token=...)", status_code=403)
        return await call_next(request)

    def save():
        st.save()

    @app.get("/")
    async def root():
        return PlainTextResponse("taxi mock server. Admin page: /admin")

    @app.get("/admin")
    async def page(request: Request):
        resp = FileResponse(str(web / "index.html"), media_type="text/html")
        if request.query_params.get("token"):
            resp.set_cookie("admin_token", request.query_params["token"], httponly=True, secure=True, samesite="strict")
        return resp

    # ---------------------------------------------------------------- meta + settings
    @app.get("/admin/api/meta")
    async def meta():
        return {"tariff_scenarios": TARIFF_SCENARIOS, "post_scenarios": POST_SCENARIOS, "fw_scenarios": FW_SCENARIOS,
                "transport_modes": TRANSPORT_MODES, "transport_targets": TRANSPORT_TARGETS,
                "tls_profiles": TLS_PROFILES, "tls_info": tls.info if tls else {}, "unregistered_modes": UNREGISTERED_MODES,
                "tariff_fields": [{"key": k, "label": l, "group": g} for k, l, g in TARIFF_FIELDS],
                "ntp": {"running": ntp.running if ntp else False, "error": ntp.error if ntp else "not started",
                        "port": ntp.port if ntp else None},
                "host": ctx["host"], "port": ctx["port"], "tls_enabled": bool(tls)}

    @app.get("/admin/api/state")
    async def get_state():
        return st.s

    @app.put("/admin/api/tariff")
    async def put_tariff(request: Request):
        b = await request.json()
        if not isinstance(b, dict):
            return PlainTextResponse("JSON object expected", 400)
        if "tariff" in b:
            if not isinstance(b["tariff"], dict):
                return PlainTextResponse("tariff must be an object", 400)
            st.s["tariff"] = b["tariff"]
        if "services" in b:
            st.s["services"] = [int(x) for x in b["services"]][:30]
        if "wifi" in b:
            st.s["wifi"] = {"wifi": str(b["wifi"].get("wifi", "")), "wifiPass": str(b["wifi"].get("wifiPass", ""))}
        if "allowed_serials" in b:
            st.s["allowed_serials"] = [str(x).strip() for x in b["allowed_serials"] if str(x).strip()]
        if "unregistered" in b:
            if b["unregistered"] not in UNREGISTERED_MODES:
                return PlainTextResponse("unknown mode", 400)
            st.s["unregistered"] = b["unregistered"]
        save()
        return {"ok": True}

    @app.put("/admin/api/scenario")
    async def put_scenario(request: Request):
        b = await request.json()
        sc = st.s["scenario"]
        lists = {"tariff": TARIFF_SCENARIOS, "post": POST_SCENARIOS, "fw": FW_SCENARIOS,
                 "transport": TRANSPORT_MODES, "transport_target": TRANSPORT_TARGETS}
        for k, allowed in lists.items():
            if k in b:
                if b[k] not in allowed:
                    return PlainTextResponse(f"unknown {k}: {b[k]}", 400)
                sc[k] = b[k]
        if "delay_s" in b:
            sc["delay_s"] = max(0.0, min(120.0, float(b["delay_s"])))
        if "fail_percent" in b:
            sc["fail_percent"] = max(0.0, min(100.0, float(b["fail_percent"])))
        save()
        return {"ok": True, "scenario": sc}

    # ---------------------------------------------------------------- dashboard
    @app.get("/admin/api/summary")
    async def summary():
        now = time.time()
        start = now - 3600
        rpm = db.q("SELECT CAST((ts - ?) / 60 AS INTEGER) AS b,"
                   " SUM(CASE WHEN fault = '' AND status BETWEEN 200 AND 399 THEN 1 ELSE 0 END) AS ok,"
                   " SUM(CASE WHEN fault = '' AND status >= 400 THEN 1 ELSE 0 END) AS err,"
                   " SUM(CASE WHEN fault != '' OR status = 0 THEN 1 ELSE 0 END) AS bad"
                   " FROM requests WHERE ts >= ? GROUP BY b", (start, start))
        per_min = [{"ok": 0, "err": 0, "bad": 0} for _ in range(60)]
        for r in rpm:
            if 0 <= r["b"] < 60:
                per_min[r["b"]] = {"ok": r["ok"], "err": r["err"], "bad": r["bad"]}
        days = []
        rows = {r["d"]: r for r in db.q(
            "SELECT date(ts, 'unixepoch', 'localtime') AS d, COUNT(*) AS n, COALESCE(SUM(total), 0) AS rial"
            " FROM trips WHERE duplicate_of IS NULL AND ts >= ? GROUP BY d", (now - 15 * 86400,))}
        for i in range(13, -1, -1):
            d = (datetime.date.today() - datetime.timedelta(days=i)).isoformat()
            r = rows.get(d, {"n": 0, "rial": 0})
            days.append({"day": d, "trips": r["n"], "toman": r["rial"] // 10})
        kinds = db.q("SELECT kind, COUNT(*) AS n FROM requests WHERE ts >= ? GROUP BY kind ORDER BY n DESC", (now - 86400,))
        today0 = datetime.datetime.combine(datetime.date.today(), datetime.time()).timestamp()
        t = db.one("SELECT COUNT(*) AS n, COALESCE(SUM(total), 0) AS rial FROM trips WHERE duplicate_of IS NULL AND ts >= ?", (today0,))
        kpi = {
            "devices_online": db.one("SELECT COUNT(*) AS n FROM devices WHERE last_seen >= ?", (now - 600,))["n"],
            "devices": db.one("SELECT COUNT(*) AS n FROM devices")["n"],
            "requests_hour": db.one("SELECT COUNT(*) AS n FROM requests WHERE ts >= ?", (start,))["n"],
            "trips_today": t["n"], "toman_today": t["rial"] // 10,
            "problems_day": db.one("SELECT COUNT(*) AS n FROM requests WHERE ts >= ? AND problems != '[]'", (now - 86400,))["n"],
            "fare_mismatch": db.one("SELECT COUNT(*) AS n FROM trips WHERE fare_check LIKE 'mismatch%'")["n"],
            "duplicates": db.one("SELECT COUNT(*) AS n FROM trips WHERE duplicate_of IS NOT NULL")["n"],
            "tls_failures_day": db.one("SELECT COUNT(*) AS n FROM requests WHERE kind = 'TLS' AND ts >= ?", (now - 86400,))["n"],
            "ntp_answers_hour": db.one("SELECT COUNT(*) AS n FROM ntp WHERE ts >= ?", (start,))["n"],
            "drivers": db.one("SELECT COUNT(*) AS n FROM drivers")["n"],
            "unregistered": db.one("SELECT COUNT(*) AS n FROM devices WHERE serial NOT IN (SELECT serial FROM drivers WHERE serial IS NOT NULL)")["n"],
        }
        last_tls = db.one("SELECT tls FROM requests WHERE tls != '' AND kind != 'TLS' ORDER BY id DESC LIMIT 1")
        recent = db.q("SELECT id, ts, kind, serial, status, problems FROM requests WHERE problems != '[]' ORDER BY id DESC LIMIT 8")
        for r in recent:
            r["problems"] = json.loads(r["problems"])
        return {"kpi": kpi, "per_min": per_min, "days": days, "kinds": kinds, "recent_problems": recent,
                "last_tls": last_tls["tls"] if last_tls else "", "now": now,
                "meter_time": now + float(st["time"].get("offset_s", 0)), "scenario": st["scenario"],
                "tls_profile": st["tls"]["profile"]}

    # ---------------------------------------------------------------- tables
    def _filters(request, table, paging=True):
        qp = request.query_params
        where, args = [], []
        if qp.get("kind"):
            where.append("kind = ?"); args.append(qp["kind"])
        if qp.get("serial"):
            where.append("serial = ?"); args.append(qp["serial"])
        if qp.get("problems") == "1":
            where.append("problems != '[]'")
        if qp.get("q"):
            col = "params" if table == "requests" else "fare_check || problems"
            where.append(f"{col} LIKE ?"); args.append(f"%{qp['q']}%")
        if paging and qp.get("before"):
            where.append("id < ?"); args.append(int(qp["before"]))
        if table == "trips" and qp.get("check") in ("ok", "mismatch", "unchecked"):
            where.append("fare_check LIKE ?"); args.append(qp["check"] + "%")
        if table == "trips" and qp.get("dup") == "1":
            where.append("duplicate_of IS NOT NULL")
        if table == "trips" and qp.get("driver"):
            where.append("driver_id = ?"); args.append(int(qp["driver"]))
        return (" WHERE " + " AND ".join(where)) if where else "", args

    @app.get("/admin/api/requests")
    async def requests_list(request: Request):
        w, a = _filters(request, "requests")
        limit = max(1, min(500, int(request.query_params.get("limit", 100))))
        rows = db.q(f"SELECT * FROM requests{w} ORDER BY id DESC LIMIT ?", (*a, limit))
        for r in rows:
            for k in ("params", "problems", "notes"):
                r[k] = json.loads(r[k] or "[]")
        w2, a2 = _filters(request, "requests", paging=False)
        total = db.one(f"SELECT COUNT(*) AS n FROM requests{w2}", a2)["n"]
        return {"rows": rows, "total": total}

    @app.get("/admin/api/trips")
    async def trips_list(request: Request):
        w, a = _filters(request, "trips")
        limit = max(1, min(500, int(request.query_params.get("limit", 100))))
        rows = db.q(f"SELECT trips.*, drivers.name AS driver FROM trips LEFT JOIN drivers ON drivers.id = trips.driver_id"
                    f"{w.replace('serial', 'trips.serial').replace(' id <', ' trips.id <')} ORDER BY trips.id DESC LIMIT ?", (*a, limit))
        for r in rows:
            r["problems"] = json.loads(r["problems"] or "[]")
        return {"rows": rows}

    @app.get("/admin/api/export/{name}.csv")
    async def export(name: str):
        if name not in ("requests", "trips", "devices", "drivers"):
            return PlainTextResponse("unknown table", 404)
        rows = db.q(f"SELECT * FROM {name} ORDER BY " + ("serial" if name == "devices" else "id"))
        for k in ("created",):
            for r in rows:
                if r.get(k):
                    r[k] = datetime.datetime.fromtimestamp(r[k]).strftime("%Y-%m-%d %H:%M:%S")
        buf = io.StringIO()
        if rows:
            w = csv.DictWriter(buf, fieldnames=list(rows[0].keys()))
            w.writeheader()
            for r in rows:
                for k in ("ts", "first_seen", "last_seen"):
                    if k in r and r[k]:
                        r[k] = datetime.datetime.fromtimestamp(r[k]).strftime("%Y-%m-%d %H:%M:%S")
                w.writerow(r)
        return Response(buf.getvalue().encode("utf-8-sig"), media_type="text/csv",
                        headers={"Content-Disposition": f'attachment; filename="{name}.csv"'})

    @app.delete("/admin/api/data")
    async def clear_data():
        db.clear_logs()
        return {"ok": True}

    # ---------------------------------------------------------------- devices
    @app.get("/admin/api/devices")
    async def devices():
        rows = db.q("SELECT * FROM devices ORDER BY last_seen DESC")
        now = time.time()
        for r in rows:
            r["online"] = (now - (r["last_seen"] or 0)) < 600
            r["trips"] = db.one("SELECT COUNT(*) AS n FROM trips WHERE serial = ? AND duplicate_of IS NULL", (r["serial"],))["n"]
            r["problems"] = db.one("SELECT COUNT(*) AS n FROM requests WHERE serial = ? AND problems != '[]'", (r["serial"],))["n"]
            d = db.driver_by_serial(r["serial"])
            r["driver"] = {"id": d["id"], "name": d["name"]} if d else None
        return {"rows": rows}

    @app.put("/admin/api/devices/{serial}")
    async def put_device(serial: str, request: Request):
        b = await request.json()
        if not db.device(serial):
            db.x("INSERT INTO devices(serial, first_seen, last_seen, last_ip, last_tls, requests) VALUES(?,?,?,?,?,0)",
                 (serial, time.time(), 0, "", ""))
        if "expired" in b:
            db.x("UPDATE devices SET expired = ? WHERE serial = ?", (1 if b["expired"] else 0, serial))
        if "tariff_override" in b:
            ov = b["tariff_override"]
            if isinstance(ov, dict):
                ov = json.dumps(ov) if ov else ""
            elif isinstance(ov, str) and ov.strip():
                try:
                    assert isinstance(json.loads(ov), dict)
                except Exception:
                    return PlainTextResponse("override must be a JSON object", 400)
            db.x("UPDATE devices SET tariff_override = ? WHERE serial = ?", (ov or "", serial))
        if "note" in b:
            db.x("UPDATE devices SET note = ? WHERE serial = ?", (str(b["note"])[:200], serial))
        return {"ok": True, "device": db.device(serial)}

    @app.delete("/admin/api/devices/{serial}")
    async def del_device(serial: str):
        db.x("DELETE FROM devices WHERE serial = ?", (serial,))
        return {"ok": True}

    # ---------------------------------------------------------------- drivers
    DRIVER_COLS = ("name", "phone", "plate", "car", "serial", "profile_id", "license_until", "active",
                   "overrides", "services", "wifi", "note")

    def _clean_driver(b, partial):
        out = {}
        for k in DRIVER_COLS:
            if k not in b:
                continue
            v = b[k]
            if k in ("overrides", "wifi"):
                if isinstance(v, str):
                    v = v.strip()
                    if v:
                        try:
                            v = json.loads(v)
                        except ValueError:
                            raise ValueError(f"{k} must be a JSON object")
                if v and not isinstance(v, dict):
                    raise ValueError(f"{k} must be a JSON object")
                v = json.dumps(v) if v else ""
            elif k == "services":
                if isinstance(v, str):
                    v = [x.strip() for x in v.split(",") if x.strip()]
                try:
                    v = json.dumps([int(x) for x in v]) if v else ""
                except (TypeError, ValueError):
                    raise ValueError("service numbers must be numbers")
            elif k == "profile_id":
                v = int(v) if v not in (None, "", 0, "0") else None
                if v and not db.profile(v):
                    raise ValueError("no such tariff profile")
            elif k == "active":
                v = 1 if v else 0
            elif k == "license_until":
                v = str(v or "").strip()
                if v and not re.match(r"^\d{4}-\d{2}-\d{2}$", v):
                    raise ValueError("license_until must be YYYY-MM-DD")
            elif k == "serial":
                v = re.sub(r"[\s:.-]", "", str(v or "")).upper() or None  # "AA:BB:.." and "aa-bb" are fine
                if v and not re.match(r"^[0-9A-F]{12}$", v):
                    raise ValueError("serial must be 12 hex characters (see the INFO page of the meter)")
            else:
                v = str(v or "").strip()[:120]
            out[k] = v
        if not partial and not out.get("name"):
            raise ValueError("a driver needs a name")
        if "name" in out and not out["name"]:
            raise ValueError("a driver needs a name")
        return out

    def _driver_row(d):
        today0 = datetime.datetime.combine(datetime.date.today(), datetime.time()).timestamp()
        st_all = db.one("SELECT COUNT(*) AS n, COALESCE(SUM(total), 0) AS rial FROM trips WHERE driver_id = ? AND duplicate_of IS NULL", (d["id"],))
        st_today = db.one("SELECT COUNT(*) AS n, COALESCE(SUM(total), 0) AS rial FROM trips WHERE driver_id = ? AND duplicate_of IS NULL AND ts >= ?", (d["id"], today0))
        dev = db.device(d["serial"]) if d.get("serial") else None
        prof = db.profile(d.get("profile_id"))
        days_left = None
        if d.get("license_until"):
            try:
                days_left = (datetime.date.fromisoformat(d["license_until"]) - datetime.date.today()).days
            except ValueError:
                pass
        d = dict(d)
        d.update(trips=st_all["n"], toman=st_all["rial"] // 10, trips_today=st_today["n"], toman_today=st_today["rial"] // 10,
                 last_seen=dev["last_seen"] if dev else None, online=bool(dev and time.time() - (dev["last_seen"] or 0) < 600),
                 profile=prof["name"] if prof else None, days_left=days_left)
        return d

    @app.get("/admin/api/drivers")
    async def drivers_list():
        return {"rows": [_driver_row(d) for d in db.q("SELECT * FROM drivers ORDER BY name COLLATE NOCASE")],
                "unregistered": st.s.get("unregistered", "default"),
                "free_devices": [r["serial"] for r in db.q(
                    "SELECT serial FROM devices WHERE serial NOT IN (SELECT serial FROM drivers WHERE serial IS NOT NULL) ORDER BY last_seen DESC")]}

    @app.post("/admin/api/drivers")
    async def driver_add(request: Request):
        try:
            d = _clean_driver(await request.json(), partial=False)
        except ValueError as e:
            return PlainTextResponse(str(e), 400)
        if d.get("serial") and db.driver_by_serial(d["serial"]):
            return PlainTextResponse("this device already belongs to another driver", 400)
        d.setdefault("active", 1)
        d["created"] = time.time()
        cols = ", ".join(d.keys())
        rid = db.x(f"INSERT INTO drivers({cols}) VALUES({', '.join('?' * len(d))})", tuple(d.values()))
        return {"ok": True, "driver": _driver_row(db.one("SELECT * FROM drivers WHERE id = ?", (rid,)))}

    @app.put("/admin/api/drivers/{did}")
    async def driver_edit(did: int, request: Request):
        if not db.one("SELECT id FROM drivers WHERE id = ?", (did,)):
            return PlainTextResponse("no such driver", 404)
        try:
            d = _clean_driver(await request.json(), partial=True)
        except ValueError as e:
            return PlainTextResponse(str(e), 400)
        if d.get("serial"):
            other = db.driver_by_serial(d["serial"])
            if other and other["id"] != did:
                return PlainTextResponse(f"this device already belongs to {other['name']}", 400)
        if d:
            db.x(f"UPDATE drivers SET {', '.join(k + ' = ?' for k in d)} WHERE id = ?", (*d.values(), did))
        return {"ok": True, "driver": _driver_row(db.one("SELECT * FROM drivers WHERE id = ?", (did,)))}

    @app.delete("/admin/api/drivers/{did}")
    async def driver_delete(did: int):
        db.x("DELETE FROM drivers WHERE id = ?", (did,))
        return {"ok": True}

    @app.get("/admin/api/drivers/{did}/preview")
    async def driver_preview(did: int):
        """exactly what this driver's meter gets when it connects"""
        d = db.one("SELECT * FROM drivers WHERE id = ?", (did,))
        if not d:
            return PlainTextResponse("no such driver", 404)
        if not d.get("serial"):
            return {"block": None, "note": "no device linked yet", "tariff": None}
        info = ctx["meter"].resolve(d["serial"])
        t = dict(info["tariff"]); t["service"] = info["services"]
        if info["wifi"].get("wifi"):
            t["wifi"], t["wifiPass"] = info["wifi"]["wifi"], info["wifi"]["wifiPass"]
        return {"block": info["block"], "source": info["source"], "tariff": t}

    # ---------------------------------------------------------------- tariff profiles
    @app.get("/admin/api/profiles")
    async def profiles():
        rows = db.q("SELECT * FROM tariff_profiles ORDER BY name COLLATE NOCASE")
        for r in rows:
            r["drivers"] = db.one("SELECT COUNT(*) AS n FROM drivers WHERE profile_id = ?", (r["id"],))["n"]
            r["tariff"] = json.loads(r["tariff"] or "{}")
        return {"rows": rows}

    async def _profile_body(request):
        b = await request.json()
        name = str(b.get("name", "")).strip()[:60]
        t = b.get("tariff", {})
        if isinstance(t, str):
            t = json.loads(t or "{}")
        if not isinstance(t, dict):
            raise ValueError("tariff must be a JSON object")
        return name, t, str(b.get("note", ""))[:200]

    @app.post("/admin/api/profiles")
    async def profile_add(request: Request):
        try:
            name, t, note = await _profile_body(request)
        except ValueError as e:
            return PlainTextResponse(str(e), 400)
        if not name:
            return PlainTextResponse("a profile needs a name", 400)
        if db.one("SELECT id FROM tariff_profiles WHERE name = ?", (name,)):
            return PlainTextResponse("this name is used", 400)
        rid = db.x("INSERT INTO tariff_profiles(name, tariff, note) VALUES(?,?,?)", (name, json.dumps(t), note))
        return {"ok": True, "id": rid}

    @app.put("/admin/api/profiles/{pid}")
    async def profile_edit(pid: int, request: Request):
        try:
            name, t, note = await _profile_body(request)
        except ValueError as e:
            return PlainTextResponse(str(e), 400)
        if not db.profile(pid):
            return PlainTextResponse("no such profile", 404)
        db.x("UPDATE tariff_profiles SET name = COALESCE(NULLIF(?, ''), name), tariff = ?, note = ? WHERE id = ?", (name, json.dumps(t), note, pid))
        return {"ok": True}

    @app.delete("/admin/api/profiles/{pid}")
    async def profile_delete(pid: int):
        db.x("UPDATE drivers SET profile_id = NULL WHERE profile_id = ?", (pid,))
        db.x("DELETE FROM tariff_profiles WHERE id = ?", (pid,))
        return {"ok": True}

    # ---------------------------------------------------------------- firmware
    @app.get("/admin/api/fw")
    async def fw_list():
        files = []
        for p in sorted(fw_dir.iterdir()):
            if p.is_file():
                iv = image_version(p)
                files.append({"name": p.name, "size": p.stat().st_size, "version": iv[0] if iv else None,
                              "is_image": bool(iv), "mtime": p.stat().st_mtime})
        return {"files": files, "fw": st["fw"]}

    @app.put("/admin/api/fw/upload")
    async def fw_upload(request: Request, name: str = "firmware.signed.bin"):
        safe = re.sub(r"[^A-Za-z0-9._-]", "_", Path(name).name) or "firmware.bin"
        data = await request.body()
        if not data or len(data) > 8 * 1024 * 1024:
            return PlainTextResponse("empty or bigger than 8 MB", 400)
        (fw_dir / safe).write_bytes(data)
        iv = image_version(fw_dir / safe)
        if iv:  # the offered version comes from the image header, so it can not differ from the file
            st.s["fw"].update({"file": safe, "version": iv[0]})
            save()
        return {"ok": True, "name": safe, "size": len(data), "version": iv[0] if iv else None,
                "warning": None if iv else "not an MCUboot image (the meter will refuse it)"}

    @app.put("/admin/api/fw/offer")
    async def fw_offer(request: Request):
        b = await request.json()
        fw = st.s["fw"]
        if "file" in b:
            fw["file"] = Path(str(b["file"])).name
            iv = image_version(fw_dir / fw["file"]) if fw["file"] else None
            if iv:
                fw["version"] = iv[0]
        if b.get("version"):
            fw["version"] = str(b["version"])
        if "offer" in b:
            fw["offer"] = bool(b["offer"])
        save()
        return {"ok": True, "fw": fw}

    @app.delete("/admin/api/fw/{name}")
    async def fw_delete(name: str):
        p = fw_dir / Path(name).name
        if p.is_file():
            p.unlink()
        if st.s["fw"]["file"] == Path(name).name:
            st.s["fw"].update({"file": "", "version": "", "offer": False})
            save()
        return {"ok": True}

    # ---------------------------------------------------------------- time (NTP)
    @app.get("/admin/api/time")
    async def get_time():
        now = time.time()
        last = db.q("SELECT ts, remote, served FROM ntp ORDER BY id DESC LIMIT 10")
        return {"now": now, "meter_time": now + float(st["time"].get("offset_s", 0)),
                "offset_s": st["time"].get("offset_s", 0), "last_requests": last,
                "ntp": {"running": ntp.running if ntp else False, "error": ntp.error if ntp else "not started",
                        "port": ntp.port if ntp else None}}

    @app.put("/admin/api/time")
    async def put_time(request: Request):
        b = await request.json()
        now = time.time()
        if "offset_s" in b:
            off = float(b["offset_s"])
        elif "target" in b:  # "HH:MM" today (local time of this PC)
            m = re.match(r"^(\d{1,2}):(\d{2})$", str(b["target"]))
            if not m or int(m.group(1)) > 23 or int(m.group(2)) > 59:
                return PlainTextResponse("target must be HH:MM", 400)
            t = datetime.datetime.now().replace(hour=int(m.group(1)), minute=int(m.group(2)), second=0, microsecond=0)
            off = t.timestamp() - now
        elif "target_iso" in b:
            off = datetime.datetime.fromisoformat(str(b["target_iso"])).timestamp() - now
        else:
            return PlainTextResponse("send offset_s, target (HH:MM) or target_iso", 400)
        st.s["time"]["offset_s"] = round(off)
        save()
        return {"ok": True, "offset_s": st["time"]["offset_s"], "meter_time": now + off}

    # ---------------------------------------------------------------- TLS
    @app.get("/admin/api/tls")
    async def get_tls():
        rows = db.q("SELECT tls, COUNT(*) AS n FROM requests WHERE tls != '' AND kind != 'TLS' GROUP BY tls ORDER BY n DESC LIMIT 10")
        fails = db.q("SELECT ts, remote, tls AS profile, problems FROM requests WHERE kind = 'TLS' ORDER BY id DESC LIMIT 10")
        for f in fails:
            f["problems"] = json.loads(f["problems"])
        return {"profile": st["tls"]["profile"], "profiles": TLS_PROFILES, "info": tls.info if tls else {},
                "negotiated": rows, "failures": fails, "enabled": bool(tls)}

    @app.put("/admin/api/tls")
    async def put_tls(request: Request):
        b = await request.json()
        if b.get("profile") not in TLS_PROFILES:
            return PlainTextResponse("unknown profile", 400)
        st.s["tls"]["profile"] = b["profile"]
        save()
        return {"ok": True}

    return app
