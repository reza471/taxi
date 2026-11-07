"""Tests for the mock server. Run:  python -m unittest discover -s tests -v   (from tools/mock-server)

Starts the server with HTTPS on a free port and talks to it like the meter does.
The tariff JSON of every scenario is also given to the meter's own C reader (core/json_lite.c).
"""
import json
import os
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CORE = ROOT.parent.parent / "core"
CTX = ssl._create_unverified_context()  # the meter does not check the certificate either


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def call(base, method, path, body=None, headers=None, timeout=15):
    req = urllib.request.Request(base + path, method=method, data=body, headers=headers or {})
    try:
        with urllib.request.urlopen(req, context=CTX, timeout=timeout) as r:
            return r.status, r.read(), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), dict(e.headers)


class ServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.port = free_port()
        cls.base = f"https://127.0.0.1:{cls.port}"
        cls.proc = subprocess.Popen(
            [sys.executable, str(ROOT / "server.py"), "--port", str(cls.port), "--bind", "127.0.0.1",
             "--data", cls.tmp, "--ip", "127.0.0.1"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for _ in range(100):
            try:
                if call(cls.base, "GET", "/")[0] == 200:
                    break
            except Exception:
                time.sleep(0.2)
        else:
            cls.proc.kill()
            raise RuntimeError("server did not start:\n" + cls.proc.stdout.read())
        cls.defaults = call(cls.base, "GET", "/admin/api/state")
        cls.defaults = json.loads(cls.defaults[1])
        # the meter's JSON reader as a command line tool
        cls.jl = Path(cls.tmp) / "jl_cli"
        subprocess.run(["gcc", "-O1", "-o", str(cls.jl), str(HERE / "jl_cli.c"), str(CORE / "json_lite.c"), "-lm"], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        try:
            cls.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:  # a "timeout" scenario request may still be sleeping
            cls.proc.kill()
            cls.proc.wait(timeout=5)
        cls.proc.stdout.close()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        self.admin("PUT", "/admin/api/tariff", {"tariff": self.defaults["tariff"], "services": self.defaults["services"],
                                                "wifi": {"wifi": "", "wifiPass": ""}, "allowed_serials": []})
        self.admin("PUT", "/admin/api/scenario", {"tariff": "ok", "post": "ok", "fw": "ok", "delay_s": 0, "fail_percent": 0})
        self.admin("PUT", "/admin/api/fw/offer", {"offer": False})
        self.admin("DELETE", "/admin/api/records")
        self.admin("PUT", "/admin/api/tariff", {"allowed_serials": []})

    def admin(self, method, path, obj=None):
        st, body, _ = call(self.base, method, path, None if obj is None else json.dumps(obj).encode(),
                           {"Content-Type": "application/json"})
        self.assertEqual(st, 200, body)
        return json.loads(body)

    def state(self):
        return self.admin("GET", "/admin/api/state")

    def meter_accepts(self, text):
        return subprocess.run([str(self.jl)], input=text.encode(), capture_output=True).returncode == 0

    def tariff(self, scenario):
        self.admin("PUT", "/admin/api/scenario", {"tariff": scenario})
        return call(self.base, "GET", "/api/taxi/TaxiFare?driver_type=1&serial=AABBCCDDEEFF")

    # ---------------------------------------------------------------- tariff
    def test_tariff_ok(self):
        st, body, h = self.tariff("ok")
        self.assertEqual(st, 200)
        j = json.loads(body)
        self.assertEqual(j["PM"], 46)
        self.assertEqual(j["service"], [211, 222, 233])
        self.assertIn("Content-Length", {k.title(): v for k, v in h.items()})
        self.assertTrue(self.meter_accepts(body.decode()))

    def test_tariff_error_scenarios(self):
        self.assertEqual(self.tariff("expired")[0], 403)
        self.assertEqual(self.tariff("error500")[0], 500)
        st, body, _ = self.tariff("html502")
        self.assertEqual(st, 502)
        self.assertFalse(self.meter_accepts(body.decode()))

    def test_tariff_json_variants_vs_meter_reader(self):
        """the meter must understand the sloppy-but-valid variants and refuse the broken ones"""
        for sc in ("weird_types", "wrapper", "many_services", "out_of_range", "huge_number"):
            st, body, _ = self.tariff(sc)
            self.assertEqual(st, 200, sc)
            json.loads(body)  # valid JSON
            self.assertTrue(self.meter_accepts(body.decode()), f"{sc}: reader must parse it (range checks refuse it later)")
        for sc in ("bad_json", "empty", "pm_zero"):
            st, body, _ = self.tariff(sc)
            self.assertEqual(st, 200, sc)
            self.assertFalse(self.meter_accepts(body.decode()), f"{sc}: must be refused")

    def test_tariff_edit_and_wifi_and_serial_filter(self):
        t = dict(self.state()["tariff"])
        t["pri_cost"] = 31000
        self.admin("PUT", "/admin/api/tariff", {"tariff": t, "services": [5, 6],
                                                "wifi": {"wifi": "Lab", "wifiPass": "pw12345678"}})
        j = json.loads(self.tariff("ok")[1])
        self.assertEqual(j["pri_cost"], 31000)
        self.assertEqual(j["service"], [5, 6])
        self.assertEqual((j["wifi"], j["wifiPass"]), ("Lab", "pw12345678"))
        self.admin("PUT", "/admin/api/tariff", {"allowed_serials": ["ONLY"]})
        self.assertEqual(call(self.base, "GET", "/api/taxi/TaxiFare?driver_type=1&serial=OTHER")[0], 404)
        self.assertEqual(call(self.base, "GET", "/api/taxi/TaxiFare?driver_type=1&serial=ONLY")[0], 200)
        self.admin("PUT", "/admin/api/tariff", {"allowed_serials": [], "wifi": {"wifi": "", "wifiPass": ""}})

    def test_tariff_timeout_and_delay(self):
        self.admin("PUT", "/admin/api/scenario", {"delay_s": 1})
        t0 = time.time()
        self.assertEqual(call(self.base, "GET", "/api/taxi/TaxiFare?driver_type=1&serial=X")[0], 200)
        self.assertGreaterEqual(time.time() - t0, 0.9)
        self.admin("PUT", "/admin/api/scenario", {"delay_s": 0, "tariff": "timeout"})
        with self.assertRaises(Exception):
            call(self.base, "GET", "/api/taxi/TaxiFare?driver_type=1&serial=X", timeout=2)

    # ---------------------------------------------------------------- records
    GOOD_END = ("/api/taxi/SetTaxiEnd?driver_id=AABBCCDDEEFF&serial=AABBCCDDEEFF&bt_number=5&start_time=13:05"
                "&stop_time=13:40&trip_distance=4200&trip_duration=2100&traffic_distance=0&traffic_duration=300"
                "&avg_speed=7&max_speed=62&trip_price=52600&traffic_price=4000&night_price=0&total_price=56600")

    def last(self):
        return self.state()["records"][0]

    def test_trip_end_good_and_bad(self):
        st, _, _ = call(self.base, "POST", self.GOOD_END, b"", {"Content-Length": "0"})
        self.assertEqual(st, 200)
        self.assertEqual(self.last()["warnings"], [])
        bad = self.GOOD_END.replace("total_price=56600", "total_price=99999").replace("start_time=13:05", "start_time=1305")
        call(self.base, "POST", bad, b"")
        w = " | ".join(self.last()["warnings"])
        self.assertIn("total_price != trip_price + traffic_price", w)
        self.assertIn("start_time is not HH:MM", w)
        call(self.base, "POST", "/api/taxi/SetTaxiEnd?serial=X&bt_number=9", b"")
        w = " | ".join(self.last()["warnings"])
        self.assertIn("missing trip_price", w)
        self.assertIn("bt_number must be 1..5", w)
        self.assertGreaterEqual(self.state()["problems"], 2)

    def test_start_and_service_and_old_quirk(self):
        call(self.base, "POST", "/api/taxi/SetTaxiStart?serial=AB&bt_number=3", b"")
        self.assertEqual((self.last()["kind"], self.last()["warnings"]), ("START", []))
        # the old firmware wrote "StartService?&serial=..": must still work
        call(self.base, "POST", "/api/taxi/StartService?&serial=AB&serviceNum=211&start=1", b"")
        r = self.last()
        self.assertEqual((r["kind"], r["warnings"], r["params"]["serviceNum"]), ("SERVICE", [], "211"))
        call(self.base, "POST", "/api/taxi/StartService?&serial=AB&serviceNum=211&start=7", b"")
        self.assertIn("start must be 1 or 2", self.last()["warnings"])

    def test_post_scenarios(self):
        for sc, code in (("error500", 500), ("bad_request", 400), ("redirect301", 301)):
            self.admin("PUT", "/admin/api/scenario", {"post": sc})
            req = urllib.request.Request(self.base + self.GOOD_END, method="POST", data=b"")

            class NoRedirect(urllib.request.HTTPRedirectHandler):
                def redirect_request(self, *a, **k):
                    return None
            opener = urllib.request.build_opener(urllib.request.HTTPSHandler(context=CTX), NoRedirect)
            try:
                st = opener.open(req, timeout=10).status
            except urllib.error.HTTPError as e:
                st = e.code
            self.assertEqual(st, code, sc)
        self.admin("PUT", "/admin/api/scenario", {"post": "ok"})

    # ---------------------------------------------------------------- firmware
    def make_image(self, version=(0, 2, 0), size=2000):
        h = bytearray(32)
        h[0:4] = struct.pack("<I", 0x96F3B83D)
        h[8:10] = struct.pack("<H", 0x20)
        h[12:16] = struct.pack("<I", size)
        h[20], h[21] = version[0], version[1]
        h[22:24] = struct.pack("<H", version[2])
        return bytes(h) + bytes(range(256)) * (size // 256)

    def test_firmware_offer_and_download_scenarios(self):
        img = self.make_image((0, 2, 5), size=24000)  # 24 KB: the slow scenario needs several 4 KB pieces
        r = json.loads(call(self.base, "PUT", "/admin/api/fw/upload?name=taxi-0.2.5.signed.bin", img)[1])
        self.assertEqual(r["version"], "0.2.5")
        self.assertEqual(self.state()["fw"]["version"], "0.2.5")
        # not offered yet
        self.assertNotIn("fw_url", json.loads(self.tariff("ok")[1]))
        self.admin("PUT", "/admin/api/fw/offer", {"offer": True})
        j = json.loads(self.tariff("ok")[1])
        self.assertEqual(j["fw_version"], "0.2.5")
        self.assertEqual(j["fw_url"], f"https://127.0.0.1:{self.port}/fw/taxi-0.2.5.signed.bin")
        self.assertTrue(self.meter_accepts(json.dumps(j)))
        path = "/fw/taxi-0.2.5.signed.bin"
        self.assertEqual(call(self.base, "GET", path)[1], img)
        for sc, check in (("truncated", lambda d: len(d) == len(img) // 2),
                          ("corrupt", lambda d: len(d) == len(img) and d != img and d[:32] == img[:32]),
                          ("wrong_magic", lambda d: d[0] != img[0] and d[1:] == img[1:])):
            self.admin("PUT", "/admin/api/scenario", {"fw": sc})
            self.assertTrue(check(call(self.base, "GET", path)[1]), sc)
        self.admin("PUT", "/admin/api/scenario", {"fw": "not_found"})
        self.assertEqual(call(self.base, "GET", path)[0], 404)
        self.admin("PUT", "/admin/api/scenario", {"fw": "slow"})
        t0 = time.time()
        self.assertEqual(call(self.base, "GET", path)[1], img)
        self.assertGreater(time.time() - t0, 0.3)
        self.admin("PUT", "/admin/api/scenario", {"fw": "ok"})
        # path tricks must not leave the firmware folder
        self.assertEqual(call(self.base, "GET", "/fw/..%2f..%2fserver.py")[0], 404)
        self.assertEqual(call(self.base, "GET", "/fw/%2e%2e/state.json")[0], 404)

    def test_firmware_non_image_and_manual_version(self):
        r = json.loads(call(self.base, "PUT", "/admin/api/fw/upload?name=junk.bin", b"not an image at all" * 5)[1])
        self.assertIsNone(r["version"])
        self.assertIn("not an MCUboot image", r["warning"])
        self.assertEqual(call(self.base, "PUT", "/admin/api/fw/upload?name=empty.bin", b"")[0], 400)
        call(self.base, "PUT", "/admin/api/fw/upload?name=a.bin", self.make_image((1, 0, 0)))
        self.admin("PUT", "/admin/api/fw/offer", {"file": "a.bin", "version": "9.9.9", "offer": True})
        self.assertEqual(json.loads(self.tariff("ok")[1])["fw_version"], "9.9.9")  # the "wrong version" test

    # ---------------------------------------------------------------- admin protection
    def test_admin_needs_localhost_or_token(self):
        # from localhost: allowed (all the other tests rely on it); a wrong token does not matter there
        self.assertEqual(call(self.base, "GET", "/admin/api/state")[0], 200)
        # simulate "from outside" by asking the server's own guard function through a different source address
        import importlib.util
        spec = importlib.util.spec_from_file_location("srv", ROOT / "server.py")
        srv = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(srv)
        srv.state = {"admin_token": "secret-token"}

        class Req:
            def __init__(self, host, headers=None, cookies=None, q=None):
                self.client = type("C", (), {"host": host})()
                self.headers, self.cookies, self.query_params = headers or {}, cookies or {}, q or {}
        self.assertTrue(srv.admin_ok(Req("127.0.0.1")))
        self.assertTrue(srv.admin_ok(Req("::1")))
        self.assertFalse(srv.admin_ok(Req("203.0.113.9")))
        self.assertFalse(srv.admin_ok(Req("203.0.113.9", headers={"x-admin-token": "wrong"})))
        self.assertTrue(srv.admin_ok(Req("203.0.113.9", headers={"x-admin-token": "secret-token"})))
        self.assertTrue(srv.admin_ok(Req("203.0.113.9", cookies={"admin_token": "secret-token"})))
        self.assertTrue(srv.admin_ok(Req("203.0.113.9", q={"token": "secret-token"})))
        self.assertFalse(srv.admin_ok(Req("203.0.113.9", q={"token": ""})))

    def test_admin_page_is_served(self):
        st, body, _ = call(self.base, "GET", "/admin")
        self.assertEqual(st, 200)
        self.assertIn(b"Taxi mock server", body)


class NtpTest(unittest.TestCase):
    def test_sntp_answer(self):
        """the NTP function answers a client request with the current time"""
        import importlib.util
        spec = importlib.util.spec_from_file_location("srv", ROOT / "server.py")
        srv = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(srv)
        port = free_port()
        # run the real function on a high port by patching bind(): reuse its code through a tiny copy
        src = (ROOT / "server.py").read_text().replace("s.bind((bind, 123))", f"s.bind((bind, {port}))")
        ns = {}
        exec(compile(src.split("def ntp_server")[1].join(["def ntp_server", ""]).split("# ----------------------------------------------------------------------------- TLS certificate")[0],
                     "ntp", "exec"), {"socket": socket, "struct": struct, "time": time}, ns)
        import threading
        threading.Thread(target=ns["ntp_server"], args=("127.0.0.1",), daemon=True).start()
        time.sleep(0.5)
        c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        c.settimeout(3)
        pkt = bytearray(48)
        pkt[0] = 0x1B
        c.sendto(bytes(pkt), ("127.0.0.1", port))
        data, _ = c.recvfrom(512)
        self.assertEqual(len(data), 48)
        self.assertEqual(data[0] & 7, 4)  # mode: server
        secs = struct.unpack("!I", data[40:44])[0] - 2208988800
        self.assertLess(abs(secs - time.time()), 3)


if __name__ == "__main__":
    unittest.main()
