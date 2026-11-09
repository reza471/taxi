"""Own small HTTPS server on one port.

Why not uvicorn: to test the meter we must control every byte (chunked answers, a connection cut in the
middle, a TCP reset, an answer without length ...). Meter calls (/api/taxi/..., /fw/...) are written here
by hand; everything else (the admin page) is passed to the FastAPI app through a tiny ASGI bridge.
Each connection carries ONE request and is closed after the answer (the meter works like that too).
TLS is started per connection, so the TLS profile can change while the server runs.
"""
import asyncio
import socket
import struct
import time
from http import HTTPStatus
from urllib.parse import parse_qsl, unquote, urlsplit

from .meter import Req

MAX_BODY = 10 * 1024 * 1024
BROWSER_ALERTS = ("CERTIFICATE_UNKNOWN", "UNKNOWN_CA", "BAD_CERTIFICATE", "CERTIFICATE_EXPIRED")


def _reason(code):
    try:
        return HTTPStatus(code).phrase
    except ValueError:
        return "Status"


class HttpServer:
    def __init__(self, meter, admin_app, db, state, tls=None):
        self.meter, self.app, self.db, self.st, self.tls = meter, admin_app, db, state, tls

    # ---------------------------------------------------------------- connection
    async def handle(self, reader, writer):
        peer = writer.get_extra_info("peername") or ("?", 0)
        remote = peer[0]
        tlsinfo = ""
        try:
            if self.tls:
                profile = self.st["tls"]["profile"]
                try:
                    await asyncio.wait_for(writer.start_tls(self.tls.context(profile)), 20)
                except Exception as e:  # the meter could not agree on TLS: very useful to see
                    text = f"{type(e).__name__}: {e}"
                    # A browser that does not trust the self-signed certificate closes its extra
                    # connections with such an alert. The meter never sends it (it checks no
                    # certificate), so these are not the meter's problem: do not log them.
                    if not any(a in text.upper() for a in BROWSER_ALERTS):
                        self.db.log_request("TLS", "", "", "", remote, 0, "handshake", 0, profile, {},
                                            [f"TLS handshake failed ({profile}): {text}"])
                    return
                so = writer.get_extra_info("ssl_object")
                if so:
                    tlsinfo = f"{so.version()} {so.cipher()[0]}"
            try:
                head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 30)
            except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, asyncio.TimeoutError, ConnectionError):
                return
            lines = head.decode("latin-1").split("\r\n")
            try:
                method, target, _ = lines[0].split(" ", 2)
            except ValueError:
                await self._simple(writer, 400, b"bad request line")
                return
            headers, hlist = {}, []
            for ln in lines[1:]:
                if ":" in ln:
                    k, v = ln.split(":", 1)
                    headers[k.strip().lower()] = v.strip()
                    hlist.append((k.strip(), v.strip()))
            body = b""
            if "transfer-encoding" in headers:
                await self._simple(writer, 411, b"send a Content-Length")
                return
            n = int(headers.get("content-length", "0") or 0)
            if n > MAX_BODY:
                await self._simple(writer, 413, b"too big")
                return
            if n:
                body = await asyncio.wait_for(reader.readexactly(n), 60)
            parts = urlsplit(target)
            path = parts.path
            if path.startswith("/api/taxi/") or path.startswith("/fw/"):
                await self._meter(writer, method, path, parts.query, headers, body, remote, tlsinfo)
            else:
                await self._asgi(writer, method, path, parts.query, hlist, body, peer, tlsinfo)
        except Exception as e:  # never let one bad connection stop the server
            try:
                self.db.log_request("ERROR", "", "", "", remote, 0, "server", 0, tlsinfo, {}, [f"{type(e).__name__}: {e}"])
            except Exception:
                pass
        finally:
            try:
                if not writer.is_closing():
                    writer.close()
                await asyncio.wait_for(writer.wait_closed(), 5)
            except Exception:
                pass

    async def _simple(self, writer, code, body):
        writer.write(f"HTTP/1.1 {code} {_reason(code)}\r\nContent-Length: {len(body)}\r\nConnection: close\r\n\r\n".encode() + body)
        await writer.drain()

    # ---------------------------------------------------------------- meter calls
    async def _meter(self, writer, method, path, query, headers, body, remote, tlsinfo):
        t0 = time.monotonic()
        q = Req(method, path, dict(parse_qsl(query, keep_blank_values=True)), headers, body, remote, tlsinfo)
        m = self.meter
        if path == "/api/taxi/TaxiFare" and method == "GET":
            r = await m.tariff(q)
        elif path == "/api/taxi/SetTaxiStart" and method in ("GET", "POST"):
            r = await m.record(q, "START")
        elif path == "/api/taxi/SetTaxiEnd" and method in ("GET", "POST"):
            r = await m.record(q, "END")
        elif path == "/api/taxi/StartService" and method in ("GET", "POST"):
            r = await m.record(q, "SERVICE")
        elif path.startswith("/fw/") and method == "GET":
            r = await m.firmware(q, unquote(path[4:]))
        else:
            from .meter import treply
            r = treply("unknown call", 404)
            r.log = {"kind": "OTHER", "serial": q.query.get("serial", ""), "params": q.query,
                     "problems": [f"unknown call {method} {path}"], "notes": [], "trip": None}
        await self.write_reply(writer, r)
        lg = r.log
        status = 0 if r.fault in ("drop", "reset", "stall") else r.status
        rid = self.db.log_request(lg["kind"], method, path, lg["serial"], remote, status, r.fault,
                                  int((time.monotonic() - t0) * 1000), tlsinfo, lg["params"], lg["problems"], lg["notes"])
        self.db.touch_device(lg["serial"], remote, tlsinfo)
        if lg.get("trip"):
            self.meter.store_trip(rid, lg["trip"], lg["problems"])

    async def write_reply(self, writer, r):
        if r.fault == "drop":
            return
        if r.fault == "reset":
            sock = writer.get_extra_info("socket")
            if sock is not None:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            writer.transport.abort()
            return
        if r.fault == "stall":
            await asyncio.sleep(40)
            return
        head = [f"HTTP/1.1 {r.status} {_reason(r.status)}", f"Content-Type: {r.ctype}", "Connection: close",
                "Server: taxi-mock"] + [f"{k}: {v}" for k, v in r.headers]
        body = r.body
        if r.fault == "partial":
            head.append(f"Content-Length: {len(body)}")
            writer.write(("\r\n".join(head) + "\r\n\r\n").encode() + body[: max(1, len(body) // 2)])
            await writer.drain()
            writer.transport.abort()
            return
        if r.transfer == "chunked":
            head.append("Transfer-Encoding: chunked")
            out = b"".join(b"%x\r\n" % len(body[i:i + 1024]) + body[i:i + 1024] + b"\r\n"
                           for i in range(0, len(body), 1024)) + b"0\r\n\r\n"
        elif r.transfer == "close_delimited":
            out = body  # no length: the closed connection marks the end
        else:
            head.append(f"Content-Length: {len(body)}")
            out = body
        writer.write(("\r\n".join(head) + "\r\n\r\n").encode())
        if r.transfer == "slow":
            for i in range(0, len(out), 4096):
                writer.write(out[i:i + 4096])
                await writer.drain()
                await asyncio.sleep(0.2)
        else:
            writer.write(out)
        await writer.drain()

    # ---------------------------------------------------------------- admin (FastAPI) through ASGI
    async def _asgi(self, writer, method, path, query, hlist, body, peer, tlsinfo):
        scope = {
            "type": "http", "asgi": {"version": "3.0", "spec_version": "2.3"}, "http_version": "1.1",
            "method": method, "scheme": "https" if self.tls else "http", "path": unquote(path),
            "raw_path": path.encode(), "query_string": query.encode(), "root_path": "",
            "headers": [(k.lower().encode("latin-1"), v.encode("latin-1")) for k, v in hlist],
            "client": (peer[0], peer[1]), "server": ("server", 0),
        }
        sent = False

        async def receive():
            nonlocal sent
            if not sent:
                sent = True
                return {"type": "http.request", "body": body, "more_body": False}
            await asyncio.sleep(3600)
            return {"type": "http.disconnect"}

        status, rh, chunks = 500, [], []

        async def send(msg):
            nonlocal status, rh
            if msg["type"] == "http.response.start":
                status = msg["status"]
                rh = [(k.decode("latin-1"), v.decode("latin-1")) for k, v in msg.get("headers", [])]
            elif msg["type"] == "http.response.body":
                chunks.append(msg.get("body", b""))

        await self.app(scope, receive, send)
        data = b"".join(chunks)
        head = [f"HTTP/1.1 {status} {_reason(status)}"] + \
               [f"{k}: {v}" for k, v in rh if k.lower() not in ("content-length", "transfer-encoding", "connection")] + \
               [f"Content-Length: {len(data)}", "Connection: close"]
        writer.write(("\r\n".join(head) + "\r\n\r\n").encode("latin-1") + data)
        await writer.drain()
