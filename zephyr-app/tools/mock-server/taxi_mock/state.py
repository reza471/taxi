"""Settings (kept in the database) and the lists of scenarios / TLS profiles shown on the admin page."""
import copy
import secrets

DEFAULT_TARIFF = {
    "PM": 46, "pri_cost": 26200, "Start_night": 22, "Stop_night": 6, "Cost_200": 1318,
    "Cost_stop": 8000, "Max_stop_km": 20, "free_trip": 0, "free_time": 0, "Percent_night": 10,
    "fraction": 50, "taxi": 1, "pri2_cost": 5000, "Cost2_200": 384, "Cost2_stop": 334,
    "samar": 1, "darbast": 1, "korsi": 1, "nobat": 1, "trip3": 0, "Cost3_200": 0, "Cost3_stop": 0,
}

TARIFF_FIELDS = [  # (key, label, group) for the form on the admin page
    ("PM", "Pulses per 10 m", "Meter"), ("Max_stop_km", "Slow below (km/h)", "Meter"),
    ("fraction", "Rounding step (Rial)", "Meter"), ("taxi", "Taxi type", "Meter"),
    ("pri_cost", "Start fare", "Single meter"), ("Cost_200", "Per 200 m", "Single meter"),
    ("Cost_stop", "Waiting per minute", "Single meter"),
    ("pri2_cost", "Start fare", "4 channels"), ("Cost2_200", "Per 200 m", "4 channels"),
    ("Cost2_stop", "Waiting per minute", "4 channels"),
    ("trip3", "Tariff 3 after (pulses, 0 = off)", "Tariff 3"), ("Cost3_200", "Per 200 m", "Tariff 3"),
    ("Cost3_stop", "Waiting per minute", "Tariff 3"),
    ("Start_night", "Night starts (hour)", "Night"), ("Stop_night", "Night ends (hour)", "Night"),
    ("Percent_night", "Night extra %", "Night"),
    ("free_trip", "Free distance (m)", "Free"), ("free_time", "Free waiting (s)", "Free"),
    ("darbast", "Single meter on", "Features"), ("korsi", "4 channels on", "Features"),
    ("samar", "Services on", "Features"), ("nobat", "Queue on", "Features"),
]

TARIFF_SCENARIOS = {
    "ok": "Normal tariff",
    "expired": "HTTP 403: license expired. Meter must show E-403",
    "error500": "HTTP 500",
    "html502": "HTTP 502 with an HTML page",
    "redirect302": "HTTP 302 to another URL (meter does not follow)",
    "bad_json": "JSON cut in the middle",
    "weird_types": "Numbers as text, true/false, 1318.5 (PHP / JS style)",
    "wrapper": 'Everything inside {"taxiFare": {...}}',
    "many_services": "15 service numbers (meter keeps 10)",
    "out_of_range": "A negative price: meter must refuse the WHOLE tariff",
    "huge_number": "A price too big for 32 bit: meter must refuse",
    "pm_zero": "PM = 0: meter must refuse",
    "empty": "{} without PM: meter must refuse",
}
POST_SCENARIOS = {
    "ok": "HTTP 200",
    "redirect301": "HTTP 301 (meter counts it as sent)",
    "error500": "HTTP 500 (meter keeps the record, retries later)",
    "bad_request": "HTTP 400 (meter keeps the record, retries later)",
}
FW_SCENARIOS = {
    "ok": "File as it is",
    "truncated": "Cut at 50 %",
    "corrupt": "64 bytes changed in the middle (signature must fail)",
    "wrong_magic": "First byte changed (meter refuses at once)",
    "redirect302": "HTTP 302 (meter does not follow redirects)",
    "not_found": "HTTP 404",
}
TRANSPORT_MODES = {
    "normal": "Normal answer with Content-Length",
    "chunked": "Transfer-Encoding: chunked",
    "close_delimited": "No length, the end of data is the closed connection",
    "slow": "Answer trickles in (about 20 KB/s)",
    "partial": "Headers and half the body, then the connection is cut",
    "drop": "Connection closed without any answer",
    "reset": "TCP reset (RST) without any answer",
    "stall": "Connection stays open, nothing is sent (meter timeout)",
}
TRANSPORT_TARGETS = {
    "all": "Every call of the meter", "tariff": "Tariff download only",
    "records": "Trip / service records only", "firmware": "Firmware download only",
}
UNREGISTERED_MODES = {
    "default": "Gets the main tariff (good for a first test)",
    "expired": "HTTP 403: shows E-403 until a driver is registered",
    "refuse": "HTTP 404: the server does not know it",
}
TLS_PROFILES = {
    "rsa": "RSA 2048 certificate, TLS 1.2 + 1.3 (like most servers)",
    "ecdsa": "EC P-256 certificate (ECDHE-ECDSA suites)",
    "tls12only": "Only TLS 1.2",
    "tls13only": "Only TLS 1.3 (the meter can NOT connect: TLS 1.2 only)",
    "ecdhe_gcm_only": "Only ECDHE + AES-GCM suites (strict modern server)",
    "bigchain": "Long certificate chain (about 10 KB): needs the 16 KB TLS buffer of the meter",
    "expired": "Expired certificate (the meter does not check it, so it still works)",
}


def default_state():
    return {
        "tariff": copy.deepcopy(DEFAULT_TARIFF),
        "services": [211, 222, 233],
        "wifi": {"wifi": "", "wifiPass": ""},
        "fw": {"file": "", "version": "", "offer": False},
        "scenario": {"tariff": "ok", "post": "ok", "fw": "ok", "delay_s": 0, "fail_percent": 0,
                     "transport": "normal", "transport_target": "all"},
        "time": {"offset_s": 0},
        "tls": {"profile": "rsa"},
        "admin_token": secrets.token_urlsafe(12),
        "allowed_serials": [],
        "unregistered": "default",  # what a meter without a driver gets: default | expired | refuse
    }


class State:
    """Settings in memory, written to the database on every change."""

    def __init__(self, db):
        self.db = db
        base = default_state()
        saved = db.kv_get("state")
        if isinstance(saved, dict):
            for k, v in saved.items():
                if isinstance(v, dict) and isinstance(base.get(k), dict):
                    base[k].update(v)
                else:
                    base[k] = v
        self.s = base
        self.save()

    def __getitem__(self, k):
        return self.s[k]

    def save(self):
        self.db.kv_set("state", self.s)
