"""Small SNTP server. Answers with this computer's time PLUS the offset set on the admin page,
so night / day changes can be tested at any hour ("make the meter think it is 21:58")."""
import socket
import struct
import threading
import time

NTP_EPOCH = 2208988800


def build_reply(request: bytes, now: float) -> bytes:
    t = now + NTP_EPOCH
    sec, frac = int(t), int((t % 1) * 2 ** 32)
    r = bytearray(48)
    r[0] = (0 << 6) | (3 << 3) | 4           # leap 0, version 3, mode 4 (server)
    r[1] = 2                                   # stratum
    r[2], r[3] = 6, 0xEC
    r[12:16] = b"LOCL"
    r[16:24] = struct.pack("!II", sec, frac)   # reference time
    r[24:32] = request[40:48]                  # originate = the client's transmit time
    r[32:40] = struct.pack("!II", sec, frac)   # receive
    r[40:48] = struct.pack("!II", sec, frac)   # transmit
    return bytes(r)


class NtpServer:
    def __init__(self, db, state, bind="0.0.0.0", port=123):
        self.db, self.st, self.bind, self.port = db, state, bind, port
        self.running = False
        self.error = ""

    def now(self):
        return time.time() + float(self.st["time"].get("offset_s", 0))

    def start(self):
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sock.bind((self.bind, self.port))
        except OSError as e:
            self.error = f"UDP {self.port}: {e}"
            print(f"NTP: can not use UDP {self.port} ({e}). Run as administrator or free the port. Continuing without NTP.")
            return False
        self.running = True
        threading.Thread(target=self._loop, daemon=True).start()
        print(f"NTP: answering on UDP {self.port}")
        return True

    def _loop(self):
        while True:
            try:
                data, addr = self.sock.recvfrom(512)
            except OSError:
                return
            if len(data) < 48:
                continue
            served = self.now()
            self.sock.sendto(build_reply(data, served), addr)
            try:
                self.db.x("INSERT INTO ntp(ts, remote, served) VALUES(?,?,?)", (time.time(), addr[0], served))
                self.db.x("DELETE FROM ntp WHERE id < (SELECT MAX(id) - 2000 FROM ntp)")
            except Exception:
                pass
