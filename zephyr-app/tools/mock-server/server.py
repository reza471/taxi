#!/usr/bin/env python3
"""
Mock server for the taxi meter (Zephyr firmware). For TESTING only.

What it does
  * answers the same calls as the real server:
        GET  /api/taxi/TaxiFare?driver_type=1&serial=...      (the tariff JSON)
        POST /api/taxi/SetTaxiStart | SetTaxiEnd | StartService
  * checks every record the meter sends and shows problems (wrong sums, missing fields ...)
  * serves firmware files for OTA (/fw/<name>) and offers them in the tariff
  * scenarios on one click: license expired (403), HTTP 500, timeouts, redirects, broken JSON,
    numbers as text, wrapped JSON, huge numbers, damaged firmware ...
  * HTTPS with a self-signed certificate made on the first start (the meter does not check it)
  * optional NTP server (UDP 123) so the meter can set its clock on a local network
  * web page to control everything:  https://<ip>:<port>/admin

Run:   python server.py --ip 203.0.113.7 --port 8443          (see README.md)
"""
import argparse
import asyncio
import datetime
import ipaddress
import json
import os
import random
import re
import secrets
import socket
import struct
import sys
import threading
import time
from pathlib import Path

import uvicorn
from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse, JSONResponse, PlainTextResponse, Response, StreamingResponse

HERE = Path(__file__).resolve().parent

# ----------------------------------------------------------------------------- state
DEFAULT_TARIFF = {
    "PM": 46, "pri_cost": 26200, "Start_night": 22, "Stop_night": 6, "Cost_200": 1318,
    "Cost_stop": 8000, "Max_stop_km": 20, "free_trip": 0, "free_time": 0, "Percent_night": 10,
    "fraction": 50, "taxi": 1, "pri2_cost": 5000, "Cost2_200": 384, "Cost2_stop": 334,
    "samar": 1, "darbast": 1, "korsi": 1, "nobat": 1, "trip3": 0, "Cost3_200": 0, "Cost3_stop": 0,
}

TARIFF_SCENARIOS = {
    "ok": "normal tariff",
    "expired": "HTTP 403 (license expired): meter must show E-403",
    "error500": "HTTP 500",
    "html502": "HTTP 502 with an HTML page",
    "bad_json": "JSON cut in the middle",
    "weird_types": "numbers as text / true,false / 1318.5 (PHP/JS style)",
    "wrapper": 'everything inside {"taxiFare": {...}}',
    "many_services": "15 service numbers (meter keeps 10)",
    "out_of_range": "a negative price: meter must refuse the WHOLE tariff",
    "huge_number": "a price that does not fit 32 bit: meter must refuse",
    "pm_zero": "PM = 0: meter must refuse",
    "empty": "{} (no PM): meter must refuse",
    "timeout": "no answer for 30 s (meter gives up after 10 s)",
}
POST_SCENARIOS = {
    "ok": "HTTP 200",
    "redirect301": "HTTP 301 (the meter counts it as sent)",
    "error500": "HTTP 500 (meter keeps the record and retries)",
    "bad_request": "HTTP 400 (meter keeps the record and retries)",
    "timeout": "no answer for 30 s",
}
FW_SCENARIOS = {
    "ok": "file as it is",
    "truncated": "file cut at 50 %",
    "corrupt": "some bytes changed in the middle (signature must fail)",
    "wrong_magic": "first byte changed (meter must refuse at once)",
    "not_found": "HTTP 404",
    "slow": "about 20 KB per second",
}

lock = threading.RLock()
DATA = HERE / "data"
state = {}
records = []


def default_state():
    return {
        "tariff": dict(DEFAULT_TARIFF),
        "services": [211, 222, 233],
        "wifi": {"wifi": "", "wifiPass": ""},
        "fw": {"file": "", "version": "", "offer": False},
        "scenario": {"tariff": "ok", "post": "ok", "fw": "ok", "delay_s": 0, "fail_percent": 0},
        "admin_token": secrets.token_urlsafe(12),
        "allowed_serials": [],
        "tls": True,
    }


def save_state():
    with lock:
        (DATA / "state.json").write_text(json.dumps(state, indent=2))


