"""SQLite storage: settings, every call of the meter, trips, devices, served tariffs, NTP answers.

One file (data/taxi.db). Python's own sqlite3, nothing to install. All access goes through `DB`,
which holds one connection and a lock (the HTTP loop and the NTP thread both write).
"""
import json
import sqlite3
import threading
import time

SCHEMA = """
CREATE TABLE IF NOT EXISTS kv (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS requests (
    id INTEGER PRIMARY KEY, ts REAL NOT NULL, kind TEXT NOT NULL, method TEXT, path TEXT,
    serial TEXT, remote TEXT, status INTEGER, fault TEXT, ms INTEGER, tls TEXT,
    params TEXT, problems TEXT, notes TEXT);
CREATE INDEX IF NOT EXISTS ix_req_ts ON requests(ts);
CREATE INDEX IF NOT EXISTS ix_req_serial ON requests(serial);
CREATE TABLE IF NOT EXISTS trips (
    id INTEGER PRIMARY KEY, request_id INTEGER, ts REAL NOT NULL, serial TEXT, bt INTEGER,
    start_time TEXT, stop_time TEXT, distance INTEGER, duration INTEGER, wait INTEGER,
    avg_speed INTEGER, max_speed INTEGER, trip_price INTEGER, traffic_price INTEGER,
    night_price INTEGER, total INTEGER, expected_low INTEGER, expected_high INTEGER,
    fare_check TEXT, duplicate_of INTEGER, problems TEXT, fingerprint TEXT);
CREATE INDEX IF NOT EXISTS ix_trip_ts ON trips(ts);
CREATE INDEX IF NOT EXISTS ix_trip_fp ON trips(fingerprint);
CREATE TABLE IF NOT EXISTS devices (
    serial TEXT PRIMARY KEY, first_seen REAL, last_seen REAL, last_ip TEXT, last_tls TEXT,
    requests INTEGER DEFAULT 0, expired INTEGER DEFAULT 0, tariff_override TEXT DEFAULT '',
    note TEXT DEFAULT '');
CREATE TABLE IF NOT EXISTS served_tariffs (id INTEGER PRIMARY KEY, ts REAL, serial TEXT, tariff TEXT);
CREATE INDEX IF NOT EXISTS ix_served ON served_tariffs(serial, ts);
CREATE TABLE IF NOT EXISTS open_trips (serial TEXT, bt INTEGER, ts REAL, PRIMARY KEY (serial, bt));
CREATE TABLE IF NOT EXISTS ntp (id INTEGER PRIMARY KEY, ts REAL, remote TEXT, served REAL);
CREATE TABLE IF NOT EXISTS tariff_profiles (
    id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, tariff TEXT NOT NULL DEFAULT '{}', note TEXT DEFAULT '');
CREATE TABLE IF NOT EXISTS drivers (
    id INTEGER PRIMARY KEY, name TEXT NOT NULL, phone TEXT DEFAULT '', plate TEXT DEFAULT '', car TEXT DEFAULT '',
    serial TEXT UNIQUE, profile_id INTEGER, license_until TEXT DEFAULT '', active INTEGER DEFAULT 1,
    overrides TEXT DEFAULT '', services TEXT DEFAULT '', wifi TEXT DEFAULT '', note TEXT DEFAULT '', created REAL);
CREATE INDEX IF NOT EXISTS ix_driver_serial ON drivers(serial);
"""

MIGRATIONS = [  # columns added after the first version (old databases get them on start)
    ("trips", "driver_id", "INTEGER"),
    ("requests", "driver_id", "INTEGER"),
]


class DB:
    def __init__(self, path):
        self.path = str(path)
        self.lock = threading.RLock()
        self.con = sqlite3.connect(self.path, check_same_thread=False, isolation_level=None)
        self.con.row_factory = sqlite3.Row
        with self.lock:
            self.con.execute("PRAGMA journal_mode=WAL")
            self.con.execute("PRAGMA synchronous=NORMAL")
            self.con.executescript(SCHEMA)
            for table, col, typ in MIGRATIONS:
                cols = [r[1] for r in self.con.execute(f"PRAGMA table_info({table})")]
                if col not in cols:
                    self.con.execute(f"ALTER TABLE {table} ADD COLUMN {col} {typ}")

    # ---------------------------------------------------------------- generic
    def q(self, sql, args=()):
        with self.lock:
            return [dict(r) for r in self.con.execute(sql, args).fetchall()]

    def one(self, sql, args=()):
        rows = self.q(sql, args)
        return rows[0] if rows else None

    def x(self, sql, args=()):
        with self.lock:
            cur = self.con.execute(sql, args)
            return cur.lastrowid

    # ---------------------------------------------------------------- settings
    def kv_get(self, key, default=None):
        r = self.one("SELECT value FROM kv WHERE key=?", (key,))
        return json.loads(r["value"]) if r else default

    def kv_set(self, key, value):
        self.x("INSERT INTO kv(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
               (key, json.dumps(value)))

    # ---------------------------------------------------------------- logging
    def log_request(self, kind, method, path, serial, remote, status, fault, ms, tls, params, problems, notes=None):
        drv = self.driver_by_serial(serial) if serial else None
        rid = self.x(
            "INSERT INTO requests(ts, kind, method, path, serial, remote, status, fault, ms, tls, params, problems, notes,"
            " driver_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (time.time(), kind, method, path, serial or "", remote, status, fault or "", ms, tls or "",
             json.dumps(params or {}), json.dumps(problems or []), json.dumps(notes or []), drv["id"] if drv else None))
        return rid

    def driver_by_serial(self, serial):
        return self.one("SELECT * FROM drivers WHERE serial = ? AND serial != ''", (serial,)) if serial else None

    def profile(self, pid):
        return self.one("SELECT * FROM tariff_profiles WHERE id = ?", (pid,)) if pid else None

    def touch_device(self, serial, remote, tls):
        if not serial:
            return
        now = time.time()
        self.x("INSERT INTO devices(serial, first_seen, last_seen, last_ip, last_tls, requests) VALUES(?,?,?,?,?,1)"
               " ON CONFLICT(serial) DO UPDATE SET last_seen=excluded.last_seen, last_ip=excluded.last_ip,"
               " last_tls=CASE WHEN excluded.last_tls != '' THEN excluded.last_tls ELSE devices.last_tls END,"
               " requests=devices.requests+1",
               (serial, now, now, remote, tls or ""))

    def device(self, serial):
        return self.one("SELECT * FROM devices WHERE serial=?", (serial,))

    def clear_logs(self):
        with self.lock:
            for t in ("requests", "trips", "served_tariffs", "open_trips", "ntp", "devices"):
                self.con.execute(f"DELETE FROM {t}")
