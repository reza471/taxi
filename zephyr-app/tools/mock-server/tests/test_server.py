"""Tests for the mock server.   cd tools/mock-server && python -m unittest discover -s tests -v

The server is started for real (HTTPS on a free port, its own data folder) and spoken to like the meter
does. Tariff JSON is also fed to the meter's own C reader (core/json_lite.c, needs gcc).
"""
import datetime
import http.client
import json
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CORE = ROOT.parent.parent / "core"
sys.path.insert(0, str(ROOT))


def free_port(kind=socket.SOCK_STREAM):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def client_ctx(max12=False, verify=False):
    c = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    c.check_hostname = False
    c.verify_mode = ssl.CERT_REQUIRED if verify else ssl.CERT_NONE   # the meter does not check either
    if max12:
        c.maximum_version = ssl.TLSVersion.TLSv1_2                   # like the meter: TLS 1.2 only
    return c


class Server:
    def __init__(self, data=None, ntp=False):
        self.data = data or tempfile.mkdtemp()
        self.port = free_port()
        self.ntp_port = free_port(socket.SOCK_DGRAM) if ntp else None
        args = [sys.executable, str(ROOT / "server.py"), "--port", str(self.port), "--bind", "127.0.0.1",
                "--data", self.data, "--ip", "127.0.0.1"]
        if ntp:
            args += ["--ntp", "--ntp-port", str(self.ntp_port)]
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for _ in range(200):
            try:
                if self.call("GET", "/")[0] == 200:
                    return
            except OSError:
                time.sleep(0.1)
        self.stop()
        raise RuntimeError("server did not start:\n" + self.proc.stdout.read())

    def call(self, method, path, body=None, headers=None, timeout=15, ctx=None):
        c = http.client.HTTPSConnection("127.0.0.1", self.port, timeout=timeout, context=ctx or client_ctx())
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
            headers = dict(headers or {}, **{"Content-Type": "application/json"})
        c.request(method, path, body=body if body is not None else (b"" if method in ("POST", "PUT") else None),
                  headers=headers or {})
        r = c.getresponse()
        data = r.read()
        hdrs = {k.lower(): v for k, v in r.getheaders()}
        c.close()
        return r.status, data, hdrs

    def api(self, method, path, body=None):
        st, data, _ = self.call(method, path, body)
        if st != 200:
            raise AssertionError(f"{method} {path} -> {st}: {data[:200]!r}")
        return json.loads(data) if data[:1] in (b"{", b"[") else data

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)
        self.proc.stdout.close()


GOOD_END = ("/api/taxi/SetTaxiEnd?driver_id={s}&serial={s}&bt_number=5&start_time=13:05&stop_time=13:40"
            "&trip_distance=4200&trip_duration=2100&traffic_distance=0&traffic_duration=300&avg_speed=7&max_speed=62"
            "&trip_price={tp}&traffic_price=40000&night_price=0&total_price={tot}")


def good_end(serial, extra=0):
    # tariff: pri 26200, Cost_200 1318, Cost_stop 8000, fraction 50 -> 26200 + floor50(27678 + 40000) = 93850
    tot = 93850 + extra
    return GOOD_END.format(s=serial, tp=tot - 40000, tot=tot)


class MeterApiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.s = Server(ntp=True)
        cls.jl = Path(cls.s.data) / "jl_cli"
        subprocess.run(["gcc", "-O1", "-o", str(cls.jl), str(HERE / "jl_cli.c"), str(CORE / "json_lite.c"), "-lm"], check=True)
        cls.defaults = cls.s.api("GET", "/admin/api/state")

    @classmethod
    def tearDownClass(cls):
        cls.s.stop()
        shutil.rmtree(cls.s.data, ignore_errors=True)

    def setUp(self):
        a = self.s.api
        a("PUT", "/admin/api/scenario", {"tariff": "ok", "post": "ok", "fw": "ok", "transport": "normal",
                                         "transport_target": "all", "delay_s": 0, "fail_percent": 0})
        a("PUT", "/admin/api/tariff", {"tariff": self.defaults["tariff"], "services": [211, 222, 233],
                                       "wifi": {"wifi": "", "wifiPass": ""}, "allowed_serials": [], "unregistered": "default"})
        a("PUT", "/admin/api/fw/offer", {"offer": False})
        a("PUT", "/admin/api/tls", {"profile": "rsa"})
        a("PUT", "/admin/api/time", {"offset_s": 0})

    def last(self, kind=None):
        rows = self.s.api("GET", "/admin/api/requests?limit=5" + (f"&kind={kind}" if kind else ""))["rows"]
        return rows[0]

    def tariff(self, serial="AABBCCDDEEFF"):
        return self.s.call("GET", f"/api/taxi/TaxiFare?driver_type=1&serial={serial}")

    def meter_accepts(self, text):
        return subprocess.run([str(self.jl)], input=text if isinstance(text, bytes) else text.encode(),
                              capture_output=True).returncode == 0

    # ------------------------------------------------------------ tariff
    def test_tariff_ok_and_reader(self):
        st, body, h = self.tariff()
        self.assertEqual(st, 200)
        j = json.loads(body)
        self.assertEqual((j["PM"], j["service"]), (46, [211, 222, 233]))
        self.assertEqual(int(h["content-length"]), len(body))
        self.assertTrue(self.meter_accepts(body))

    def test_tariff_scenarios(self):
        for sc, code in (("expired", 403), ("error500", 500), ("html502", 502), ("redirect302", 302)):
            self.s.api("PUT", "/admin/api/scenario", {"tariff": sc})
            st, body, h = self.tariff()
            self.assertEqual(st, code, sc)
            if code == 302:
                self.assertIn("location", h)
        for sc in ("weird_types", "wrapper", "many_services", "out_of_range", "huge_number"):
            self.s.api("PUT", "/admin/api/scenario", {"tariff": sc})
            st, body, _ = self.tariff()
            self.assertEqual(st, 200)
            json.loads(body)
            self.assertTrue(self.meter_accepts(body), sc)  # parsed; range checks refuse some later
        for sc in ("bad_json", "empty", "pm_zero"):
            self.s.api("PUT", "/admin/api/scenario", {"tariff": sc})
            self.assertFalse(self.meter_accepts(self.tariff()[1]), sc)

    # ------------------------------------------------------------ records
    def test_good_trip_passes_fare_check(self):
        self.s.call("POST", "/api/taxi/SetTaxiStart?serial=FA0000000001&bt_number=5")
        self.assertEqual(self.s.call("POST", good_end("FA0000000001"))[0], 200)
        r = self.last("END")
        self.assertEqual(r["problems"], [], r)
        trip = self.s.api("GET", "/admin/api/trips?serial=FA0000000001")["rows"][0]
        self.assertTrue(trip["fare_check"].startswith("ok"), trip["fare_check"])

    def test_wrong_fare_duplicate_and_order(self):
        sn = "FA0000000002"
        self.s.call("POST", f"/api/taxi/SetTaxiStart?serial={sn}&bt_number=5")
        self.s.call("POST", good_end(sn, extra=9000))
        self.assertTrue(any("fare mismatch" in p for p in self.last("END")["problems"]))
        self.s.call("POST", good_end(sn, extra=9000))  # the meter sent it again
        self.assertTrue(any("duplicate" in p for p in self.last("END")["problems"]))
        rows = self.s.api("GET", f"/admin/api/trips?serial={sn}")["rows"]
        self.assertIsNotNone(rows[0]["duplicate_of"])
        self.s.call("POST", good_end("FA0000000003"))  # END with no START before
        self.assertTrue(any("END without START" in n for n in self.last("END")["notes"]))
        self.s.call("POST", f"/api/taxi/SetTaxiStart?serial={sn}&bt_number=2")
        self.s.call("POST", f"/api/taxi/SetTaxiStart?serial={sn}&bt_number=2")
        self.assertTrue(any("START again" in p for p in self.last("START")["problems"]))

    def test_end_field_checks_and_service(self):
        self.s.call("POST", good_end("FA0000000004").replace("start_time=13:05", "start_time=1305")
                    .replace("total_price=93850", "total_price=1"))
        p = " | ".join(self.last("END")["problems"])
        self.assertIn("start_time is not HH:MM", p)
        self.assertIn("total_price != trip_price + traffic_price", p)
        self.s.call("POST", "/api/taxi/SetTaxiEnd?serial=X&bt_number=9")
        self.assertIn("bt_number must be 1..5", " ".join(self.last("END")["problems"]))
        self.s.call("POST", "/api/taxi/StartService?&serial=AB&serviceNum=211&start=1")  # old "?&" quirk
        r = self.last("SERVICE")
        self.assertEqual((r["problems"], r["params"]["serviceNum"]), ([], "211"))

    def test_post_scenarios(self):
        for sc, code in (("error500", 500), ("bad_request", 400), ("redirect301", 301)):
            self.s.api("PUT", "/admin/api/scenario", {"post": sc})
            self.assertEqual(self.s.call("POST", "/api/taxi/SetTaxiStart?serial=AB&bt_number=1")[0], code, sc)

    def test_delay_and_random_failure(self):
        self.s.api("PUT", "/admin/api/scenario", {"delay_s": 1})
        t0 = time.time()
        self.tariff()
        self.assertGreaterEqual(time.time() - t0, 0.9)
        self.s.api("PUT", "/admin/api/scenario", {"delay_s": 0, "fail_percent": 100})
        self.assertEqual(self.tariff()[0], 503)

    # ------------------------------------------------------------ transport faults
    def raw(self, path, timeout=8):
        """send a request over TLS and return everything the server sent before it closed"""
        sock = socket.create_connection(("127.0.0.1", self.s.port), timeout=timeout)
        t = client_ctx(max12=True).wrap_socket(sock)
        t.sendall(f"GET {path} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n".encode())
        out = b""
        try:
            while True:
                b = t.recv(65536)
                if not b:
                    break
                out += b
        finally:
            t.close()
        return out

    def test_transport_modes(self):
        p = "/api/taxi/TaxiFare?driver_type=1&serial=TT0000000001"
        normal = json.loads(self.tariff("TT0000000001")[1])
        self.s.api("PUT", "/admin/api/scenario", {"transport": "chunked"})
        st, body, h = self.tariff("TT0000000001")
        self.assertEqual(h.get("transfer-encoding"), "chunked")
        self.assertEqual(json.loads(body)["PM"], normal["PM"])
        self.s.api("PUT", "/admin/api/scenario", {"transport": "close_delimited"})
        out = self.raw(p)
        head, body = out.split(b"\r\n\r\n", 1)
        self.assertNotIn(b"content-length", head.lower())
        self.assertEqual(json.loads(body)["PM"], normal["PM"])
        self.s.api("PUT", "/admin/api/scenario", {"transport": "partial"})
        with self.assertRaises((http.client.IncompleteRead, ConnectionError, ssl.SSLError)):
            self.tariff("TT0000000001")
        self.s.api("PUT", "/admin/api/scenario", {"transport": "drop"})
        with self.assertRaises((http.client.RemoteDisconnected, ConnectionError, ssl.SSLError)):
            self.tariff("TT0000000001")
        self.s.api("PUT", "/admin/api/scenario", {"transport": "reset"})
        with self.assertRaises((ConnectionError, http.client.RemoteDisconnected, ssl.SSLError, OSError)):
            self.tariff("TT0000000001")
        self.s.api("PUT", "/admin/api/scenario", {"transport": "stall"})
        with self.assertRaises((socket.timeout, TimeoutError)):
            self.s.call("GET", p, timeout=2)
        # only for the firmware: the tariff stays normal
        self.s.api("PUT", "/admin/api/scenario", {"transport": "drop", "transport_target": "firmware"})
        self.assertEqual(self.tariff("TT0000000001")[0], 200)
        r = self.last("TARIFF")
        self.assertEqual(r["fault"], "")

    def test_fault_is_logged(self):
        self.s.api("PUT", "/admin/api/scenario", {"transport": "drop"})
        try:
            self.tariff("TT0000000002")
        except Exception:
            pass
        time.sleep(0.3)
        r = self.last("TARIFF")
        self.assertEqual((r["fault"], r["status"]), ("drop", 0))

    # ------------------------------------------------------------ firmware
    def image(self, ver=(0, 2, 5), size=24000):
        h = bytearray(32)
        h[0:4] = struct.pack("<I", 0x96F3B83D)
        h[8:10] = struct.pack("<H", 0x20)
        h[12:16] = struct.pack("<I", size)
        h[20], h[21] = ver[0], ver[1]
        h[22:24] = struct.pack("<H", ver[2])
        return bytes(h) + bytes(range(256)) * (size // 256)

    def test_firmware(self):
        img = self.image()
        r = self.s.api("PUT", "/admin/api/fw/upload?name=taxi-0.2.5.signed.bin", img)
        self.assertEqual(r["version"], "0.2.5")
        self.assertNotIn("fw_url", json.loads(self.tariff()[1]))
        self.s.api("PUT", "/admin/api/fw/offer", {"offer": True})
        j = json.loads(self.tariff()[1])
        self.assertEqual((j["fw_version"], j["fw_url"]), ("0.2.5", f"https://127.0.0.1:{self.s.port}/fw/taxi-0.2.5.signed.bin"))
        self.assertTrue(self.meter_accepts(json.dumps(j)))
        path = "/fw/taxi-0.2.5.signed.bin"
        self.assertEqual(self.s.call("GET", path)[1], img)
        for sc, ok in (("truncated", lambda d: len(d) == len(img) // 2),
                       ("corrupt", lambda d: len(d) == len(img) and d != img and d[:32] == img[:32]),
                       ("wrong_magic", lambda d: d[0] != img[0] and d[1:] == img[1:])):
            self.s.api("PUT", "/admin/api/scenario", {"fw": sc})
            self.assertTrue(ok(self.s.call("GET", path)[1]), sc)
        for sc, code in (("not_found", 404), ("redirect302", 302)):
            self.s.api("PUT", "/admin/api/scenario", {"fw": sc})
            self.assertEqual(self.s.call("GET", path)[0], code)
        self.s.api("PUT", "/admin/api/scenario", {"fw": "ok"})
        self.assertEqual(self.s.call("GET", "/fw/..%2f..%2fserver.py")[0], 404)
        self.assertEqual(self.s.call("GET", "/fw/%2e%2e%2ftaxi.db")[0], 404)
        r = self.s.api("PUT", "/admin/api/fw/upload?name=junk.bin", b"nope" * 10)
        self.assertIsNone(r["version"])
        self.assertEqual(self.s.call("PUT", "/admin/api/fw/upload?name=e.bin", b"")[0], 400)

    # ------------------------------------------------------------ TLS profiles
    def handshake(self, ctx):
        sock = socket.create_connection(("127.0.0.1", self.s.port), timeout=8)
        try:
            t = ctx.wrap_socket(sock, server_hostname="localhost")
            info = (t.version(), t.cipher()[0], t.getpeercert(binary_form=True))
            t.close()
            return info
        except Exception:
            sock.close()
            raise

    def test_tls_profiles(self):
        from cryptography import x509
        m = client_ctx(max12=True)
        self.s.api("PUT", "/admin/api/tls", {"profile": "rsa"})
        self.assertEqual(self.handshake(client_ctx())[0], "TLSv1.3")
        self.assertEqual(self.handshake(m)[0], "TLSv1.2")
        self.s.api("PUT", "/admin/api/tls", {"profile": "ecdhe_gcm_only"})
        v, cipher, _ = self.handshake(m)
        self.assertIn("ECDHE", cipher)
        self.assertIn("GCM", cipher)
        self.s.api("PUT", "/admin/api/tls", {"profile": "ecdsa"})
        self.assertIn("ECDSA", self.handshake(m)[1])
        self.s.api("PUT", "/admin/api/tls", {"profile": "expired"})
        cert = x509.load_der_x509_certificate(self.handshake(m)[2])
        self.assertLess(cert.not_valid_after_utc, datetime.datetime.now(datetime.timezone.utc))
        self.s.api("PUT", "/admin/api/tls", {"profile": "bigchain"})
        self.assertEqual(self.handshake(m)[0], "TLSv1.2")
        info = self.s.api("GET", "/admin/api/tls")["info"]["bigchain"]
        self.assertGreater(info["chain_bytes"], 8192)  # bigger than the meter's TLS buffer
        self.s.api("PUT", "/admin/api/tls", {"profile": "tls13only"})
        with self.assertRaises(ssl.SSLError):
            self.handshake(m)  # a TLS-1.2-only client (the meter) can not connect
        time.sleep(0.3)
        fails = self.s.api("GET", "/admin/api/requests?kind=TLS&limit=3")["rows"]
        self.assertTrue(fails and "tls13only" in fails[0]["problems"][0])
        self.s.api("PUT", "/admin/api/tls", {"profile": "rsa"})

    def test_browser_cert_alert_is_not_logged(self):
        before = self.s.api("GET", "/admin/api/requests?kind=TLS&limit=1")["total"]
        with self.assertRaises(ssl.SSLError):
            self.handshake(client_ctx(verify=True))  # rejects the self-signed certificate, like a browser
        time.sleep(0.3)
        self.assertEqual(self.s.api("GET", "/admin/api/requests?kind=TLS&limit=1")["total"], before)

    # ------------------------------------------------------------ drivers
    def test_drivers(self):
        a = self.s.api
        pid = a("POST", "/admin/api/profiles", {"name": "Airport-T", "tariff": {"pri_cost": 40000, "Cost_200": 2000}})["id"]
        d = a("POST", "/admin/api/drivers", {"name": "Test Driver", "serial": "dd:00:00:00:00:01", "profile_id": pid,
                                            "overrides": '{"korsi": 0}', "services": "501, 502",
                                            "wifi": {"wifi": "Net1", "wifiPass": "pw123456"}})["driver"]
        self.assertEqual(d["serial"], "DD0000000001")
        j = json.loads(self.tariff("DD0000000001")[1])
        self.assertEqual((j["pri_cost"], j["Cost_200"], j["korsi"], j["service"], j["wifi"]),
                         (40000, 2000, 0, [501, 502], "Net1"))
        self.assertEqual(json.loads(self.tariff("DD0000000099")[1])["pri_cost"], 26200)  # other meter: main tariff
        st, body, _ = self.s.call("POST", "/admin/api/drivers", {"name": "Other", "serial": "DD0000000001"})
        self.assertEqual(st, 400)
        yesterday = (datetime.date.today() - datetime.timedelta(days=1)).isoformat()
        a("PUT", f"/admin/api/drivers/{d['id']}", {"license_until": yesterday})
        st, body, _ = self.tariff("DD0000000001")
        self.assertEqual(st, 403)
        self.assertIn("ended", body.decode())
        a("PUT", f"/admin/api/drivers/{d['id']}", {"license_until": "", "active": False})
        self.assertEqual(self.tariff("DD0000000001")[0], 403)
        a("PUT", f"/admin/api/drivers/{d['id']}", {"active": True})
        self.assertEqual(self.tariff("DD0000000001")[0], 200)
        prev = a("GET", f"/admin/api/drivers/{d['id']}/preview")
        self.assertEqual(prev["source"], ["main tariff", "profile 'Airport-T'", "driver's own prices"])
        # trips are linked to the driver and checked against HIS prices
        self.s.call("POST", "/api/taxi/SetTaxiStart?serial=DD0000000001&bt_number=5")
        tot = 40000 + (4200 * 2000 // 200 + 40000) // 50 * 50
        self.s.call("POST", GOOD_END.format(s="DD0000000001", tp=tot - 40000, tot=tot))
        trip = a("GET", f"/admin/api/trips?driver={d['id']}")["rows"][0]
        self.assertEqual(trip["driver"], "Test Driver")
        self.assertTrue(trip["fare_check"].startswith("ok"), trip["fare_check"])
        self.assertEqual(a("GET", "/admin/api/drivers")["rows"][0]["trips"] >= 1, True)
        # unregistered meters
        for mode, code in (("expired", 403), ("refuse", 404), ("default", 200)):
            a("PUT", "/admin/api/tariff", {"unregistered": mode})
            self.assertEqual(self.tariff("DD0000000077")[0], code, mode)
        # deleting the profile sends the driver back to the main tariff (plus his own changes)
        a("DELETE", f"/admin/api/profiles/{pid}")
        self.assertEqual(json.loads(self.tariff("DD0000000001")[1])["pri_cost"], 26200)
        for bad in ({"name": ""}, {"name": "x", "license_until": "soon"}, {"name": "x", "overrides": "[1]"},
                    {"name": "x", "services": "a,b"}, {"name": "x", "profile_id": 9999}, {"name": "x", "serial": "PP0000000001"},
                    {"name": "x", "serial": "AABB"}):
            self.assertEqual(self.s.call("POST", "/admin/api/drivers", bad)[0], 400, bad)
        a("DELETE", f"/admin/api/drivers/{d['id']}")

    def test_device_expired_and_override(self):
        self.tariff("EE0000000001")
        self.s.api("PUT", "/admin/api/devices/EE0000000001", {"tariff_override": {"pri_cost": 1234}})
        self.assertEqual(json.loads(self.tariff("EE0000000001")[1])["pri_cost"], 1234)
        self.s.api("PUT", "/admin/api/devices/EE0000000001", {"expired": True})
        self.assertEqual(self.tariff("EE0000000001")[0], 403)
        self.assertEqual(self.tariff("EE0000000002")[0], 200)

    # ------------------------------------------------------------ time / NTP
    def test_ntp_offset(self):
        def ask():
            c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            c.settimeout(3)
            p = bytearray(48)
            p[0] = 0x1B
            c.sendto(bytes(p), ("127.0.0.1", self.s.ntp_port))
            data = c.recvfrom(100)[0]
            c.close()
            return struct.unpack("!I", data[40:44])[0] - 2208988800
        self.assertLess(abs(ask() - time.time()), 3)
        r = self.s.api("PUT", "/admin/api/time", {"target": "21:58"})
        want = datetime.datetime.now().replace(hour=21, minute=58, second=0, microsecond=0).timestamp()
        self.assertLess(abs(ask() - want), 5)
        self.s.api("PUT", "/admin/api/time", {"offset_s": 0})
        self.assertEqual(self.s.call("PUT", "/admin/api/time", {"target": "25:00"})[0], 400)

    # ------------------------------------------------------------ admin
    def test_admin_page_summary_csv(self):
        st, body, _ = self.s.call("GET", "/admin")
        self.assertEqual(st, 200)
        self.assertIn(b"Taxi Mock", body)
        self.assertEqual(self.s.call("GET", "/admin/static/app.js")[0], 200)
        s = self.s.api("GET", "/admin/api/summary")
        self.assertEqual((len(s["per_min"]), len(s["days"])), (60, 14))
        for name in ("requests", "trips", "devices", "drivers"):
            st, body, h = self.s.call("GET", f"/admin/api/export/{name}.csv")
            self.assertEqual(st, 200)
            self.assertIn("text/csv", h["content-type"])

    def test_admin_guard(self):
        from taxi_mock.admin import create_admin
        from taxi_mock.db import DB
        from taxi_mock.state import State
        tmp = Path(tempfile.mkdtemp())
        db = DB(tmp / "t.db")
        st = State(db)
        st.s["admin_token"] = "secret-token"
        app = create_admin({"db": db, "state": st, "tls": None, "ntp": None, "fw_dir": tmp, "web_dir": ROOT / "web",
                            "host": "x", "port": 1, "meter": None})
        ok = app.state.admin_ok

        class Req:
            def __init__(self, host, headers=None, cookies=None, q=None):
                self.client = type("C", (), {"host": host})()
                self.headers, self.cookies, self.query_params = headers or {}, cookies or {}, q or {}
        self.assertTrue(ok(Req("127.0.0.1")))
        self.assertTrue(ok(Req("::1")))
        self.assertFalse(ok(Req("203.0.113.9")))
        self.assertFalse(ok(Req("203.0.113.9", headers={"x-admin-token": "wrong"})))
        self.assertFalse(ok(Req("203.0.113.9", q={"token": ""})))
        self.assertTrue(ok(Req("203.0.113.9", headers={"x-admin-token": "secret-token"})))
        self.assertTrue(ok(Req("203.0.113.9", cookies={"admin_token": "secret-token"})))
        shutil.rmtree(tmp, ignore_errors=True)


class PersistenceTest(unittest.TestCase):
    def test_everything_survives_a_restart(self):
        data = tempfile.mkdtemp()
        s = Server(data)
        try:
            s.api("PUT", "/admin/api/tariff", {"tariff": dict(s.api("GET", "/admin/api/state")["tariff"], pri_cost=31000)})
            s.api("POST", "/admin/api/drivers", {"name": "Keep Me", "serial": "AF00000000F1"})
            s.call("POST", "/api/taxi/SetTaxiStart?serial=AF00000000F1&bt_number=5")
            s.call("POST", good_end("AF00000000F1"))
            s.api("PUT", "/admin/api/scenario", {"tariff": "expired"})
        finally:
            s.stop()
        s = Server(data)
        try:
            self.assertEqual(s.api("GET", "/admin/api/state")["tariff"]["pri_cost"], 31000)
            self.assertEqual(s.api("GET", "/admin/api/state")["scenario"]["tariff"], "expired")
            self.assertEqual(s.api("GET", "/admin/api/drivers")["rows"][0]["name"], "Keep Me")
            self.assertGreaterEqual(s.api("GET", "/admin/api/requests?limit=10")["total"], 2)
            self.assertEqual(s.api("GET", "/admin/api/trips")["rows"][0]["driver"], "Keep Me")
        finally:
            s.stop()
            shutil.rmtree(data, ignore_errors=True)


class FareCheckTest(unittest.TestCase):
    def test_against_meter_rules(self):
        from taxi_mock.fare import check
        from taxi_mock.state import DEFAULT_TARIFF as T
        r = dict(bt=5, distance=4200, duration=2100, wait=300, avg_speed=7, max_speed=60,
                 trip_price=53850, traffic_price=40000, night_price=0, total=93850)
        self.assertEqual(check(T, r)["verdict"], "ok")
        self.assertEqual(check(T, dict(r, total=99850, trip_price=59850))["verdict"], "mismatch")
        self.assertEqual(check(T, dict(r, avg_speed=30))["verdict"], "mismatch")
        self.assertEqual(check({}, r)["verdict"], "unchecked")


if __name__ == "__main__":
    unittest.main()