def load_state():
    global state
    base = default_state()
    p = DATA / "state.json"
    if p.exists():
        try:
            saved = json.loads(p.read_text())
            for k, v in saved.items():
                if isinstance(v, dict) and isinstance(base.get(k), dict):
                    base[k].update(v)
                else:
                    base[k] = v
        except Exception as e:  # damaged file: start clean
            print("state.json unreadable, using defaults:", e)
    state = base
    save_state()


# ----------------------------------------------------------------------------- helpers
def image_version(path: Path):
    """Version and size from the MCUboot image header, or None if the file is not an image."""
    try:
        with open(path, "rb") as f:
            h = f.read(32)
    except OSError:
        return None
    if len(h) < 32 or struct.unpack("<I", h[:4])[0] != 0x96F3B83D:
        return None
    major, minor, patch = h[20], h[21], struct.unpack("<H", h[22:24])[0]
    return f"{major}.{minor}.{patch}", struct.unpack("<I", h[12:16])[0]


def fw_dir() -> Path:
    d = DATA / "firmware"
    d.mkdir(parents=True, exist_ok=True)
    return d


def build_tariff(request: Request, scenario: str):
    s = state
    t = dict(s["tariff"])
    t["service"] = list(s["services"])
    if s["wifi"].get("wifi"):
        t["wifi"] = s["wifi"]["wifi"]
        t["wifiPass"] = s["wifi"]["wifiPass"]
    fw = s["fw"]
    if fw["offer"] and fw["file"] and fw["version"]:
        t["fw_version"] = fw["version"]
        t["fw_url"] = f"https://{request.headers.get('host', 'localhost')}/fw/{fw['file']}"
    if scenario == "weird_types":
        t["PM"] = str(t["PM"])
        t["pri_cost"] = float(t["pri_cost"])
        t["Cost_200"] = t["Cost_200"] + 0.5
        t["samar"] = bool(t["samar"])
        t["korsi"] = bool(t["korsi"])
        t["darbast"] = True
        t["nobat"] = "1"
    elif scenario == "wrapper":
        t = {"taxiFare": t}
    elif scenario == "many_services":
        t["service"] = list(range(201, 216))
    elif scenario == "out_of_range":
        t["pri_cost"] = -5
    elif scenario == "huge_number":
        t["pri_cost"] = 99999999999999
    elif scenario == "pm_zero":
        t["PM"] = 0
    elif scenario == "empty":
        t = {}
    return t


def client_ip(request: Request):
    return request.client.host if request.client else "?"


def log_record(request: Request, kind, serial, params, status, warnings):
    with lock:
        records.append({
            "time": datetime.datetime.now().strftime("%H:%M:%S"),
            "kind": kind, "serial": serial, "remote": client_ip(request),
            "params": params, "status": status, "warnings": warnings,
        })
        del records[:-500]


def serial_allowed(serial):
    al = state.get("allowed_serials") or []
    return not al or serial in al


# ----------------------------------------------------------------------------- record checks
INT_RE = re.compile(r"^-?\d+$")
HM_RE = re.compile(r"^([01]\d|2[0-3]):[0-5]\d$")


def check_end(p):
    w = []
    need = ["driver_id", "serial", "bt_number", "start_time", "stop_time", "trip_distance",
            "trip_duration", "traffic_distance", "traffic_duration", "avg_speed", "max_speed",
            "trip_price", "traffic_price", "night_price", "total_price"]
    for k in need:
        if k not in p:
            w.append(f"missing {k}")
    nums = ["bt_number", "trip_distance", "trip_duration", "traffic_distance", "traffic_duration",
            "avg_speed", "max_speed", "trip_price", "traffic_price", "night_price", "total_price"]
    val = {}
    for k in nums:
        if k in p:
            if not INT_RE.match(p[k]):
                w.append(f"{k} is not an integer: {p[k]!r}")
            elif int(p[k]) < 0:
                w.append(f"{k} is negative")
            else:
                val[k] = int(p[k])
    for k in ("start_time", "stop_time"):
        if k in p and not HM_RE.match(p[k]):
            w.append(f"{k} is not HH:MM: {p[k]!r}")
    if "bt_number" in val and not 1 <= val["bt_number"] <= 5:
        w.append("bt_number must be 1..5")
    if all(k in val for k in ("trip_price", "traffic_price", "total_price")):
        if val["trip_price"] + val["traffic_price"] != val["total_price"]:
            w.append("total_price != trip_price + traffic_price")
    if "night_price" in val and "trip_price" in val and val["night_price"] > val["trip_price"]:
        w.append("night_price is bigger than trip_price")
    if "avg_speed" in val and "max_speed" in val and val["avg_speed"] > val["max_speed"]:
        w.append("avg_speed is bigger than max_speed")
    return w


