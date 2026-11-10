#!/usr/bin/env python3
"""cstv_link.py — cstvd's connection to the CS:TV hub (cstv.xyz): one WebSocket, line for line.

cstvd starts one per connection (python3 cstv_link.py <wss url>) while a game is connected, writes the hub's
lines to its stdin and reads the hub's lines from its stdout. When the connection ends this process ends; cstvd
starts a new one later (1 s .. 30 s). The protocol itself is daf's master protocol (notes/PROTOCOL.md), carried
in WebSocket text messages (several lines per message, "\\n" between them) so it gets TLS and Cloudflare's front.

stdout: "#open" once connected, then every line from the hub, and "#closed <why>" last.
stdin:  lines for the hub, as they are.

The sign-in token never leaves this process and its file: the first line to the hub is "auth <token>" (or
"auth -"), the hub's "token <t>" (a new sign-in, !tvlogin) is saved and "token -" (signed out) deletes it;
neither reaches cstvd or the game. File: ~/.config/cstv/hub.json (Windows: %APPDATA%\\cstv\\hub.json), 0600.

Only the Python standard library (it also runs on the embeddable Python in the Windows build).
"""
import base64
import hashlib
import json
import os
import socket
import ssl
import struct
import sys
import threading
import urllib.parse

DEFAULT_URL = "wss://cstv.xyz/game"
if os.name == "nt":
    CONF_DIR = os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~"), "cstv")
else:
    CONF_DIR = os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "cstv")
CONF = os.path.join(CONF_DIR, "hub.json")

out_lock = threading.Lock()


def out(line):
    with out_lock:
        sys.stdout.buffer.write(line.encode("utf-8", "replace") + b"\n")
        sys.stdout.buffer.flush()


def load_conf():
    try:
        with open(CONF, encoding="utf-8") as f:
            c = json.load(f)
        return c if isinstance(c, dict) else {}
    except (OSError, ValueError):
        return {}


def save_conf(conf):
    os.makedirs(CONF_DIR, exist_ok=True)
    tmp = CONF + ".tmp"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(conf, f)
    os.replace(tmp, CONF)


class WS:
    """Minimal RFC 6455 client: text messages, answers pings."""

    def __init__(self, url, timeout=10):
        u = urllib.parse.urlsplit(url)
        secure = u.scheme == "wss"
        host, port = u.hostname, u.port or (443 if secure else 80)
        raw = socket.create_connection((host, port), timeout=timeout)
        raw.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock = ssl.create_default_context().wrap_socket(raw, server_hostname=host) if secure else raw
        key = base64.b64encode(os.urandom(16)).decode()
        path = (u.path or "/") + ("?" + u.query if u.query else "")
        req = (f"GET {path} HTTP/1.1\r\nHost: {host}{'' if port in (80, 443) else ':' + str(port)}\r\n"
               f"Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
               f"Sec-WebSocket-Version: 13\r\nUser-Agent: cstv-link/1\r\n\r\n")
        self.sock.sendall(req.encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("the hub closed the connection during the handshake")
            head += chunk
            if len(head) > 16384:
                raise ConnectionError("bad handshake")
        head, self.buf = head.split(b"\r\n\r\n", 1)
        lines = head.decode("latin-1").split("\r\n")
        if " 101 " not in lines[0] + " ":
            raise ConnectionError("the hub refused: " + lines[0])
        accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        hdrs = {l.split(":", 1)[0].strip().lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}
        if hdrs.get("sec-websocket-accept") != accept:
            raise ConnectionError("bad handshake answer")
        self.sock.settimeout(None)
        self.wlock = threading.Lock()

    def _frame(self, opcode, payload):
        hdr = bytearray([0x80 | opcode])
        n = len(payload)
        if n < 126:
            hdr.append(0x80 | n)
        elif n < 65536:
            hdr.append(0x80 | 126)
            hdr += struct.pack(">H", n)
        else:
            hdr.append(0x80 | 127)
            hdr += struct.pack(">Q", n)
        mask = os.urandom(4)
        body = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        with self.wlock:
            self.sock.sendall(bytes(hdr) + mask + body)

    def send(self, text):
        self._frame(1, text.encode("utf-8"))

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("the hub closed the connection")
            self.buf += chunk
        data, self.buf = self.buf[:n], self.buf[n:]
        return data

    def recv(self):
        parts = []
        while True:
            b0, b1 = self._read(2)
            fin, opcode, n = b0 & 0x80, b0 & 0x0F, b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._read(8))[0]
            if n > (1 << 22):
                raise ConnectionError("message too large")
            mask = self._read(4) if b1 & 0x80 else None
            data = self._read(n)
            if mask:
                data = bytes(b ^ mask[i & 3] for i, b in enumerate(data))
            if opcode == 9:
                self._frame(10, data)
            elif opcode == 8:
                code = struct.unpack(">H", data[:2])[0] if len(data) >= 2 else 1005
                why = data[2:].decode("utf-8", "replace")
                raise ConnectionError(f"the hub closed the connection ({code}{': ' + why if why else ''})")
            elif opcode in (0, 1, 2):
                parts.append(data)
                if fin:
                    return b"".join(parts).decode("utf-8", "replace")

    def close(self):
        try:
            self._frame(8, struct.pack(">H", 1000))
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


def from_cstvd(ws, done):
    """stdin -> the hub, a few lines per message."""
    try:
        for raw in sys.stdin.buffer:
            line = raw.decode("utf-8", "replace").strip()
            if line:
                ws.send(line)
    except (OSError, ConnectionError):
        pass
    done.set()
    ws.close()


def main():
    url = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1].startswith(("ws://", "wss://")) else \
        os.environ.get("CSTV_HUB_URL", DEFAULT_URL)
    try:
        ws = WS(url)
    except (OSError, ConnectionError, ssl.SSLError) as e:
        out(f"#closed can't connect to {urllib.parse.urlsplit(url).hostname}: {e}")
        return 1
    conf = load_conf()
    ws.send("auth " + (conf.get("token") or "-"))
    out("#open")
    done = threading.Event()
    threading.Thread(target=from_cstvd, args=(ws, done), daemon=True).start()
    why = "closed"
    try:
        while not done.is_set():
            for line in ws.recv().split("\n"):
                line = line.strip()
                if line.startswith("token "):
                    tok = line[6:].strip()
                    conf = load_conf()
                    if tok == "-":
                        conf.pop("token", None)
                    else:
                        conf["token"] = tok
                    save_conf(conf)
                elif line:
                    out(line)
    except (OSError, ConnectionError, ssl.SSLError) as e:
        why = str(e) or e.__class__.__name__
    if done.is_set():
        why = "cstvd closed the link"
    out("#closed " + why)
    ws.close()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        pass