def check_start(p):
    w = []
    for k in ("serial", "bt_number"):
        if k not in p:
            w.append(f"missing {k}")
    if "bt_number" in p and not (INT_RE.match(p["bt_number"]) and 1 <= int(p["bt_number"]) <= 5):
        w.append("bt_number must be 1..5")
    return w


def check_service(p):
    w = []
    for k in ("serial", "serviceNum", "start"):
        if k not in p:
            w.append(f"missing {k}")
    if "start" in p and p["start"] not in ("1", "2"):
        w.append("start must be 1 or 2")
    if "serviceNum" in p and not INT_RE.match(p["serviceNum"]):
        w.append("serviceNum is not an integer")
    return w


# ----------------------------------------------------------------------------- app
app = FastAPI(title="Taxi meter mock server", docs_url="/admin/docs", redoc_url=None, openapi_url="/admin/openapi.json")


async def api_prelude(request: Request):
    """delay and random failures that apply to every call of the meter"""
    sc = state["scenario"]
    if sc["delay_s"] > 0:
        await asyncio.sleep(min(float(sc["delay_s"]), 120))
    if sc["fail_percent"] > 0 and random.random() * 100 < sc["fail_percent"]:
        return PlainTextResponse("random failure (scenario)", status_code=503)
    return None


@app.get("/api/taxi/TaxiFare")
async def tax_fare(request: Request):
    q = dict(request.query_params)
    serial = q.get("serial", "")
    pre = await api_prelude(request)
    sc = state["scenario"]["tariff"]
    if not serial_allowed(serial):
        log_record(request, "TARIFF", serial, q, 404, ["serial not in the allowed list"])
        return PlainTextResponse("unknown serial", status_code=404)
    if pre is not None:
        log_record(request, "TARIFF", serial, q, 503, ["random failure scenario"])
        return pre
    warn = [] if serial else ["no serial"]
    if sc == "expired":
        log_record(request, "TARIFF", serial, q, 403, warn + ["scenario: expired"])
        return JSONResponse({"error": "license expired"}, status_code=403)
    if sc == "error500":
        log_record(request, "TARIFF", serial, q, 500, warn + ["scenario: 500"])
        return PlainTextResponse("internal error", status_code=500)
    if sc == "html502":
        log_record(request, "TARIFF", serial, q, 502, warn + ["scenario: html 502"])
        return HTMLResponse("<html><body><h1>502 Bad Gateway</h1></body></html>", status_code=502)
    if sc == "timeout":
        log_record(request, "TARIFF", serial, q, 0, warn + ["scenario: timeout"])
        await asyncio.sleep(30)
        return PlainTextResponse("too late")
    body = build_tariff(request, sc)
    if sc == "bad_json":
        text = json.dumps(body)
        log_record(request, "TARIFF", serial, q, 200, warn + ["scenario: broken JSON"])
        return Response(text[: len(text) // 2], media_type="application/json")
    log_record(request, "TARIFF", serial, q, 200, warn + ([f"scenario: {sc}"] if sc != "ok" else []))
    return JSONResponse(body)


async def handle_post(request: Request, kind, checker):
    p = dict(request.query_params)
    # the old firmware sends "?&serial=..": Starlette already ignores the empty first key
    serial = p.get("serial", "")
    pre = await api_prelude(request)
    if not serial_allowed(serial):
        log_record(request, kind, serial, p, 404, ["serial not in the allowed list"])
        return PlainTextResponse("unknown serial", status_code=404)
    if pre is not None:
        log_record(request, kind, serial, p, 503, ["random failure scenario"])
        return pre
    warnings = checker(p)
    sc = state["scenario"]["post"]
    if sc == "error500":
        log_record(request, kind, serial, p, 500, warnings + ["scenario: 500"])
        return PlainTextResponse("internal error", status_code=500)
    if sc == "bad_request":
        log_record(request, kind, serial, p, 400, warnings + ["scenario: 400"])
        return PlainTextResponse("bad request", status_code=400)
    if sc == "timeout":
        log_record(request, kind, serial, p, 0, warnings + ["scenario: timeout"])
        await asyncio.sleep(30)
        return PlainTextResponse("too late")
    if sc == "redirect301":
        log_record(request, kind, serial, p, 301, warnings + ["scenario: 301"])
        return Response(status_code=301, headers={"Location": "/"})
    log_record(request, kind, serial, p, 200, warnings)
    return JSONResponse({"ok": True})


@app.api_route("/api/taxi/SetTaxiStart", methods=["GET", "POST"])
async def set_start(request: Request):
    return await handle_post(request, "START", check_start)


@app.api_route("/api/taxi/SetTaxiEnd", methods=["GET", "POST"])
async def set_end(request: Request):
    return await handle_post(request, "END", check_end)


@app.api_route("/api/taxi/StartService", methods=["GET", "POST"])
async def start_service(request: Request):
    return await handle_post(request, "SERVICE", check_service)


@app.get("/fw/{name}")
async def firmware(name: str, request: Request):
    safe = Path(name).name
    path = fw_dir() / safe
    sc = state["scenario"]["fw"]
    if sc == "not_found" or not path.is_file():
        log_record(request, "FIRMWARE", "", {"file": safe}, 404, ["scenario: 404"] if sc == "not_found" else ["no such file"])
        return PlainTextResponse("not found", status_code=404)
    data = path.read_bytes()
    notes = [] if sc == "ok" else [f"scenario: {sc}"]
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
    if sc == "slow":
        async def gen():
            for i in range(0, len(data), 4096):
                yield data[i:i + 4096]
                await asyncio.sleep(0.2)
        log_record(request, "FIRMWARE", "", {"file": safe, "bytes": len(data)}, 200, notes)
        return StreamingResponse(gen(), media_type="application/octet-stream",
                                 headers={"Content-Length": str(len(data))})
    log_record(request, "FIRMWARE", "", {"file": safe, "bytes": len(data)}, 200, notes)
    return Response(content=data, media_type="application/octet-stream")


# ----------------------------------------------------------------------------- admin
def admin_ok(request: Request):
    if client_ip(request) in ("127.0.0.1", "::1"):
        return True
    tok = (request.headers.get("x-admin-token") or request.cookies.get("admin_token")
           or request.query_params.get("token") or "")
    return secrets.compare_digest(tok, state["admin_token"])


def guard(request: Request):
    if admin_ok(request):
        return None
    return PlainTextResponse("admin: forbidden (use localhost or the admin token)", status_code=403)


@app.get("/")
async def root():
    return PlainTextResponse("taxi mock server. Admin page: /admin")


@app.get("/admin", response_class=HTMLResponse)
async def admin_page(request: Request):
    g = guard(request)
    if g:
        return g
    resp = HTMLResponse(ADMIN_HTML)
    if request.query_params.get("token"):
        resp.set_cookie("admin_token", request.query_params["token"], httponly=True, secure=True, samesite="strict")
    return resp


@app.get("/admin/api/state")
async def admin_state(request: Request):
    g = guard(request)
    if g:
        return g
    with lock:
        files = []
        for p in sorted(fw_dir().iterdir()):
            if p.is_file():
                iv = image_version(p)
                files.append({"name": p.name, "size": p.stat().st_size,
                              "version": iv[0] if iv else None, "is_image": bool(iv)})
        out = dict(state)
        out["files"] = files
        out["scenarios"] = {"tariff": TARIFF_SCENARIOS, "post": POST_SCENARIOS, "fw": FW_SCENARIOS}
        out["records"] = list(reversed(records[-200:]))
        out["host"] = request.headers.get("host", "")
        out["counts"] = {k: sum(1 for r in records if r["kind"] == k) for k in ("TARIFF", "START", "END", "SERVICE", "FIRMWARE")}
        out["problems"] = sum(1 for r in records if r["warnings"] and any(not x.startswith("scenario") for x in r["warnings"]))
        return out


@app.put("/admin/api/tariff")
async def admin_tariff(request: Request):
    g = guard(request)
    if g:
        return g
    try:
        body = await request.json()
        assert isinstance(body, dict)
    except Exception:
        return PlainTextResponse("body must be a JSON object", status_code=400)
    with lock:
        if "tariff" in body:
            state["tariff"] = body["tariff"]
        if "services" in body:
            state["services"] = [int(x) for x in body["services"]]
        if "wifi" in body:
            state["wifi"] = {"wifi": str(body["wifi"].get("wifi", "")), "wifiPass": str(body["wifi"].get("wifiPass", ""))}
        if "allowed_serials" in body:
            state["allowed_serials"] = [str(x) for x in body["allowed_serials"]]
        save_state()
    return {"ok": True}


@app.put("/admin/api/scenario")
async def admin_scenario(request: Request):
    g = guard(request)
    if g:
        return g
    body = await request.json()
    with lock:
        for k in ("tariff", "post", "fw"):
            if k in body:
                allowed = {"tariff": TARIFF_SCENARIOS, "post": POST_SCENARIOS, "fw": FW_SCENARIOS}[k]
                if body[k] not in allowed:
                    return PlainTextResponse(f"unknown {k} scenario", status_code=400)
                state["scenario"][k] = body[k]
        if "delay_s" in body:
            state["scenario"]["delay_s"] = max(0, min(120, float(body["delay_s"])))
        if "fail_percent" in body:
            state["scenario"]["fail_percent"] = max(0, min(100, float(body["fail_percent"])))
        save_state()
    return {"ok": True}


@app.put("/admin/api/fw/upload")
async def admin_fw_upload(request: Request, name: str = "firmware.signed.bin"):
    g = guard(request)
    if g:
        return g
    safe = re.sub(r"[^A-Za-z0-9._-]", "_", Path(name).name) or "firmware.bin"
    data = await request.body()
    if not data or len(data) > 8 * 1024 * 1024:
        return PlainTextResponse("empty or bigger than 8 MB", status_code=400)
    (fw_dir() / safe).write_bytes(data)
    iv = image_version(fw_dir() / safe)
    with lock:
        # the offered version is taken from the image header, so it can not differ from the file
        if iv:
            state["fw"].update({"file": safe, "version": iv[0]})
        save_state()
    return {"ok": True, "name": safe, "size": len(data), "version": iv[0] if iv else None,
            "warning": None if iv else "not an MCUboot image (the meter will refuse it)"}


@app.put("/admin/api/fw/offer")
async def admin_fw_offer(request: Request):
    g = guard(request)
    if g:
        return g
    body = await request.json()
    with lock:
        fw = state["fw"]
        if "file" in body:
            fw["file"] = Path(str(body["file"])).name
            iv = image_version(fw_dir() / fw["file"])
            if iv:
                fw["version"] = iv[0]
        if "version" in body:  # manual override, for the "wrong version" test
            fw["version"] = str(body["version"])
        if "offer" in body:
            fw["offer"] = bool(body["offer"])
        save_state()
    return {"ok": True, "fw": state["fw"]}


@app.delete("/admin/api/records")
async def admin_clear(request: Request):
    g = guard(request)
    if g:
        return g
    with lock:
        records.clear()
    return {"ok": True}


# ----------------------------------------------------------------------------- admin page
ADMIN_HTML = """<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Taxi mock server</title><style>
body{font-family:system-ui,Segoe UI,sans-serif;margin:0;background:#f4f5f7;color:#1c1e21}
header{background:#1f2937;color:#fff;padding:12px 18px;display:flex;gap:18px;align-items:center;flex-wrap:wrap}
header b{font-size:18px} .pill{background:#374151;border-radius:99px;padding:3px 10px;font-size:13px}
.bad{background:#b91c1c} main{display:grid;grid-template-columns:repeat(auto-fit,minmax(380px,1fr));gap:14px;padding:14px}
section{background:#fff;border-radius:8px;padding:14px;box-shadow:0 1px 3px #0002} h2{margin:0 0 8px;font-size:16px}
label{display:block;margin:8px 0 2px;font-size:13px;color:#555} select,input,textarea,button{font:inherit;padding:6px 8px;border:1px solid #bbb;border-radius:6px}
textarea{width:100%;box-sizing:border-box;font-family:Consolas,monospace;font-size:12px} button{background:#2563eb;color:#fff;border:0;cursor:pointer}
button.sec{background:#6b7280} table{border-collapse:collapse;width:100%;font-size:12px} td,th{border-bottom:1px solid #eee;padding:4px 6px;text-align:left;vertical-align:top}
tr.w td{background:#fef2f2} .k{font-weight:600} .wide{grid-column:1/-1} code{background:#eef;padding:1px 4px;border-radius:4px}
.warn{color:#b91c1c} .ok{color:#15803d} small{color:#666}
</style></head><body>
<header><b>Taxi mock server</b><span class="pill" id="host"></span><span class="pill" id="counts"></span><span class="pill" id="problems"></span></header>
<main>
<section><h2>1. Tariff the meter downloads</h2>
<label>Scenario for <code>GET TaxiFare</code></label><select id="sc_tariff"></select> <small id="sc_tariff_d"></small>
<label>Tariff values (JSON)</label><textarea id="tariff" rows="12"></textarea>
<label>Service numbers (SAMR page), comma separated</label><input id="services" style="width:100%">
<label>WiFi sent to the meter (leave empty = do not send)</label><input id="wifi" placeholder="name" style="width:48%"> <input id="wifipass" placeholder="password" style="width:48%">
<label>Only answer these serials (comma separated, empty = everybody)</label><input id="serials" style="width:100%">
<p><button onclick="saveTariff()">Save tariff</button> <span id="msg1"></span></p></section>

<section><h2>2. Trouble makers</h2>
<label>When the meter sends a trip / service record</label><select id="sc_post"></select> <small id="sc_post_d"></small>
<label>Delay every answer (seconds, 0..120)</label><input id="delay" type="number" min="0" max="120" value="0">
<label>Fail this percent of all calls with HTTP 503</label><input id="fail" type="number" min="0" max="100" value="0">
<p><button onclick="saveScenario()">Apply</button> <span id="msg2"></span></p>
<small>The meter must never lose a trip: with errors it keeps the record in flash and sends it later, oldest first.</small></section>

<section><h2>3. Firmware update (OTA)</h2>
<label>Upload <code>zephyr.signed.bin</code> (from <code>build/zephyr-app/zephyr/</code>)</label><input type="file" id="fwfile"> <button class="sec" onclick="upload()">Upload</button>
<div id="files" style="margin:8px 0"></div>
<label><input type="checkbox" id="offer" style="width:auto"> offer this firmware in the tariff</label>
<label>File / version offered (the version is read from the image, change it only to test a wrong version)</label>
<select id="fwsel"></select> <input id="fwver" style="width:90px" placeholder="0.2.0">
<label>What the download does</label><select id="sc_fw"></select> <small id="sc_fw_d"></small>
<p><button onclick="saveFw()">Apply</button> <span id="msg3"></span></p>
<small>The meter downloads only when idle, only a NEWER version, and MCUboot checks the signature.</small></section>

<section class="wide"><h2>4. What the meter sent <button class="sec" onclick="clearRec()" style="float:right">Clear</button></h2>
<table><thead><tr><th>time</th><th>kind</th><th>serial</th><th>from</th><th>HTTP</th><th>data</th><th>problems</th></tr></thead><tbody id="rec"></tbody></table></section>
</main>
<script>
let S=null, busy=false;
const $=id=>document.getElementById(id);
async function api(m,p,b){const r=await fetch(p,{method:m,headers:{'Content-Type':'application/json'},body:b===undefined?undefined:JSON.stringify(b)});
 if(!r.ok) throw new Error(await r.text()); return r.json();}
function fill(sel,map,cur){sel.innerHTML='';for(const k in map){const o=document.createElement('option');o.value=k;o.textContent=k;sel.appendChild(o)};sel.value=cur}
function desc(id,map,sel){$(id).textContent=map[$(sel).value]}
async function load(first){
 S=await api('GET','/admin/api/state');
 $('host').textContent=S.host; $('counts').textContent=Object.entries(S.counts).map(e=>e[0]+' '+e[1]).join('  ');
 $('problems').textContent=S.problems+' record problem(s)'; $('problems').className='pill'+(S.problems?' bad':'');
 if(first){
  fill($('sc_tariff'),S.scenarios.tariff,S.scenario.tariff);fill($('sc_post'),S.scenarios.post,S.scenario.post);fill($('sc_fw'),S.scenarios.fw,S.scenario.fw);
  $('tariff').value=JSON.stringify(S.tariff,null,2);$('services').value=S.services.join(', ');$('wifi').value=S.wifi.wifi;$('wifipass').value=S.wifi.wifiPass;
  $('serials').value=S.allowed_serials.join(', ');$('delay').value=S.scenario.delay_s;$('fail').value=S.scenario.fail_percent;
  $('offer').checked=S.fw.offer;$('fwver').value=S.fw.version;
 }
 const fs=$('fwsel');const cur=fs.value||S.fw.file;fs.innerHTML='<option value="">(none)</option>';
 S.files.forEach(f=>{const o=document.createElement('option');o.value=f.name;o.textContent=f.name;fs.appendChild(o)});fs.value=cur;
 $('files').innerHTML=S.files.length?S.files.map(f=>f.name+' - '+f.size+' bytes - '+(f.is_image?'<span class=ok>image '+f.version+'</span>':'<span class=warn>not an MCUboot image</span>')).join('<br>'):'<small>no file yet</small>';
 desc('sc_tariff_d',S.scenarios.tariff,'sc_tariff');desc('sc_post_d',S.scenarios.post,'sc_post');desc('sc_fw_d',S.scenarios.fw,'sc_fw');
 $('rec').innerHTML=S.records.map(r=>{const w=r.warnings.filter(x=>!x.startsWith('scenario'));
  return '<tr class="'+(w.length?'w':'')+'"><td>'+r.time+'</td><td class=k>'+r.kind+'</td><td>'+(r.serial||'')+'</td><td>'+r.remote+'</td><td>'+r.status+'</td><td>'+
  Object.entries(r.params).filter(e=>e[0]!='serial').map(e=>e[0]+'='+e[1]).join(' ')+'</td><td class=warn>'+r.warnings.join('; ')+'</td></tr>'}).join('');
}
async function saveTariff(){try{
 const t=JSON.parse($('tariff').value);
 await api('PUT','/admin/api/tariff',{tariff:t,services:$('services').value.split(',').map(x=>x.trim()).filter(x=>x).map(Number),
  wifi:{wifi:$('wifi').value,wifiPass:$('wifipass').value},allowed_serials:$('serials').value.split(',').map(x=>x.trim()).filter(x=>x)});
 await api('PUT','/admin/api/scenario',{tariff:$('sc_tariff').value});$('msg1').textContent='saved';setTimeout(()=>$('msg1').textContent='',2000);load(false)}catch(e){$('msg1').textContent=e.message}}
async function saveScenario(){try{await api('PUT','/admin/api/scenario',{post:$('sc_post').value,delay_s:+$('delay').value,fail_percent:+$('fail').value});
 $('msg2').textContent='applied';setTimeout(()=>$('msg2').textContent='',2000);load(false)}catch(e){$('msg2').textContent=e.message}}
async function upload(){const f=$('fwfile').files[0];if(!f)return;
 const r=await fetch('/admin/api/fw/upload?name='+encodeURIComponent(f.name),{method:'PUT',body:f});const j=await r.json();
 $('msg3').textContent=j.warning||('uploaded, version '+j.version);$('fwver').value=j.version||'';await load(false)}
async function saveFw(){try{const b={offer:$('offer').checked};if($('fwsel').value)b.file=$('fwsel').value;if($('fwver').value)b.version=$('fwver').value;
 await api('PUT','/admin/api/fw/offer',b);await api('PUT','/admin/api/scenario',{fw:$('sc_fw').value});$('msg3').textContent='applied';setTimeout(()=>$('msg3').textContent='',2000);load(false)}catch(e){$('msg3').textContent=e.message}}
async function clearRec(){await api('DELETE','/admin/api/records');load(false)}
['sc_tariff','sc_post','sc_fw'].forEach(i=>$(i).onchange=()=>{desc(i+'_d',S.scenarios[i.slice(3)],i)});
load(true);setInterval(()=>load(false),2000);
</script></body></html>"""


# ----------------------------------------------------------------------------- NTP
def ntp_server(bind):
    """Tiny SNTP server: answers with this computer's time."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.bind((bind, 123))
    except OSError as e:
        print(f"NTP: can not use UDP 123 ({e}). Run as administrator or free the port. Continuing without NTP.")
        return
    print("NTP: answering on UDP 123")
    offset = 2208988800
    while True:
        data, addr = s.recvfrom(512)
        if len(data) < 48:
            continue
        now = time.time() + offset
        sec, frac = int(now), int((now % 1) * 2**32)
        reply = bytearray(48)
        reply[0] = (0 << 6) | (3 << 3) | 4          # LI 0, version 3, mode 4 (server)
        reply[1] = 2                                  # stratum
        reply[2] = 6
        reply[3] = 0xEC
        reply[12:16] = b"LOCL"
        reply[16:24] = struct.pack("!II", sec, frac)  # reference
        reply[24:32] = data[40:48]                    # originate = client transmit
        reply[32:40] = struct.pack("!II", sec, frac)  # receive
        reply[40:48] = struct.pack("!II", sec, frac)  # transmit
        s.sendto(bytes(reply), addr)


# ----------------------------------------------------------------------------- TLS certificate
def ensure_cert(ips, names):
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID

    crt, key = DATA / "cert.pem", DATA / "key.pem"
    meta = DATA / "cert.meta"
    want = json.dumps({"ips": sorted(ips), "names": sorted(names)})
    if crt.exists() and key.exists() and meta.exists() and meta.read_text() == want:
        return crt, key
    k = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "taxi-mock-server")])
    san = [x509.DNSName(n) for n in names] + [x509.IPAddress(ipaddress.ip_address(i)) for i in ips]
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(k.public_key())
            .serial_number(x509.random_serial_number()).not_valid_before(now - datetime.timedelta(days=1))
            .not_valid_after(now + datetime.timedelta(days=825))
            .add_extension(x509.SubjectAlternativeName(san), critical=False)
            .sign(k, hashes.SHA256()))
    key.write_bytes(k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL,
                                    serialization.NoEncryption()))
    crt.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    meta.write_text(want)
    print("TLS: new self-signed certificate created for", ", ".join(ips + names))
    return crt, key


def main():
    global DATA
    ap = argparse.ArgumentParser(description="Mock server for the taxi meter (testing only)")
    ap.add_argument("--ip", default="", help="your public/static IP (goes into the certificate and into the hints)")
    ap.add_argument("--port", type=int, default=8443, help="the open TCP port (default 8443)")
    ap.add_argument("--bind", default="0.0.0.0", help="address to listen on (default all)")
    ap.add_argument("--data", default=str(HERE / "data"), help="folder for state, certificate and firmware files")
    ap.add_argument("--ntp", action="store_true", help="also answer NTP on UDP 123 (local network tests)")
    ap.add_argument("--no-tls", action="store_true", help="plain HTTP (only for curl tests, the meter needs HTTPS)")
    a = ap.parse_args()

    DATA = Path(a.data)
    DATA.mkdir(parents=True, exist_ok=True)
    fw_dir()
    load_state()

    kw = {}
    if not a.no_tls:
        ips = list(dict.fromkeys(["127.0.0.1"] + ([a.ip] if a.ip else [])))
        crt, key = ensure_cert(ips, ["localhost"])
        kw = {"ssl_certfile": str(crt), "ssl_keyfile": str(key)}
    scheme = "http" if a.no_tls else "https"
    host = a.ip or "127.0.0.1"
    print("=" * 72)
    print(f"Admin page   : {scheme}://127.0.0.1:{a.port}/admin   (from this PC, no token)")
    print(f"               {scheme}://{host}:{a.port}/admin?token={state['admin_token']}   (from outside)")
    print("Build the meter for this server (west build ... -- <these>):")
    print(f"   -DCONFIG_TAXI_SERVER_HOST=\\\"{host}\\\" -DCONFIG_TAXI_SERVER_PORT={a.port}" +
          (f" -DCONFIG_TAXI_NTP_HOST=\\\"{host}\\\"" if a.ntp else ""))
    print("This server has NO protection for the meter calls: run it only while you test.")
    print("=" * 72, flush=True)
    if a.ntp:
        threading.Thread(target=ntp_server, args=(a.bind,), daemon=True).start()
    uvicorn.run(app, host=a.bind, port=a.port, log_level="warning", **kw)


if __name__ == "__main__":
    main()
