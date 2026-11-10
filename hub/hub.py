#!/usr/bin/env python3
"""cstv-hub: CS:TV without the server plugin (cstv.xyz).

One process serves the website and two WebSockets: /ws for browsers (JSON) and /game for the games (their cstvd
through cstv_link.py; daf's master protocol, one line per message). It holds what the SourceMod plugin held on a
plugin server, for everyone with CS:TV:
  * server rooms: everyone with CS:TV on the same game server shares a TV (its place per map, the link, the
    clock, the queue); anyone there may control it, like the plugin;
  * lobbies: a code anyone can join from the game or the website, on any server, optionally with a password;
    the creator owns it, with the plugin's 7 permission switches for the others, a chat (GIFs, emotes,
    moderation) and a page on the website;
  * streams: a player's own game picture, relayed by mediamtx (RTMP), which asks this process who may publish.
Video never passes through here: every viewer streams it themselves. Sign-in is Steam (OpenID) on the website;
a game signs in with a code from the website (!tvlogin <code>) and keeps a token.

Run:  python3 hub.py            (http://localhost:8787)
Env:  CSTV_HUB_PORT (8787), CSTV_HUB_HOST (127.0.0.1), CSTV_HUB_PUBLIC_URL (http://localhost:<port>: the
      address people open; Steam login returns there), CSTV_HUB_DB (data/hub.db next to this file),
      CSTV_HUB_STREAM_HOST (the host mediamtx's RTMP port is reachable at; empty = no streams),
      CSTV_HUB_AUTH_PORT (27971: mediamtx's auth requests, on 127.0.0.1), CSTV_HUB_WHEP_URL (http://127.0.0.1:8889:
      mediamtx's WebRTC server; the website's stream player sends its offer through the hub to it).
      CSTV_HUB_ADMINS (SteamID64s, comma separated) and/or data/admins (one per line, next to hub.db; read again
      every few seconds): cstv.xyz admins, who have every lobby's owner powers, the admin page (/admin: every
      lobby, silent joins, site bans by account + IP, warnings, site-wide mutes) and can't be kicked by lobbies.
CLI:  hub.py ban <steamid64> [reason] | unban <steamid64>   (site-wide, works while the hub runs)

Protocol: PROTOCOL.md. Needs Python 3.10+ and the `websockets` package (>= 13).
"""
import asyncio
import base64
import hashlib
import hmac
import ipaddress
import json
import logging
import math
import os
import re
import secrets
import signal
import sqlite3
import sys
import time
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import datetime as dt
from zoneinfo import ZoneInfo
from collections import deque
from http.cookies import SimpleCookie

from websockets.asyncio.server import serve
from websockets.datastructures import Headers
from websockets.exceptions import ConnectionClosed
from websockets.http11 import Response

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = int(os.environ.get("CSTV_HUB_PORT", "8787"))
HOST = os.environ.get("CSTV_HUB_HOST", "127.0.0.1")
PUBLIC_URL = os.environ.get("CSTV_HUB_PUBLIC_URL", f"http://localhost:{PORT}").rstrip("/")
DB_PATH = os.environ.get("CSTV_HUB_DB", os.path.join(HERE, "data", "hub.db"))
WEB_DIR = os.path.join(HERE, "web")
SECURE = PUBLIC_URL.startswith("https://")
ORIGIN = "/".join(PUBLIC_URL.split("/")[:3])
PUBLIC_HOST = urllib.parse.urlsplit(PUBLIC_URL).netloc
SITE = PUBLIC_HOST   # what players are told to open
TRUST_CF = os.environ.get("CSTV_HUB_TRUST_CF") == "1"   # behind a Cloudflare tunnel: Cf-Connecting-Ip is the visitor
MAX_CONN_PER_IP = int(os.environ.get("CSTV_HUB_MAX_CONN_PER_IP", "12"))
GIPHY_URL = os.environ.get("CSTV_HUB_GIPHY_URL", "https://api.giphy.com/v1/gifs")
TWITCH_GQL_ID = "kimne78kx3ncx6brgo4mv6wki5h1ko"   # Twitch's public web client id (as in cstv_overlay.py)
SLOW_CHOICES = (0, 3, 5, 10, 30, 60, 120)


def giphy_key():
    """CSTV_HUB_GIPHY_KEY, or data/giphy.key (read each time: dropping the file in works without a restart)."""
    k = os.environ.get("CSTV_HUB_GIPHY_KEY")
    if k:
        return k.strip()
    try:
        with open(os.path.join(os.path.dirname(DB_PATH), "giphy.key")) as f:
            return f.read().strip() or None
    except OSError:
        return None


_admins = [0.0, frozenset()]   # [read at, SteamID64s]


def site_admins():
    """CSTV_HUB_ADMINS + data/admins ("#" starts a comment), read again every 5 s."""
    if time.monotonic() - _admins[0] > 5:
        ids = set(os.environ.get("CSTV_HUB_ADMINS", "").replace(",", " ").split())
        try:
            with open(os.path.join(os.path.dirname(DB_PATH), "admins")) as f:
                ids |= {ln.split("#")[0].strip() for ln in f}
        except OSError:
            pass
        _admins[:] = [time.monotonic(), frozenset(x for x in ids if SID_RE.match(x))]
    return _admins[1]


def is_site_admin(steamid):
    return bool(steamid) and steamid in site_admins()


log = logging.getLogger("cstv-hub")

# The plugin's permission bits (cstv.sp), same meaning, same order.
PERM_LINK, PERM_QUEUE, PERM_SKIP, PERM_PAUSE, PERM_SEEK, PERM_PLAYBACK, PERM_SCREEN = (1 << i for i in range(7))
PERM_ALL = (1 << 7) - 1
ADMIN = 1 << 7    # may change permissions, kick, ban, mute, lobby settings
OWNER = 1 << 8    # the lobby's creator (only the owner makes admins)
SITEADMIN = 1 << 9   # a cstv.xyz admin (data/admins): the owner's powers in every lobby, can't be kicked or banned there

CODE_CHARS = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"   # no 0/O, 1/I
URL_MAX = 1500
PROTO = 2                 # the game protocol's version (daf's master: 2)
QUEUE_MAX = 100
HISTORY_MAX = 50
LOAD_TIMEOUT = 25.0       # s: a new link / seek waits this long at most for the first viewer's "started"
SYNC_EVERY = 15.0         # s: the games get the clock again
TOL_SHARED, TOL_ALONE = 150, 5000   # ms a viewer may drift with 2+ watching / alone
SCALE_MIN, SCALE_MAX = 0.1, 4.0
SERVER_TTL = 3600         # s: an empty server room is forgotten (its screens stay in the database)
LOBBY_UNLOAD = 600        # s: an empty lobby is saved and unloaded
LOBBY_EXPIRE = 3600       # s: a lobby nobody has been in for this long closes for good
VISITS_KEEP = 30 * 86400  # s: how far back the admin page's visitor log goes
LABEL_MAX = 40
PASS_MAX = 64
PASS_TRIES, PASS_LOCKOUT = 5, 600   # wrong lobby passwords per IP, then it stops listening this long (s)
BUCKET_MAX, BUCKET_RATE = 8.0, 0.5  # game control commands: 8 at once, one more every 2 s
OUT_MAX = 5000            # messages queued for one connection before it counts as not reading
RTMP_PORT = 1935
STREAM_READY_WAIT = 20    # s: a stream the streamer says is pushing goes live without the relay's "ready" after this
LEFT_GRACE = 45           # s: "X left" only when they're not back by then (a page reload, a reconnect says nothing)
JOIN_QUIET = 120          # s: one "X joined" per person in this long (someone coming and going)
STREAM_HOST = os.environ.get("CSTV_HUB_STREAM_HOST", "").strip()
AUTH_PORT = int(os.environ.get("CSTV_HUB_AUTH_PORT", "27971"))
WHEP_URL = os.environ.get("CSTV_HUB_WHEP_URL", "http://127.0.0.1:8889").rstrip("/")
# mediamtx's HLS (127.0.0.1 only): the website's fallback for browsers whose WebRTC can't decode H.264 (Vivaldi);
# the hub passes it on under a per-viewer token (/hls/<token>/...)
HLS_URL = os.environ.get("CSTV_HUB_HLS_URL", "http://127.0.0.1:8888").rstrip("/")
ROOM_RE = re.compile(r"^[0-9A-Za-z.:\[\]_-]{3,64}$")
MAP_RE = re.compile(r"^[0-9A-Za-z._-]{1,64}$")
STREAM_PATH_RE = re.compile(r"^live/([0-9a-f]{16})$")
SID_RE = re.compile(r"^\d{17}$")
LOBBY_PAGE_WEB, LOBBY_PAGE_GAME = 20, 8
WARN_MAX = 500


def now_ms():
    return int(time.monotonic() * 1000)


def new_code(n=6):
    return "".join(secrets.choice(CODE_CHARS) for _ in range(n))


def token_hash(tok):
    return hashlib.sha256(tok.encode()).hexdigest()


def b64e(s):
    return base64.urlsafe_b64encode(s.encode()).decode().rstrip("=")


def b64o(s):
    return b64e(s) if s else "-"


def b64d(s):
    if s == "-":
        return ""
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4)).decode("utf-8", "replace")


def url_ok(u):
    """http(s) with a public host: every viewer's player fetches the link, so no LAN or loopback addresses
    (names are checked again by each cstvd against what they resolve to)."""
    if not isinstance(u, str) or not 8 <= len(u) <= URL_MAX or any(c <= " " or c == "\x7f" for c in u):
        return False
    try:
        p = urllib.parse.urlsplit(u)
    except ValueError:
        return False
    if p.scheme not in ("http", "https") or not p.hostname:
        return False
    host = p.hostname.lower()
    if host == "localhost" or host.endswith((".localhost", ".local", ".lan", ".internal")):
        return False
    try:
        return ipaddress.ip_address(host).is_global
    except ValueError:
        return True


def link_kind(u):
    lu = (u or "").lower()
    if lu.startswith("rtmp://"):
        return "stream"   # only the hub sets these (url_ok refuses rtmp)
    if "youtube.com/" in lu or "youtu.be/" in lu:
        return "youtube"
    i = lu.find("twitch.tv/")
    if i != -1:
        rest = lu[i + 10:]
        if not rest:
            return "other"
        return "twitch_vod" if rest.startswith("videos/") or "/" in rest else "twitch_live"
    return "other"


def yt_list(u):
    if link_kind(u) != "youtube":
        return ""
    m = re.search(r"[?&]list=([^&#]+)", u)
    return m.group(1) if m else ""


def link_start(u):
    """Where a link says to start, in ms (0 = the beginning): YouTube t= / start= (715, 715s, 11m55s, 1h2m3s;
    also after #), Twitch VODs t=01h02m03s. cstvd's player starts there by itself, so the room's clock must too
    (it said 0, and the first sync pulled everyone back to the start)."""
    if link_kind(u) not in ("youtube", "twitch_vod"):
        return 0
    try:
        p = urllib.parse.urlsplit(u)
    except ValueError:
        return 0
    q = urllib.parse.parse_qs(p.fragment)
    q.update(urllib.parse.parse_qs(p.query))
    v = ((q.get("t") or q.get("start") or [""])[0]).strip().lower()
    m = re.fullmatch(r"(?:(\d{1,3})h)?(?:(\d{1,4})m)?(?:(\d{1,6})s?)?", v)
    if not v or not m:
        return 0
    h, mi, sec = (int(x or 0) for x in m.groups())
    return (h * 3600 + mi * 60 + sec) * 1000


def parse_clock(s):
    parts = s.split(":")
    if not 1 <= len(parts) <= 3 or not all(p.isdigit() and len(p) <= 6 for p in parts):
        return None
    t = 0
    for p in parts:
        t = t * 60 + int(p)
    return float(t)


def clock_label(secs):
    s = int(secs)
    return f"{s // 3600}:{s // 60 % 60:02d}:{s % 60:02d}" if s >= 3600 else f"{s // 60}:{s % 60:02d}"


def short_url(u):
    p = urllib.parse.urlsplit(u)
    s = (p.hostname or "") + p.path
    return s if len(s) <= 60 else s[:57] + "..."


YT_HOSTS = ("youtube.com", "www.youtube.com", "m.youtube.com", "music.youtube.com", "youtu.be")
TW_HOSTS = ("twitch.tv", "www.twitch.tv", "m.twitch.tv")


def site_of(u):
    h = (urllib.parse.urlsplit(u).hostname or "").lower()
    if h in YT_HOSTS:
        return "youtube"
    if h in TW_HOSTS:
        return "twitch"
    return "other"


def fmt_left(sec):
    sec = int(sec) + 1
    return f"{sec // 3600} h {sec % 3600 // 60} min" if sec >= 3600 else f"{sec // 60} min {sec % 60} s" if sec >= 60 else f"{sec} s"


def clean_text(s, limit):
    """One line of printable text (no control characters: they'd reach the game's chat)."""
    s = re.sub(r"\s+", " ", str(s))
    return "".join(ch for ch in s if ch.isprintable()).strip()[:limit]


# ---------------------------------------------------------------- database
class DB:
    def __init__(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.c = sqlite3.connect(path, isolation_level=None)
        self.c.row_factory = sqlite3.Row
        self.c.executescript("""
            PRAGMA journal_mode=WAL;
            CREATE TABLE IF NOT EXISTS users(steamid TEXT PRIMARY KEY, name TEXT, avatar TEXT, updated REAL);
            CREATE TABLE IF NOT EXISTS sessions(hash TEXT PRIMARY KEY, steamid TEXT, kind TEXT, created REAL, last REAL);
            CREATE TABLE IF NOT EXISTS lobbies(code TEXT PRIMARY KEY, name TEXT, owner TEXT, default_perms INTEGER,
                sites TEXT, created REAL, last REAL, saved TEXT);
            CREATE TABLE IF NOT EXISTS members(code TEXT, steamid TEXT, perms INTEGER, admin INTEGER,
                PRIMARY KEY(code, steamid));
            CREATE TABLE IF NOT EXISTS bans(code TEXT, steamid TEXT, by TEXT, at REAL, PRIMARY KEY(code, steamid));
            CREATE TABLE IF NOT EXISTS global_bans(steamid TEXT PRIMARY KEY, reason TEXT, at REAL);
            CREATE TABLE IF NOT EXISTS srv_screens(room TEXT, loc TEXT, geo TEXT, at REAL, PRIMARY KEY(room, loc));
            CREATE TABLE IF NOT EXISTS ip_bans(ip TEXT PRIMARY KEY, steamid TEXT, reason TEXT, at REAL);
            CREATE TABLE IF NOT EXISTS global_mutes(steamid TEXT PRIMARY KEY, until REAL);
            CREATE TABLE IF NOT EXISTS visits(id INTEGER PRIMARY KEY, at REAL, until REAL, ip TEXT, steamid TEXT,
                name TEXT, kind TEXT, ua TEXT);
            CREATE INDEX IF NOT EXISTS visits_at ON visits(at);
        """)
        for tbl, col, decl in (("lobbies", "slow", "INTEGER DEFAULT 0"), ("lobbies", "gifs", "INTEGER DEFAULT 1"),
                               ("lobbies", "emotes", "TEXT"), ("sessions", "label", "TEXT"),
                               ("lobbies", "password", "TEXT"), ("members", "pw", "TEXT"), ("lobbies", "seen", "REAL")):
            try:
                self.c.execute(f"ALTER TABLE {tbl} ADD COLUMN {col} {decl}")
            except sqlite3.OperationalError:
                pass   # added before
        # lobbies from before "seen" get a full LOBBY_EXPIRE from now; visits still open were ended by a restart
        self.c.execute("UPDATE lobbies SET seen=? WHERE seen IS NULL", (time.time(),))
        self.c.execute("UPDATE visits SET until=at WHERE until IS NULL")

    def q(self, sql, *args):
        return self.c.execute(sql, args).fetchall()

    def one(self, sql, *args):
        return self.c.execute(sql, args).fetchone()

    def x(self, sql, *args):
        self.c.execute(sql, args)


db = None


def user_row(steamid):
    return db.one("SELECT * FROM users WHERE steamid=?", steamid)


def user_name(steamid):
    r = user_row(steamid)
    return r["name"] if r and r["name"] else steamid


def globally_banned(steamid):
    return not is_site_admin(steamid) and db.one("SELECT 1 FROM global_bans WHERE steamid=?", steamid) is not None


def ip_banned(ip):
    return db.one("SELECT 1 FROM ip_bans WHERE ip=?", ip) is not None


def global_mute_left(steamid):
    r = db.one("SELECT until FROM global_mutes WHERE steamid=?", steamid) if steamid else None
    return r["until"] - time.time() if r and r["until"] > time.time() else 0


def new_session(steamid, kind, label=None):
    tok = secrets.token_urlsafe(32)
    t = time.time()
    db.x("INSERT INTO sessions(hash, steamid, kind, created, last, label) VALUES(?,?,?,?,?,?)",
         token_hash(tok), steamid, kind, t, t, label)
    return tok


def session_user(tok):
    if not tok or len(tok) > 100:
        return None
    r = db.one("SELECT steamid FROM sessions WHERE hash=?", token_hash(tok))
    if not r:
        return None
    db.x("UPDATE sessions SET last=? WHERE hash=?", time.time(), token_hash(tok))
    return r["steamid"]


# ---------------------------------------------------------------- outgoing HTTP (in threads)
def http_get(url, timeout=6, body=None, headers=None):
    req = urllib.request.Request(url, data=body, headers={"User-Agent": "cstv-hub/1", **(headers or {})})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read(4 << 20)


async def jreq(url, body=None, headers=None):
    return json.loads(await asyncio.to_thread(http_get, url, 8, body, headers))


def http_post(url, form, timeout=10):
    data = urllib.parse.urlencode(form).encode()
    req = urllib.request.Request(url, data=data, headers={"User-Agent": "cstv-hub/1",
                                                          "Content-Type": "application/x-www-form-urlencoded"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read(1 << 16)


def http_post_sdp(url, sdp, timeout=10):
    req = urllib.request.Request(url, data=sdp.encode(), method="POST",
                                 headers={"User-Agent": "cstv-hub/1", "Content-Type": "application/sdp"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read(1 << 18).decode("utf-8", "replace")


async def fetch_profile(steamid):
    """Name + avatar from the public profile XML (no API key needed)."""
    try:
        raw = await asyncio.to_thread(http_get, f"https://steamcommunity.com/profiles/{steamid}/?xml=1")
        root = ET.fromstring(raw)
        name = (root.findtext("steamID") or "").strip()
        avatar = (root.findtext("avatarMedium") or "").strip()
        if name:
            db.x("INSERT INTO users VALUES(?,?,?,?) ON CONFLICT(steamid) DO UPDATE SET name=?, avatar=?, updated=?",
                 steamid, name, avatar, time.time(), name, avatar, time.time())
    except Exception as e:
        log.info("profile %s: %s", steamid, e)


async def fetch_title(url):
    site = site_of(url)
    try:
        if site == "youtube":
            raw = await asyncio.to_thread(http_get, "https://www.youtube.com/oembed?format=json&url=" + urllib.parse.quote(url, safe=""))
            return clean_text(json.loads(raw).get("title", ""), 200) or None
        if site == "twitch":
            p = [x for x in urllib.parse.urlsplit(url).path.split("/") if x]
            if len(p) >= 2 and p[0] == "videos":
                return f"Twitch video {p[1]}"
            if p:
                return f"{p[0]} (Twitch)"
    except Exception as e:
        log.info("title %s: %s", url, e)
    return None


# ---- BTTV / 7TV emotes (the same sources as the in-game chat overlay), cached
emote_cache = {}   # key -> (time, {name: {"u": url, "s": source}})


async def cached(key, ttl, fn):
    hit = emote_cache.get(key)
    if hit and time.time() - hit[0] < ttl:
        return hit[1]
    try:
        val = await fn()
    except Exception as e:
        log.info("emotes %s: %s", key, e)
        val = hit[1] if hit else {}
    emote_cache[key] = (time.time(), val)
    return val


def seventv_url(e):
    host = (e.get("data") or {}).get("host") or {}
    return "https:" + host["url"] + "/2x.webp" if str(host.get("url", "")).startswith("//") else None


async def emotes_global():
    out = {}
    try:
        for e in await jreq("https://api.betterttv.net/3/cached/emotes/global"):
            out[e["code"]] = {"u": f"https://cdn.betterttv.net/emote/{e['id']}/2x", "s": "BTTV"}
    except Exception as e:
        log.info("bttv global: %s", e)
    try:
        for e in (await jreq("https://7tv.io/v3/emote-sets/global")).get("emotes", []):
            if seventv_url(e):
                out[e["name"]] = {"u": seventv_url(e), "s": "7TV"}
    except Exception as e:
        log.info("7tv global: %s", e)
    return out


async def emotes_channel(login):
    q = {"query": "query($l:String!){user(login:$l){id}}", "variables": {"l": login}}
    d = await jreq("https://gql.twitch.tv/gql", json.dumps(q).encode(),
                   {"Client-Id": TWITCH_GQL_ID, "Content-Type": "application/json"})
    uid = ((d.get("data") or {}).get("user") or {}).get("id")
    out = {}
    if not uid:
        return out
    try:
        b = await jreq(f"https://api.betterttv.net/3/cached/users/twitch/{uid}")
        for e in b.get("channelEmotes", []) + b.get("sharedEmotes", []):
            out[e["code"]] = {"u": f"https://cdn.betterttv.net/emote/{e['id']}/2x", "s": "BTTV " + login}
    except Exception as e:
        log.info("bttv %s: %s", login, e)
    try:
        u = await jreq(f"https://7tv.io/v3/users/twitch/{uid}")
        for e in ((u.get("emote_set") or {}).get("emotes")) or []:
            if seventv_url(e):
                out[e["name"]] = {"u": seventv_url(e), "s": "7TV " + login}
    except Exception as e:
        log.info("7tv %s: %s", login, e)
    return out


# ---- GIFs: GIPHY search (rated PG-13, GIPHY's own moderation), only through the hub so the key stays here
gif_cache = {}   # (query, offset) -> (time, (items, next offset))
GIF_PAGE = 50     # GIPHY's most per request; the picker asks for the next page when scrolled to the end
GIF_MAX_OFFSET = 4950


async def giphy(q, key, offset=0):
    """One page of results: (items, the offset of the next page or None)."""
    k = (q.lower(), offset)
    hit = gif_cache.get(k)
    if hit and time.time() - hit[0] < 600:
        return hit[1]
    params = {"api_key": key, "limit": GIF_PAGE, "offset": offset, "rating": "pg-13", "bundle": "messaging_non_clips"}
    url = GIPHY_URL + ("/search?" + urllib.parse.urlencode({**params, "q": q, "lang": "en"}) if q
                       else "/trending?" + urllib.parse.urlencode(params))
    d = await jreq(url)

    def pick(r):
        u = r.get("webp") or r.get("url") or ""
        host = (urllib.parse.urlsplit(u).hostname or "")
        ok = u.startswith("https://") and (host == "giphy.com" or host.endswith(".giphy.com"))
        return {"url": u, "w": int(r.get("width") or 0), "h": int(r.get("height") or 0)} if ok else None

    items = []
    for g in d.get("data", []):
        im = g.get("images") or {}
        full, small = pick(im.get("fixed_height") or {}), pick(im.get("fixed_height_small") or im.get("fixed_height") or {})
        if full and small and re.fullmatch(r"[A-Za-z0-9]+", str(g.get("id", ""))):
            items.append({"id": g["id"], "title": clean_text(g.get("title", ""), 80), "preview": small, "full": full})
    pg = d.get("pagination") or {}
    seen = offset + int(pg.get("count") or len(d.get("data", [])))
    nxt = seen if d.get("data") and seen < int(pg.get("total_count") or 0) and seen <= GIF_MAX_OFFSET else None
    if len(gif_cache) > 300:
        gif_cache.clear()
    gif_cache[k] = (time.time(), (items, nxt))
    return items, nxt


# ---------------------------------------------------------------- connections
class Denied(Exception):
    pass


class Conn:
    """A browser (kind "web", JSON at /ws), a game (kind "game": its cstvd through cstv_link.py, lines at /game),
    or an older game build's cstv_hublink.py (kind "web", legacy: the website's JSON without an Origin)."""
    seq = 0

    def __init__(self, ws, kind, ip, legacy=False):
        Conn.seq += 1
        self.id = Conn.seq
        self.ws = ws
        self.kind = kind
        self.legacy = legacy
        self.ip = ip
        self.steamid = None         # proven: Steam login (web) or a token from the website (game)
        self.claimed = "0"          # game: the SteamID the game says it plays as
        self.session = None         # game: hash of its token (signing it out on the website ends it)
        self.token = None           # game: the token cstv_link.py offered (checked at hello)
        self.ingame_name = None
        self.room = None
        self.server, self.map = "-", "-"
        self.watching = kind == "game"   # a game's cstvd only connects while CS:TV runs
        self.silent = False         # a site admin who joined a lobby without anyone seeing (not listed, not counted)
        self.leaving = False        # leaving its lobby on purpose (the game's !leave): "X left" at once
        self.hello = False
        self.visit = None           # its row in the visitor log
        self.out = deque()
        self.wake = asyncio.Event()
        self.closing = False
        self.lines = deque(maxlen=200)   # game: when its last lines arrived (flood limit)
        self.cmds = deque(maxlen=40)     # web: rate limit
        self.chats = deque(maxlen=5)
        self.gifs = {}              # GIF ids this connection was shown by a search -> what may be posted
        self.bucket, self.bucket_at = BUCKET_MAX, time.monotonic()

    @property
    def name(self):
        if self.steamid:
            return user_name(self.steamid)
        return self.ingame_name or "guest"

    @property
    def loc(self):
        return f"{self.server}/{self.map}" if self.server != "-" else ""

    def put(self, item):
        """Queue a message (a line for a game, a dict for a browser); writer() sends them in order."""
        if self.closing:
            return
        if len(self.out) >= OUT_MAX:   # it stopped reading: drop it rather than buffer forever
            self.close(1008, "not reading")
            return
        self.out.append(item)
        self.wake.set()

    def close(self, code=1000, why=""):
        if not self.closing:
            self.closing = True
            asyncio.ensure_future(self.ws.close(code, why))

    async def writer(self):
        try:
            while True:
                await self.wake.wait()
                self.wake.clear()
                while self.out:
                    if self.kind == "game":   # several lines per message
                        lines, size = [], 0
                        while self.out and size < 32768:
                            lines.append(self.out.popleft())
                            size += len(lines[-1]) + 1
                        await self.ws.send("\n".join(lines))
                    else:
                        await self.ws.send(json.dumps(self.out.popleft(), separators=(",", ":")))
        except ConnectionClosed:
            pass

    def tell(self, text):
        """A line for this player only (in-game chat / the website's toast)."""
        self.put("msg " + b64e(text) if self.kind == "game" else {"t": "notice", "msg": text})

    def deny(self, text):
        self.put("msg " + b64e(text[:1].upper() + text[1:] + ("" if text.endswith((".", "!", "?")) else "."))
                 if self.kind == "game" else {"t": "denied", "msg": text})

    def spend(self):
        """Control commands from a game share one bucket (daf's master): 8, refilled at one per 2 s."""
        now = time.monotonic()
        self.bucket = min(BUCKET_MAX, self.bucket + (now - self.bucket_at) * BUCKET_RATE)
        self.bucket_at = now
        if self.bucket < 1:
            self.tell("Slow down.")
            return False
        self.bucket -= 1
        return True


conns = set()
lobbies = {}          # code -> Lobby (loaded ones)
srv_rooms = {}        # game server address -> Room
pair_codes = {}       # "Connect game" code from the website -> (steamid, expires); the game redeems it with !tvlogin

JOIN_FAIL = {"nolobby": "There's no lobby with that code.", "banned": "You're banned from that lobby.",
             "password": "That lobby has a password.", "wrong": "Wrong password.",
             "locked": "Too many wrong passwords for that lobby. Try again in a few minutes."}


# ---------------------------------------------------------------- rooms
class Room:
    """What a room shares: the screen per <server>/<map>, the link, the clock, the queue. The clock follows
    daf's master (and the plugin before it): a new link or a seek holds it ("loading") until the first viewer
    reports "started" (at most LOAD_TIMEOUT s); then position = base_pos + (now - base_time) * speed.

    A server room (this class) is everyone with CS:TV on one game server who isn't in a lobby. Anyone in it
    may control it, like the plugin. Lobbies (Lobby below) have an owner, permissions and the website."""
    kind = "srv"

    def __init__(self, key):
        self.key = key
        self.conns = {}             # conn -> None, in join order
        self.url = None
        self.title = None
        self.by = None
        self.rev = 0
        self.paused = False
        self.speed = 100            # percent
        self.base_pos = 0           # ms
        self.base_time = now_ms()
        self.loading = False
        self.load_start = 0.0
        self.seek_id = 0
        self.loop = False
        self.loop_hold = False
        self.loop_at = 0.0
        self.skip_rev = -1
        self.dur = None             # ms, when a browser's player knows it
        self.queue = []             # {uid, url, title, by}
        self.history = []           # [url, pos ms, title, by, when (time.time())], oldest first
        self.resume = self.resume_title = self.resume_list = ""   # the playlist to go back to after the queue
        self.up_next = None
        self.uid_seq = 0
        self.cur_title_uid = 0
        self.screens = {}           # "<server>/<map>" -> "scale float px py pz nx ny nz rx ry rz ux uy uz"
        self.stream = None          # {id, secret, conn, url, live}
        self.tol = -1
        self.shown_viewers = -1
        self.chat = deque(maxlen=60)
        self.chat_id = 0
        self.empty_since = time.time()
        self.dirty = False
        if self.kind == "srv":
            self.screens = {r["loc"]: r["geo"] for r in db.q("SELECT loc, geo FROM srv_screens WHERE room=?", key)}

    # ---- who may
    def perms_of(self, c):
        return PERM_ALL

    def refusal(self, c, what):
        return f"you're not allowed to {what} here"

    def need(self, c, perm, what):
        if not self.perms_of(c) & perm:
            raise Denied(self.refusal(c, what))

    def check_site(self, url):
        pass

    def viewers(self):
        return sum(1 for c in self.conns if c.watching and not c.silent)

    # ---- clock
    def pos(self):
        if self.paused or self.loading or not self.url:
            return self.base_pos
        return self.base_pos + (now_ms() - self.base_time) * self.speed // 100

    def state_line(self):
        return f"state {self.rev} {int(self.paused)} {int(self.pos())} {self.speed} {self.seek_id} {int(not self.loading)}"

    def state_msg(self):
        n = self.viewers()
        kind = link_kind(self.url) if self.url else None
        return {"t": "state", "rev": self.rev, "url": self.url, "title": self.title, "by": self.by,
                "paused": self.paused, "pos": int(self.pos()), "at": now_ms(), "speed": self.speed, "seek": self.seek_id,
                "running": not self.loading, "loop": self.loop, "dur": self.dur, "now": now_ms(),
                "tol": TOL_SHARED if n >= 2 else TOL_ALONE, "viewers": n,
                "live": kind in ("stream", "twitch_live"), "stream": kind == "stream"}

    # ---- sending
    def to_games(self, line, loc=None):
        for c in self.conns:
            if c.kind == "game" and (loc is None or c.loc == loc):
                c.put(line)

    def to_webs(self, msg):
        for c in self.conns:
            if c.kind == "web":
                c.put(msg)

    def send_state(self):
        self.dirty = True
        line, msg = self.state_line(), None
        for c in self.conns:
            if c.kind == "game":
                c.put(line)
            else:
                msg = msg or self.state_msg()
                c.put(msg)

    def queue_line(self):
        return "queue" + "".join(f" {q['uid']},{b64e(q['url'])},{b64o(q['title'])},{b64o(q['by'])}" for q in self.queue)

    def send_queue(self):
        self.dirty = True
        self.to_games(self.queue_line())
        self.to_webs({"t": "queue", "items": self.queue})
        self.update_up_next()

    def update_up_next(self):
        url = self.queue[0]["url"] if self.queue else None
        if url == self.up_next:
            return
        self.up_next = url
        if url:
            self.to_games("upnext " + b64e(url))

    def system(self, text):
        """A line everyone in the room reads: the website's chat (lobbies), the game's chat."""
        self.to_games("msg " + b64e(text))

    say = system

    def send_screen(self, c):
        g = self.screens.get(c.loc) if c.loc else None
        c.put("screen " + g if g else "screen none")

    def send_everything(self, c):
        """What a game needs to show this room (after joining or moving to another map)."""
        self.send_screen(c)
        if self.url:
            c.put(f"url {self.rev} {b64e(self.url)}")
            c.put(self.state_line())
            c.put("nowtitle " + b64o(self.title))
        else:
            c.put("stop")
        c.put(self.queue_line())
        c.put(self.history_line())
        if self.up_next:
            c.put("upnext " + b64e(self.up_next))
        c.put(f"tol {TOL_SHARED if self.viewers() >= 2 else TOL_ALONE}")

    def update_tol(self, skip=None):
        n = self.viewers()
        tol = TOL_SHARED if n >= 2 else TOL_ALONE
        if tol != self.tol:
            self.tol = tol
            for c in self.conns:
                if c.kind == "game" and c is not skip:
                    c.put(f"tol {tol}")
        if n != self.shown_viewers:
            self.shown_viewers = n
            if any(c.kind == "web" for c in self.conns):
                self.to_webs(self.state_msg())

    def owner_conn_id(self):
        return "-"

    def roster_line(self):
        parts = ["roster " + self.owner_conn_id()]
        for c in self.conns:
            if not c.silent:
                parts.append(f"{c.id},{c.steamid or 0},{b64o(c.name)},{b64o(c.loc if c.kind == 'game' else 'website')}")
        return " ".join(parts)

    def send_roster(self):
        if any(c.kind == "game" for c in self.conns):
            self.to_games(self.roster_line())

    def room_line(self, c):
        return f"room srv {self.key} 0 all - 0 -"

    def send_rooms(self):
        for c in self.conns:
            if c.kind == "game":
                c.put(self.room_line(c))

    # ---- playback (daf's master)
    def set_link(self, url, keep_speed, start=0, record=True, finished=False, title=None, by=None):
        old = self.url
        if record and old and link_kind(old) != "stream":   # a stream's link is dead once it ends
            if len(self.history) >= HISTORY_MAX:
                self.history.pop(0)
            self.history.append([old, 0 if finished else int(self.pos()), self.title or "", self.by or "", time.time()])
            self.send_history()
        self.url = url
        self.title = title or None
        self.by = by
        self.rev += 1
        self.paused = False
        self.loop = self.loop_hold = False
        if not keep_speed:
            self.speed = 100
        self.base_pos = int(start) or link_start(url)
        self.base_time = now_ms()
        self.loading = True   # the clock starts when a viewer's video does ("started")
        self.load_start = time.monotonic()
        self.dur = None
        self.to_games(f"url {self.rev} {b64e(url)}")
        self.send_state()
        self.to_games("nowtitle " + b64o(self.title))
        self.update_up_next()
        if not title:
            self.uid_seq += 1
            self.cur_title_uid = self.uid_seq
            self.lookup_title(self.cur_title_uid, url)
        if self.stream and old == self.stream["url"] and url != old:
            self.end_stream()   # something else replaced the live stream
        if isinstance(self, Lobby) and not self.emotes:   # chat's emotes follow the Twitch channel playing
            self.to_games("lchat_emotes " + (self.emote_channel() or "-"))
        if isinstance(self, Lobby) and yt_video_id(old) != yt_video_id(url):   # the stream chat panel follows the video
            for c in self.conns:
                if c.kind == "web":
                    yt_unsubscribe(c)
            yt_announce(self)

    def history_msg(self):
        return {"t": "history", "items": [{"url": e[0], "title": e[2] or None, "by": (e[3] if len(e) > 3 else "") or None,
                                           "at": e[4] if len(e) > 4 else None} for e in reversed(self.history)]}

    def history_line(self):
        """For games (the Lobby page's History): newest first, url,title,by,when."""
        return "history" + "".join(f" {b64e(e[0])},{b64o(e[2] if len(e) > 2 else '')},{b64o(e[3] if len(e) > 3 else '')},"
                                   f"{int(e[4]) if len(e) > 4 and e[4] else 0}" for e in reversed(self.history[-30:]))

    def send_history(self):
        if any(c.kind == "web" for c in self.conns):
            self.to_webs(self.history_msg())
        if any(c.kind == "game" for c in self.conns):
            self.to_games(self.history_line())

    def start_clock(self):
        self.loading = False
        self.loop_hold = False
        self.base_time = now_ms()
        self.send_state()

    def stop(self):
        old = self.url
        self.url = self.title = self.by = None
        self.rev += 1
        self.paused = self.loading = self.loop = False
        self.base_pos, self.base_time, self.dur = 0, now_ms(), None
        self.dirty = True
        self.to_games("stop")
        self.to_webs(self.state_msg())
        if self.stream and old == self.stream["url"]:
            self.end_stream()

    def end_stream(self):
        s, self.stream = self.stream, None
        if not s:
            return
        s["conn"].put("stream_end")
        if self.url == s["url"]:
            self.stop()
        log.info("stream %s in %s ended", s["id"], self.key)

    def queue_push(self, url, by, front=False, title=None):
        self.uid_seq += 1
        item = {"uid": self.uid_seq, "url": url, "title": title or None, "by": by}
        if front:
            self.queue.insert(0, item)
        else:
            self.queue.append(item)
        self.send_queue()
        if not title:
            self.lookup_title(item["uid"], url)

    def play_queued(self, i, finished):
        item = self.queue.pop(i)
        self.send_queue()
        self.set_link(item["url"], False, 0, True, finished, item["title"], item["by"])
        left = "" if self.queue else " (queue now empty)"
        self.say(f"From the queue ({item['by']}): {item['title'] or short_url(item['url'])}{left}")

    def advance(self, helper_next, helper_title, finished):
        """The current video is over (or skipped). Order: the queue, the playlist, an interrupted playlist."""
        lst = yt_list(self.url or "")
        if self.queue:
            if lst:
                self.resume_list = lst
                self.resume, self.resume_title = helper_next, helper_title
            self.play_queued(0, finished)
            return True
        if helper_next:
            self.set_link(helper_next, True, 0, True, finished, helper_title, self.by)
            self.say(f"Up next: {helper_title}" if helper_title else "Up next in the playlist.")
            return True
        if self.resume_list and self.resume:
            url, title = self.resume, self.resume_title
            self.resume = self.resume_title = self.resume_list = ""
            self.set_link(url, True, 0, True, finished, title, self.by)
            self.say(f"Back to the playlist: {title}" if title else "Back to the playlist.")
            return True
        return False

    def loop_restart(self):
        self.base_pos = 0
        self.base_time = now_ms()
        self.loading = True
        self.loop_hold = True
        self.load_start = self.loop_at = time.monotonic()
        self.seek_id += 1
        self.send_state()

    def tick(self):
        if self.url and self.loading and time.monotonic() - self.load_start > LOAD_TIMEOUT:
            self.start_clock()
        s = self.stream   # the streamer says it's pushing, the relay never said "ready": go anyway (a relay without the hook)
        if s and s.get("claimed") and not s["live"] and time.monotonic() - s["claimed"] > STREAM_READY_WAIT:
            log.warning("stream %s: no word from the relay in %d s, switching to it anyway", s["id"], STREAM_READY_WAIT)
            self.stream_go_live()
        # a video whose length a browser reported ends on the hub's clock too (nobody watching, all viewers gone)
        if self.url and self.dur and not self.loading and not self.paused and self.pos() > self.dur + 3000:
            self.on_ended(self.rev)

    # ---- reports from viewers
    def on_started(self, rev, seek=None):
        if self.loading and rev == self.rev and (seek is None or seek == self.seek_id):
            self.start_clock()

    def on_ended(self, rev, nxt="", title=""):
        if not self.url or rev != self.rev:
            return   # another viewer's report already moved us on
        if link_kind(self.url) == "stream":
            return   # a viewer whose player gave up: a stream ends when its streamer stops it, not then
        if self.loop and self.skip_rev != self.rev:
            if self.loop_hold or time.monotonic() - self.loop_at < 3.0:
                return
            if not self.loading:
                self.loop_restart()
                return
        if nxt and url_ok(nxt) and yt_list(nxt) and yt_list(nxt) == yt_list(self.url):
            title = clean_text(title, 190)
        else:
            nxt, title = "", ""
        if not self.advance(nxt, title, self.skip_rev != self.rev):
            self.stop()

    def on_title(self, uid, title):
        title = clean_text(title, 190)
        if not title:
            return
        if uid == self.cur_title_uid and self.url and not self.title:
            self.title = title
            self.to_games("nowtitle " + b64o(title))
            self.to_webs(self.state_msg())
            self.dirty = True
        for q in self.queue:
            if q["uid"] == uid and q["title"] != title:
                q["title"] = title
                self.send_queue()

    def lookup_title(self, uid, url):
        """YouTube / Twitch here; anything else by a viewer's helper (yt-dlp), as in daf's master."""
        async def run():
            t = await fetch_title(url)
            if t:
                self.on_title(uid, t)
                return
            c = next((c for c in self.conns if c.kind == "game"), None)
            if c:
                c.put(f"title {uid} {b64e(url)}")
        asyncio.ensure_future(run())

    # ---- commands (game lines and website messages)
    def cmd_link(self, c, url):
        self.need(c, PERM_LINK, "play links")
        url = url.strip()
        if not url_ok(url):
            raise Denied("that doesn't look like a link (http:// or https://)")
        self.check_site(url)
        self.set_link(url, False, by=c.name)
        self.resume = self.resume_title = self.resume_list = ""
        self.say(f"{c.name} is playing {short_url(url)}")

    def cmd_queue(self, c, url):
        self.need(c, PERM_QUEUE, "add to the queue")
        url = url.strip()
        if not url_ok(url):
            raise Denied("that doesn't look like a link (http:// or https://)")
        self.check_site(url)
        if not self.url:
            self.set_link(url, False, by=c.name)
            self.say(f"{c.name} is playing {short_url(url)}")
            return
        if len(self.queue) >= QUEUE_MAX:
            raise Denied(f"the queue is full ({QUEUE_MAX})")
        self.queue_push(url, c.name)
        self.say(f"{c.name} queued {short_url(url)} (#{len(self.queue)})")

    def queue_index(self, uid):
        i = next((i for i, q in enumerate(self.queue) if q["uid"] == uid), None)
        if i is None:
            raise Denied("that's no longer in the queue")
        return i

    def cmd_unqueue(self, c, uid):
        self.need(c, PERM_QUEUE, "change the queue")
        item = self.queue.pop(self.queue_index(uid))
        self.send_queue()
        self.say(f"{c.name} removed {item['title'] or short_url(item['url'])} from the queue.")

    def cmd_playnow(self, c, uid):
        self.need(c, PERM_LINK, "play links")
        self.play_queued(self.queue_index(uid), False)

    def cmd_tofront(self, c, uid):
        self.need(c, PERM_QUEUE, "change the queue")
        self.queue.insert(0, self.queue.pop(self.queue_index(uid)))
        self.send_queue()

    def cmd_clearqueue(self, c):
        self.need(c, PERM_QUEUE, "change the queue")
        self.queue.clear()
        self.resume = self.resume_title = self.resume_list = ""
        self.send_queue()
        self.say(f"{c.name} cleared the queue.")

    def cmd_next(self, c):
        self.need(c, PERM_SKIP, "skip")
        if not self.url:
            if not self.queue:
                raise Denied("nothing is playing")
            self.play_queued(0, False)
            return
        if not yt_list(self.url) or self.queue:
            if not self.advance("", "", False):
                self.stop()
            self.say(f"{c.name} skipped.")
            return
        # a playlist: its next video is known to the viewers' helpers, not here
        g = c if c.kind == "game" and c in self.conns else next((x for x in self.conns if x.kind == "game"), None)
        if not g:
            self.stop()
        else:
            self.skip_rev = self.rev
            g.put("skip")
        self.say(f"{c.name} skipped to the next video.")

    def cmd_prev(self, c):
        self.need(c, PERM_SKIP, "go back")
        if not self.history:
            raise Denied("nothing to go back to")
        url, pos, title = self.history.pop()[:3]
        self.send_history()
        cur = self.url
        if cur and link_kind(cur) != "stream" and not (yt_list(cur) and yt_list(cur) == yt_list(url)):
            if len(self.queue) >= QUEUE_MAX:
                self.queue.pop()
            self.queue_push(cur, c.name, True, self.title)
        self.set_link(url, True, pos, False, False, title, c.name)
        label = title or short_url(url)
        self.say(f"{c.name} went back to {label}" + (f" (from {clock_label(pos / 1000)})." if pos > 1000 else "."))

    def cmd_pause(self, c, want=None):
        self.need(c, PERM_PAUSE, "pause")
        if not self.url:
            return
        if link_kind(self.url) == "stream":
            raise Denied("a live stream can't be paused")
        want = (not self.paused) if want is None else bool(want)
        if want == self.paused:
            return
        if want:
            self.base_pos = self.pos()
            self.paused = True
        else:
            self.base_time = now_ms()
            self.paused = False
        self.send_state()
        self.say(f"{c.name} {'paused' if self.paused else 'resumed'} the video.")

    def cmd_seek(self, c, spec):
        """spec: "+30" / "-10" / "1:23" (game), or ms as a number (website: absolute) / ("rel", ms)."""
        self.need(c, PERM_SEEK, "seek")
        if not self.url:
            raise Denied("nothing is playing")
        if link_kind(self.url) in ("twitch_live", "stream"):
            raise Denied("a live stream can't be seeked")
        if isinstance(spec, str):
            rel = spec[:1] in ("+", "-")
            t = parse_clock(spec[1:] if rel else spec)
            if t is None:
                raise Denied("seek with !tv +30, !tv -10 or !tv 1:23")
            ms = int(t * 1000)
            pos = max(0, self.pos() + (-ms if spec[0] == "-" else ms)) if rel else ms
            label = (f"{c.name} skipped {'back' if spec[0] == '-' else 'forward'} {int(t)} s (now at {clock_label(pos / 1000)})."
                     if rel else f"{c.name} jumped to {clock_label(pos / 1000)}.")
        else:
            rel = isinstance(spec, tuple)
            pos = max(0, int(self.pos() + spec[1] if rel else spec))
            label = f"{c.name} jumped to {clock_label(pos / 1000)}."
        self.base_pos = int(pos)
        self.base_time = now_ms()
        self.loading = True
        self.load_start = time.monotonic()
        self.seek_id += 1
        self.send_state()
        self.say(label)

    def cmd_speed(self, c, pct):
        self.need(c, PERM_PLAYBACK, "change the speed")
        pct = int(pct)
        if not 25 <= pct <= 400 or not self.url or link_kind(self.url) == "stream":
            return
        self.base_pos = self.pos()
        self.base_time = now_ms()
        self.speed = pct
        self.send_state()
        self.say(f"{c.name} set the playback speed to {pct / 100:g}x.")

    def cmd_loop(self, c, on):
        self.need(c, PERM_PLAYBACK, "loop videos")
        if not self.url or bool(on) == self.loop:
            return
        self.loop = bool(on)
        self.send_state()
        self.say(f"{c.name} turned looping {'on' if self.loop else 'off'}.")

    def cmd_stop(self, c):
        self.need(c, PERM_LINK, "stop the video")
        if self.url:
            self.stop()
            self.say(f"{c.name} stopped the video.")

    def save_screen(self, loc):
        if self.kind == "srv":
            if loc in self.screens:
                db.x("INSERT OR REPLACE INTO srv_screens VALUES(?,?,?,?)", self.key, loc, self.screens[loc], time.time())
            else:
                db.x("DELETE FROM srv_screens WHERE room=? AND loc=?", self.key, loc)
        self.dirty = True

    def cmd_place(self, c, a):
        self.need(c, PERM_SCREEN, "place the TV")
        if not c.loc:
            raise Denied("join a game server first")
        scale, floating = float(a[0]), int(a[1])
        vec = [float(x) for x in a[2:14]]
        if not SCALE_MIN <= scale <= SCALE_MAX or floating not in (0, 1) or len(vec) != 12:
            return
        if not all(map(math.isfinite, vec)) or any(abs(x) > 65536 for x in vec[:3]) or any(abs(x) > 1.01 for x in vec[3:]):
            return
        self.screens[c.loc] = f"{scale:g} {floating} " + " ".join(f"{x:.3f}" for x in vec)
        self.save_screen(c.loc)
        for x in self.conns:
            if x.kind == "game" and x.loc == c.loc:
                self.send_screen(x)
        self.say(f"{c.name} placed a {'floating ' if floating else ''}TV. !tvhelp for commands")

    def cmd_remove(self, c):
        self.need(c, PERM_SCREEN, "remove the TV")
        if not c.loc or self.screens.pop(c.loc, None) is None:
            return
        self.save_screen(c.loc)
        for x in self.conns:
            if x.kind == "game" and x.loc == c.loc:
                self.send_screen(x)
        if self.url and self.kind == "srv" and not self.screens:
            self.stop()
        self.say(f"{c.name} removed the TV.")

    def cmd_stream(self, c, sub):
        s = self.stream
        if sub == "start":
            if self.kind != "lobby":
                raise Denied("streaming works in a lobby, for its owner and admins")
            if not self.perms_of(c) & ADMIN:
                raise Denied("only the lobby's owner and admins can stream")
            if not STREAM_HOST:
                raise Denied("streams aren't set up on this server yet")
            if c.kind != "game":
                return
            if s and s["conn"] is not c:
                raise Denied(f"{s['conn'].name} is already streaming here")
            if not s:
                sid = secrets.token_hex(8)
                s = self.stream = {"id": sid, "secret": secrets.token_urlsafe(18), "conn": c, "live": False,
                                   "url": f"rtmp://{STREAM_HOST}:{RTMP_PORT}/live/{sid}"}
                log.info("%s (%s) starts stream %s in %s", c.name, c.steamid or c.claimed, sid, self.key)
            c.put("stream_go " + b64e(f"{s['url']}?key={s['secret']}"))
        elif not s or s["conn"] is not c:
            return
        elif sub == "live" and not s["live"]:   # the streamer's encoder is pushing; the room switches once the relay has it
            s["claimed"] = s.get("claimed") or time.monotonic()
            if s.get("ready"):
                self.stream_go_live()
        elif sub == "stop":
            self.end_stream()

    def stream_go_live(self):
        s = self.stream
        if not s or s["live"]:
            return
        s["live"] = True
        c = s["conn"]
        self.set_link(s["url"], False, title=f"{c.name}'s stream", by=c.name)
        self.resume = self.resume_title = self.resume_list = ""
        self.say(f"{c.name} is streaming.")


class Lobby(Room):
    """A lobby: a code anyone can join (with the password, if it has one), from the website or the game, on any
    server. The creator owns it; the plugin's 7 permission switches say what the others may do. Kept in the
    database (settings, members, bans, and what played, paused, after a restart)."""
    kind = "lobby"

    def __init__(self, row):
        super().__init__(row["code"])
        self.code = row["code"]
        self.name = row["name"]
        self.owner = row["owner"]
        self.default_perms = row["default_perms"]
        self.sites = row["sites"]
        self.slow = row["slow"] or 0                     # chat slow mode, seconds (admins exempt)
        self.gifs = row["gifs"] is None or bool(row["gifs"])
        self.emotes = row["emotes"]                      # Twitch channel whose BTTV/7TV emotes chat uses
        self.password = row["password"] or ""
        self.last_chat = {}                              # steamid -> time of their last message
        self.members = {r["steamid"]: {"perms": r["perms"], "admin": bool(r["admin"]), "pw": r["pw"]}
                        for r in db.q("SELECT * FROM members WHERE code=?", self.code)}
        self.bans = {r["steamid"] for r in db.q("SELECT steamid FROM bans WHERE code=?", self.code)}
        self.ip_bans = set()      # banned players who weren't signed in (until the hub restarts)
        self.mutes = {}           # steamid -> until (time.time()): timed out
        self.pass_fails = {}      # ip -> (count, first failure)
        saved = json.loads(row["saved"]) if row["saved"] else None
        if saved:   # after a restart: as it was, paused where it got to
            st = saved.get("st") or {}
            self.url, self.title, self.by = st.get("url"), st.get("title"), st.get("by")
            self.rev = int(st.get("rev") or 0)
            self.base_pos = int(st.get("pos") or 0)
            self.speed = int(st.get("speed") or 100)
            self.seek_id = int(st.get("seek") or 0)
            self.loop = bool(st.get("loop"))
            self.dur = st.get("dur")
            self.paused = bool(self.url)
            if self.url and link_kind(self.url) == "stream":
                self.url = self.title = self.by = None   # a stream is gone after a restart
                self.paused = False
            self.queue = saved.get("queue") or []
            self.screens = saved.get("screens") or {}
            self.history = saved.get("history") or []
            self.uid_seq = max([q["uid"] for q in self.queue] + [0])
        self.left_pending = {}    # who -> (time.monotonic(), name or None for a guest): gone, "left" not said yet
        self.joined_said = {}     # who -> time.monotonic() of their last "joined"
        self.announced = set()    # who got a "joined" (so their leaving is said too)

    # ---- who may
    def perms_sid(self, steamid):
        if not steamid:
            return 0
        if is_site_admin(steamid):
            return PERM_ALL | ADMIN | OWNER | SITEADMIN
        if steamid in self.bans:
            return 0
        if steamid == self.owner:
            return PERM_ALL | ADMIN | OWNER
        m = self.members.get(steamid)
        if m and m["admin"]:
            return PERM_ALL | ADMIN
        return self.default_perms if not m or m["perms"] is None else m["perms"]

    def perms_of(self, c):
        return 0 if c.ip in self.ip_bans and not c.steamid else self.perms_sid(c.steamid)

    def refusal(self, c, what):
        if not c.steamid:
            return (f"sign in to {what}: get a code at {SITE}/link, then type !tvlogin <code>" if c.kind == "game"
                    else f"sign in with Steam to {what}")
        return f"you're not allowed to {what} in this lobby"

    def check_site(self, url):
        if self.sites != "any" and site_of(url) not in ("youtube", "twitch"):
            raise Denied("this lobby only plays YouTube and Twitch links")

    def banned(self, c):
        if is_site_admin(c.steamid):
            return False
        return (c.steamid and c.steamid in self.bans) or (not c.steamid and c.ip in self.ip_bans)

    def remember(self, steamid):
        if steamid and steamid not in self.members:
            self.members[steamid] = {"perms": None, "admin": False, "pw": None}
            db.x("INSERT OR IGNORE INTO members(code, steamid, perms, admin) VALUES(?,?,NULL,0)", self.code, steamid)

    def pw_tag(self):
        return hashlib.sha256(f"{self.code}\0{self.password}".encode()).hexdigest()[:16]

    def admit(self, c, given):
        """None when c may come in, else why not: banned / password / wrong / locked. Signed-in players who got
        in with the current password before don't need it again; the owner and admins never do."""
        if self.banned(c):
            return "banned"
        if not self.password:
            return None
        if c.steamid and (self.perms_sid(c.steamid) & ADMIN or (self.members.get(c.steamid) or {}).get("pw") == self.pw_tag()):
            return None
        now = time.time()
        n, first = self.pass_fails.get(c.ip, (0, now))
        if now - first > PASS_LOCKOUT:
            n, first = 0, now
        if n >= PASS_TRIES:
            return "locked"
        if not given:
            return "password"
        if hmac.compare_digest(given.encode(), self.password.encode()):
            self.pass_fails.pop(c.ip, None)
            if c.steamid:
                self.remember(c.steamid)
                self.members[c.steamid]["pw"] = self.pw_tag()
                db.x("UPDATE members SET pw=? WHERE code=? AND steamid=?", self.pw_tag(), self.code, c.steamid)
            return None
        self.pass_fails[c.ip] = (n + 1, first)
        return "wrong"

    # ---- sending
    def system(self, text, game_print=True):
        """A notice in the lobby chat; game_print: also printed in the games' own chat."""
        self.chat_id += 1
        m = {"id": self.chat_id, "sys": True, "text": text, "ts": time.time()}
        self.chat.append(m)
        self.to_webs({"t": "chat", "msg": m})
        if game_print:
            self.to_games("msg " + b64e(text))
        self.to_games(lchat_line(m))

    # ---- "X joined" / "X left" in the chat: once per person (their game and their browser are one person; a
    # guest is their IP), never for a silent site admin, nothing for a reload or a reconnect
    @staticmethod
    def who(c):
        return c.steamid or "ip:" + c.ip

    def present(self, key, skip=None):
        return any(x is not skip and not x.silent and self.who(x) == key for x in self.conns)

    def on_enter(self, c):
        if c.silent:
            return
        key = self.who(c)
        if self.present(key, skip=c) or self.left_pending.pop(key, None):
            return   # already here on another connection / back before their "left" was said
        now = time.monotonic()
        if now - self.joined_said.get(key, -JOIN_QUIET) < JOIN_QUIET:
            return
        self.joined_said[key] = now
        self.announced.add(key)
        if any(x is not c and not x.silent for x in self.conns):   # (nobody to tell when they're the first in)
            self.system(f"{c.name} joined" if c.steamid else "A guest joined")

    def on_leave(self, c, at_once=False):
        """at_once: they left on purpose (the game's !leave), not a closed tab that may come back."""
        if c.silent:
            return
        key = self.who(c)
        if key in self.announced and not self.present(key):
            self.left_pending[key] = (time.monotonic() - (LEFT_GRACE if at_once else 0), c.name if c.steamid else None)
            if at_once:
                self.say_left()

    def say_left(self):
        now = time.monotonic()
        for key, (t, name) in list(self.left_pending.items()):
            if now - t >= LEFT_GRACE:
                del self.left_pending[key]
                self.announced.discard(key)
                if not self.present(key) and any(not x.silent for x in self.conns):
                    self.system(f"{name} left" if name else "A guest left")

    def tick(self):
        super().tick()
        self.say_left()

    say = system

    def post_chat(self, c, text, gif=None):
        self.chat_id += 1
        msg = {"id": self.chat_id, "steamid": c.steamid, "name": c.name, "text": text, "ts": time.time(), "from": c.kind}
        if gif:
            msg["gif"] = gif
        self.chat.append(msg)
        self.to_webs({"t": "chat", "msg": msg})
        shown = text or f"[GIF] {gif.get('title') or ''}".strip()
        self.to_games(f"chat {b64o(c.name)} {b64o(shown)} {'game' if c.kind == 'game' else 'web'}")
        self.to_games(lchat_line(msg))

    def emote_channel(self):
        """Whose BTTV / 7TV emotes chat shows: the lobby's setting, else the Twitch channel playing (as the website)."""
        if self.emotes:
            return self.emotes
        m = re.match(r"https?://(?:www\.|m\.)?twitch\.tv/(\w+)/?$", self.url or "")
        return m.group(1).lower() if m else ""

    def send_lchat_all(self, c):
        """A game's Lobby page: the chat so far and whose emotes it uses."""
        c.put("lchat_reset")
        for m in list(self.chat)[-50:]:
            c.put(lchat_line(m))
        c.put("lchat_emotes " + (self.emote_channel() or "-"))

    def owner_conn_id(self):
        return next((str(c.id) for c in self.conns if c.steamid and c.steamid == self.owner), "-")

    def member_list(self):
        online = {}
        for c in self.conns:
            if c.steamid and not c.silent:
                o = online.setdefault(c.steamid, {"web": False, "game": False, "watching": False, "where": None})
                o["game" if c.kind == "game" else "web"] = True
                o["watching"] |= c.watching
                if c.kind == "game" and c.loc:
                    o["where"] = c.loc
        out = []
        for sid in set(self.members) | set(online) | {self.owner}:
            r = user_row(sid)
            o = online.get(sid)
            out.append({"steamid": sid, "name": r["name"] if r and r["name"] else sid, "avatar": r["avatar"] if r else None,
                        "perms": self.perms_sid(sid), "own": (self.members.get(sid) or {}).get("perms"),
                        "online": bool(o), "web": bool(o and o["web"]), "game": bool(o and o["game"]),
                        "watching": bool(o and o["watching"]), "where": o["where"] if o else None,
                        "muted": self.mutes.get(sid, 0) > time.time()})
        out.sort(key=lambda m: (not m["online"], -(m["perms"] & OWNER), -(m["perms"] & ADMIN), m["name"].lower()))
        guests = [{"id": c.id, "name": c.name, "game": c.kind == "game"} for c in self.conns if not c.steamid and not c.silent]
        return out, guests

    def members_msg(self):
        lst, guests = self.member_list()
        return {"t": "members", "list": lst, "guests": len(guests), "guest_list": guests,
                "banned": [{"steamid": s, "name": user_name(s)} for s in sorted(self.bans)]}

    def send_roster(self):
        super().send_roster()
        if any(c.kind == "web" for c in self.conns):
            self.to_webs(self.members_msg())

    def you_msg(self, c):
        p = self.perms_of(c)
        return {"t": "you", "steamid": c.steamid, "name": c.name if c.steamid else None, "perms": p,
                "password": self.password if p & ADMIN else None, "silent": c.silent}

    def settings_msg(self):
        return {"t": "settings", "name": self.name, "default_perms": self.default_perms, "sites": self.sites,
                "slow": self.slow, "gifs": self.gifs, "emotes": self.emotes, "locked": bool(self.password)}

    def settings_line(self):
        """lsettings <sites> <slow s> <gifs 0|1> <emote channel|-> <default perms>: the game's lobby Settings tab."""
        return f"lsettings {self.sites} {self.slow} {int(self.gifs)} {self.emotes or '-'} {self.default_perms}"

    def room_line(self, c):
        admin = bool(self.perms_of(c) & ADMIN)
        pw = b64e(self.password) if admin and self.password else "-"
        perm = "all" if self.default_perms & PERM_LINK else "owner"
        return f"room lobby {self.code} {int(admin)} {perm} {b64o(self.name)} {int(bool(self.password))} {pw}"

    def send_rooms(self):
        """Settings or someone's rights changed: every game its room line, every browser the settings + its rights."""
        self.dirty = True
        settings = self.settings_msg()
        for c in self.conns:
            if c.kind == "game":
                c.put(self.room_line(c))
                c.put(self.settings_line())
            else:
                c.put(settings)
                c.put(self.you_msg(c))

    def snapshot(self, c):
        m = self.members_msg()
        return {**self.settings_msg(), "t": "lobby", "code": self.code, "owner": self.owner, "state": self.state_msg(),
                "queue": self.queue, "chat": list(self.chat), "members": m["list"], "guests": m["guests"],
                "guest_list": m["guest_list"], "banned": m["banned"], "you": self.you_msg(c),
                "history": self.history_msg()["items"]}


def lchat_line(m):
    """lchat <id> <steamid|-> <name> <text> <web|game|sys> <gif url|-> (b64, "-" = empty): one lobby chat message."""
    gif = (m.get("gif") or {}).get("url") or ""
    return (f"lchat {m['id']} {m.get('steamid') or '-'} {b64o(m.get('name') or '')} {b64o(m.get('text') or '')} "
            f"{'sys' if m.get('sys') else m.get('from') or 'web'} {b64o(gif)}")


def srv_room(addr):
    r = srv_rooms.get(addr)
    if not r:
        r = srv_rooms[addr] = Room(addr)
    return r


def load_lobby(code):
    code = str(code or "").upper().strip()
    if code in lobbies:
        return lobbies[code]
    if not re.fullmatch(r"[A-Z0-9]{4,8}", code):
        return None
    row = db.one("SELECT * FROM lobbies WHERE code=?", code)
    if not row:
        return None
    lb = Lobby(row)
    lobbies[code] = lb
    return lb


def save_lobby(lb):
    st = {"rev": lb.rev, "url": lb.url, "title": lb.title, "by": lb.by, "pos": int(lb.pos()), "speed": lb.speed,
          "seek": lb.seek_id, "loop": lb.loop, "dur": lb.dur}
    db.x("UPDATE lobbies SET saved=?, last=?, name=?, default_perms=?, sites=?, slow=?, gifs=?, emotes=?, password=?, owner=? "
         "WHERE code=?", json.dumps({"st": st, "queue": lb.queue, "screens": lb.screens, "history": lb.history}),
         time.time(), lb.name, lb.default_perms, lb.sites, lb.slow, int(lb.gifs), lb.emotes, lb.password, lb.owner, lb.code)
    lb.dirty = False


def new_lobby(c, name, password=""):
    if not c.steamid:
        raise Denied(f"sign in to create a lobby: get a code at {SITE}/link, then type !tvlogin <code>" if c.kind == "game"
                     else "sign in with Steam to create a lobby")
    own = owned_lobby(c.steamid)
    if own:
        raise Denied(f"you already have a lobby ({own}): !lobby {own} to go there, or delete it on {SITE} first"
                     if c.kind == "game" else f"you already have a lobby ({own})")
    code = new_code()
    while db.one("SELECT 1 FROM lobbies WHERE code=?", code):
        code = new_code()
    name = clean_text(name or f"{c.name}'s lobby", LABEL_MAX) or f"{c.name}'s lobby"
    db.x("INSERT INTO lobbies(code, name, owner, default_perms, sites, created, last, password, seen) VALUES(?,?,?,?,?,?,?,?,?)",
         code, name, c.steamid, PERM_ALL, "any", time.time(), time.time(), (password or "")[:PASS_MAX], time.time())
    return load_lobby(code)


def owned_lobby(steamid):
    """The code of the lobby this player owns, if they may not create another (one each; cstv.xyz admins any number)."""
    if not steamid or is_site_admin(steamid):
        return None
    r = db.one("SELECT code FROM lobbies WHERE owner=? ORDER BY created LIMIT 1", steamid)
    return r["code"] if r else None


# ---------------------------------------------------------------- joining and leaving
def enter(c, r):
    if c.room is r:
        return
    leave_room(c)
    c.room = r
    r.conns[c] = None
    if isinstance(r, Lobby) and not c.silent:
        r.remember(c.steamid)
    r.update_tol(skip=c)
    if c.kind == "game":
        c.put(r.room_line(c))
        r.send_roster()
        r.send_everything(c)
        if isinstance(r, Lobby):
            c.put(r.settings_line())
            r.send_lchat_all(c)
    else:
        c.put(r.snapshot(c))
        r.send_roster()
        if isinstance(r, Lobby):
            yt_announce(r, c)
    if isinstance(r, Lobby):
        r.on_enter(c)   # after its snapshot: the joiner sees the notice once


def leave_room(c):
    r = c.room
    if not r:
        return
    del r.conns[c]
    c.room = None
    if c.kind == "web":
        yt_unsubscribe(c)
    if isinstance(r, Lobby):
        r.on_leave(c, at_once=c.leaving)
    if r.stream and r.stream["conn"] is c:
        r.end_stream()
    r.update_tol()
    r.send_roster()
    if not r.conns:
        r.empty_since = time.time()
        if isinstance(r, Lobby):
            db.x("UPDATE lobbies SET seen=? WHERE code=?", r.empty_since, r.code)


def go_home(c):
    """A game out of a lobby: back to its game server's room, or nowhere when it's not on a server."""
    if c.kind != "game":
        leave_room(c)
        return
    if c.server != "-":
        enter(c, srv_room(c.server))
        return
    leave_room(c)
    for line in ("room srv - 0 all - 0 -", "roster -", "screen none", "stop", "queue"):
        c.put(line)


def kick(lb, target, ban, by):
    """Out of the lobby (a game goes back to its server's room)."""
    why = f"{'Banned' if ban else 'Kicked'} from lobby {lb.code} by {by}."
    if target.kind == "game":
        target.put("kicked " + b64e(why))
        go_home(target)
    else:
        target.put({"t": "kicked", "msg": "you were banned from this lobby" if ban else "you were kicked"})
        leave_room(target)


def admin_kick(lb, actor, sid=None, conn=None, ban=False):
    """Kick or ban a signed-in player (sid: all their connections) or one guest connection (by IP for a ban)."""
    me = lb.perms_of(actor)
    if sid:
        if lb.perms_sid(sid) & SITEADMIN:
            raise Denied("a cstv.xyz admin can't be kicked or banned")
        if sid == lb.owner:
            raise Denied("the owner can't be kicked")
        if lb.perms_sid(sid) & ADMIN and not me & OWNER:
            raise Denied("only the owner can kick an admin")
        if ban:
            lb.bans.add(sid)
            db.x("INSERT OR REPLACE INTO bans VALUES(?,?,?,?)", lb.code, sid, actor.steamid, time.time())
            purge(lb, lambda x: x.get("steamid") == sid)
        for x in [x for x in lb.conns if x.steamid == sid]:
            kick(lb, x, ban, actor.name)
        lb.system(f"{user_name(sid)} was {'banned' if ban else 'kicked'}.")
    elif conn:
        if ban:
            lb.ip_bans.add(conn.ip)
        kick(lb, conn, ban, actor.name)
        lb.system(f"{conn.name} was {'banned' if ban else 'kicked'}.")
    lb.send_roster()


def purge(lb, match):
    ids = [x["id"] for x in lb.chat if match(x)]
    if ids:
        lb.chat = deque((x for x in lb.chat if x["id"] not in ids), maxlen=lb.chat.maxlen)
        lb.to_webs({"t": "chat_delete", "ids": ids})
        lb.to_games("lchat_del " + " ".join(str(i) for i in ids))


def chat_allowed(lb, c, now):
    if not c.steamid:
        raise Denied(lb.refusal(c, "chat"))
    if lb.perms_sid(c.steamid) == 0 and c.steamid in lb.bans:
        raise Denied("you're banned from this lobby")
    if lb.mutes.get(c.steamid, 0) > time.time():
        raise Denied(f"you're timed out for {fmt_left(lb.mutes[c.steamid] - time.time())}")
    left = global_mute_left(c.steamid)
    if left:
        raise Denied("you're muted on cstv.xyz" + (f" for {fmt_left(left)}" if left < 10 * 365 * 86400 else ""))
    if lb.slow and not lb.perms_sid(c.steamid) & ADMIN:
        left = lb.last_chat.get(c.steamid, 0) + lb.slow - now
        if left > 0:
            raise Denied(f"slow mode: wait {math.ceil(left)} s")
    c.chats.append(now)
    if len(c.chats) >= 5 and now - c.chats[0] < 5:
        raise Denied("slow down")
    lb.last_chat[c.steamid] = now


# ---------------------------------------------------------------- every lobby (the website's home page, the game's Lobby tab)
def lobby_people(lb):
    """Who is in a loaded lobby right now (silent site admins left out): names of the signed-in, guest count."""
    names, guests = {}, 0
    for c in lb.conns if lb else ():
        if c.silent:
            continue
        if c.steamid:
            names.setdefault(c.steamid, c.name)
        else:
            guests += 1
    return list(names.values()), guests


def lobby_list(see_all):
    """Every lobby: most people first, then the oldest. What plays and who's there only for lobbies without a
    password (and for site admins)."""
    out = []
    for r in db.q("SELECT code, name, owner, password, created FROM lobbies"):
        lb = lobbies.get(r["code"])
        names, guests = lobby_people(lb)
        locked = bool(lb.password if lb else r["password"])
        it = {"code": r["code"], "name": lb.name if lb else r["name"], "locked": locked, "people": len(names) + guests,
              "created": r["created"] or 0}
        if see_all or not locked:
            it["names"], it["guests"] = names[:20], guests
            if lb and lb.url:
                kind = link_kind(lb.url)
                it["playing"] = {"title": lb.title or short_url(lb.url), "kind": kind,
                                 "url": None if kind == "stream" else lb.url, "paused": lb.paused}
        if see_all:
            it["owner"] = user_name(r["owner"])
        out.append(it)
    out.sort(key=lambda x: (-x["people"], x["created"]))
    return out


def lobby_page(see_all, page, size):
    items = lobby_list(see_all)
    pages = max(1, -(-len(items) // size))
    page = min(max(int(page or 1), 1), pages)
    return items[(page - 1) * size: page * size], page, pages, len(items)


def game_lobbies(c, a):
    """lobbies <page> -> one line: lobbies <page> <pages> <total> then code,locked,people,name,playing per lobby."""
    items, page, pages, total = lobby_page(is_site_admin(c.steamid), a[0] if a else 1, LOBBY_PAGE_GAME)
    parts = [f"lobbies {page} {pages} {total}"]
    for it in items:
        playing = (it.get("playing") or {}).get("title") or ""
        parts.append(f"{it['code']},{int(it['locked'])},{it['people']},{b64o(it['name'])},{b64o(playing[:80])}")
    c.put(" ".join(parts))


def delete_lobby(code, why):
    lbd = lobbies.pop(code, None)
    if lbd:
        for x in list(lbd.conns):
            if x.kind == "game":
                x.put("kicked " + b64e(why))
                go_home(x)
            else:
                x.put({"t": "kicked", "msg": why})
                leave_room(x)
    for tbl in ("lobbies", "members", "bans"):
        db.x(f"DELETE FROM {tbl} WHERE code=?", code)


def site_targets(m):
    """The connections a site action is about: a SteamID's (all of them) or one connection's."""
    sid = str(m.get("steamid") or "")
    if SID_RE.match(sid):
        return sid, [x for x in conns if x.steamid == sid]
    x = next((x for x in conns if x.id == int(m.get("conn") or 0)), None)
    return (x.steamid if x else None), ([x] if x else [])


def warn(targets, text):
    for x in targets:
        x.put("warn " + b64e(text) if x.kind == "game" else {"t": "warn", "msg": text})


def site_overview(c):
    def where(x):
        if isinstance(x.room, Lobby):
            return f"lobby {x.room.code}" + (" (silent)" if x.silent else "")
        if x.room:
            return f"server {x.room.key}"
        return ""
    people = [{"conn": x.id, "kind": "game" if x.kind == "game" else "web", "steamid": x.steamid, "name": x.name,
               "ip": x.ip, "where": where(x), "code": x.room.code if isinstance(x.room, Lobby) else None,
               "admin": is_site_admin(x.steamid)} for x in conns if (x.kind == "game" and x.hello) or x.kind == "web"]
    people.sort(key=lambda p: (p["code"] is None, p["name"].lower()))
    bans = [{"steamid": r["steamid"], "name": user_name(r["steamid"]), "reason": r["reason"], "at": r["at"]}
            for r in db.q("SELECT * FROM global_bans ORDER BY at DESC")]
    ips = [{"ip": r["ip"], "steamid": r["steamid"], "name": user_name(r["steamid"]) if r["steamid"] else None,
            "reason": r["reason"], "at": r["at"]} for r in db.q("SELECT * FROM ip_bans ORDER BY at DESC")]
    mutes = [{"steamid": r["steamid"], "name": user_name(r["steamid"]), "until": r["until"]}
             for r in db.q("SELECT * FROM global_mutes WHERE until>? ORDER BY until", time.time())]
    c.put({"t": "site_overview", "lobbies": lobby_list(True), "people": people, "bans": bans, "ip_bans": ips,
           "mutes": mutes, "stats": public_stats()})


# ---- the visitor log: a row per website page / game connection (VISITS_KEEP long)
def visit_start(c, ua=None):
    cur = db.c.execute("INSERT INTO visits(at, ip, steamid, name, kind, ua) VALUES(?,?,?,?,?,?)",
                       (time.time(), c.ip, c.steamid or "", c.name, "game" if c.kind == "game" or c.legacy else "web",
                        (ua or "")[:300] or None))
    c.visit = cur.lastrowid


def visit_who(c):   # a game signed in after connecting
    if c.visit:
        db.x("UPDATE visits SET steamid=?, name=? WHERE id=?", c.steamid or "", c.name, c.visit)


def visit_end(c):
    if c.visit:
        db.x("UPDATE visits SET until=? WHERE id=?", time.time(), c.visit)
        c.visit = None


def site_visits(c, m):
    """One day of the visitor log, the day in the admin's own time zone: a row per address + account + website/game
    (first connection, last seen, how many connections, online now), and every day there is with its number of
    addresses."""
    try:
        tz = ZoneInfo(str(m.get("tz") or "UTC"))
    except Exception:
        tz = dt.timezone.utc
    now = time.time()
    days = {}
    for r in db.q("SELECT at, ip FROM visits WHERE at>?", now - VISITS_KEEP):
        days.setdefault(dt.datetime.fromtimestamp(r["at"], tz).date(), set()).add(r["ip"])
    today = dt.datetime.fromtimestamp(now, tz).date()
    try:
        day = dt.date.fromisoformat(str(m.get("day") or ""))
    except ValueError:
        day = today
    start = dt.datetime.combine(day, dt.time(), tz).timestamp()
    end = dt.datetime.combine(day + dt.timedelta(days=1), dt.time(), tz).timestamp()
    rows = {}
    for r in db.q("SELECT * FROM visits WHERE at>=? AND at<? ORDER BY at", start, end):
        k = (r["ip"], r["steamid"], r["kind"])
        it = rows.get(k)
        if not it:
            it = rows[k] = {"ip": r["ip"], "steamid": r["steamid"] or None, "kind": r["kind"], "first": r["at"],
                            "last": r["at"], "n": 0, "online": False}
        it["n"] += 1
        it["last"] = max(it["last"], r["until"] or now)
        it["online"] = it["online"] or r["until"] is None
        it["name"] = r["name"] or it.get("name")
        it["ua"] = r["ua"] or it.get("ua")
    days.setdefault(today, set())
    c.put({"t": "site_visits", "day": day.isoformat(), "today": today.isoformat(), "items": list(rows.values()),
           "days": [{"day": d.isoformat(), "n": len(ips)} for d, ips in sorted(days.items(), reverse=True)]})


def site_action(c, m):
    """The admin page (cstv.xyz admins only)."""
    if c.legacy or not is_site_admin(c.steamid):
        raise Denied("that's for cstv.xyz admins")
    act = m.get("action")
    if act == "overview":
        site_overview(c)
        return
    if act == "visits":
        site_visits(c, m)
        return
    if act == "delete_lobby":
        code = str(m.get("code") or "").upper()
        if not db.one("SELECT 1 FROM lobbies WHERE code=?", code):
            raise Denied("no lobby with that code")
        delete_lobby(code, "A cstv.xyz admin deleted the lobby.")
        log.info("site admin %s deleted lobby %s", c.steamid, code)
    elif act in ("warn", "ban", "ipban", "kick", "mute"):
        sid, targets = site_targets(m)
        if sid and is_site_admin(sid):
            raise Denied("that's a cstv.xyz admin")
        if not sid and not targets:
            raise Denied("they're not connected any more")
        if act == "warn":
            text = str(m.get("msg") or "").strip()[:WARN_MAX]
            if not text:
                raise Denied("write the warning first")
            warn(targets, text)
            if not targets:
                raise Denied("they're not connected right now")
        elif act == "kick":   # out of the lobby they're in
            for x in targets:
                if isinstance(x.room, Lobby):
                    kick(x.room, x, False, "a cstv.xyz admin")
        elif act == "mute":   # site-wide chat mute; minutes 0 lifts it, -1 = for good
            if not sid:
                raise Denied("only signed-in players can be muted")
            mins = int(m.get("minutes", 60))
            if mins == 0:
                db.x("DELETE FROM global_mutes WHERE steamid=?", sid)
            else:
                until = time.time() + (100 * 365 * 86400 if mins < 0 else min(mins, 525600) * 60)
                db.x("INSERT OR REPLACE INTO global_mutes VALUES(?,?)", sid, until)
                for lb in lobbies.values():
                    purge(lb, lambda x: x.get("steamid") == sid)
        else:   # ban: the Steam account (signed in or not, wherever it connects from); ipban: the addresses it's on now
            reason = clean_text(m.get("reason") or "", 200)
            if act == "ban":
                if not sid:
                    raise Denied("a guest has no Steam account to ban: use an IP ban")
                db.x("INSERT OR REPLACE INTO global_bans VALUES(?,?,?)", sid, reason, time.time())
                db.x("DELETE FROM sessions WHERE steamid=?", sid)
                gone = [x for x in conns if x.steamid == sid or (x.kind == "game" and x.claimed == sid)]
            else:
                ips = {x.ip for x in targets} - {x.ip for x in conns if is_site_admin(x.steamid)}
                if not ips:
                    raise Denied("that's your own address too")
                for ip in ips:
                    db.x("INSERT OR REPLACE INTO ip_bans VALUES(?,?,?,?)", ip, sid, reason, time.time())
                gone = [x for x in conns if x.ip in ips and not is_site_admin(x.steamid)]
            for x in gone:
                if x.kind == "game":
                    x.tell("You're banned from CS:TV.")
                else:
                    x.put({"t": "error", "code": "sitebanned", "msg": "you're banned from cstv.xyz"})
                x.close(4003, "banned")
            if sid:
                for lb in lobbies.values():
                    purge(lb, lambda x: x.get("steamid") == sid)
            log.info("site admin %s: %s %s %s", c.steamid, act, sid or "-", sorted({x.ip for x in gone}))
    elif act == "unban":
        if m.get("ip"):
            db.x("DELETE FROM ip_bans WHERE ip=?", str(m["ip"]))
        else:
            sid = str(m.get("steamid") or "")
            db.x("DELETE FROM global_bans WHERE steamid=?", sid)
            db.x("DELETE FROM ip_bans WHERE steamid=?", sid)
    else:
        raise Denied("unknown admin action")
    site_overview(c)


# ---------------------------------------------------------------- the game's lines (/game)
def game_where(c, a):
    server, mp = a[0], a[1]
    if (server != "-" and not ROOM_RE.match(server)) or not MAP_RE.match(mp):
        return
    c.server, c.map = server, mp
    r = c.room
    if r and (r.kind == "lobby" or r.key == server):
        r.send_roster()
        r.send_everything(c)
    else:
        go_home(c)


def game_hello(c, a):
    c.put(f"welcome {PROTO} {c.id}")
    if int(a[0]) != PROTO:
        c.tell("Your CS:TV is too old or too new for this server: update it.")
        c.close(4000, "protocol")
        return
    c.claimed = a[1] if a[1].isdigit() and len(a[1]) <= 20 else "0"
    c.ingame_name = clean_text(b64d(a[2]), 32) or None
    if c.claimed != "0" and globally_banned(c.claimed):
        c.close(4003, "banned")
        return
    sid = session_user(c.token) if c.token else None
    if c.token and not sid:
        c.put("token -")   # signed out on the website (or expired): the link forgets it
    elif sid and c.claimed not in ("0", sid):
        c.tell(f"This game is signed in to CS:TV as {user_name(sid)}, but you're playing on another Steam account. "
               f"Type !tvlogout, then !tvlogin with a code for this account.")
        sid = None
    if sid and globally_banned(sid):
        c.close(4003, "banned")
        return
    c.steamid = sid
    c.session = token_hash(c.token) if sid else None
    c.hello = True
    if not c.visit:
        visit_start(c)
    c.put(f"streamhost {STREAM_HOST or '-'}")
    c.put(f"you {sid or '-'} {b64o(c.name) if sid else '-'}")


def game_identity_changed(c):
    visit_who(c)
    c.put(f"you {c.steamid or '-'} {b64o(c.name) if c.steamid else '-'}")
    r = c.room
    if r:
        c.put(r.room_line(c))
        if isinstance(r, Lobby):
            r.remember(c.steamid)
        r.send_roster()


def game_login(c, a):
    code = a[0].upper() if a else ""
    sid, exp = pair_codes.get(code, (None, 0))
    if not sid or exp < time.time():
        raise Denied(f"that code isn't valid (codes last 10 minutes: get a new one at {SITE}/link)")
    if c.claimed not in ("0", sid):
        raise Denied(f"that code is for {user_name(sid)}'s Steam account, but you're playing on another one")
    if globally_banned(sid):
        raise Denied("that account is banned from CS:TV")
    pair_codes.pop(code, None)
    if c.session:
        db.x("DELETE FROM sessions WHERE hash=?", c.session)
    label = clean_text(f"{c.ingame_name or 'CS:S'} on {clean_text(a[1] if len(a) > 1 else '?', 10)}", 60)
    tok = new_session(sid, "game", label)
    c.steamid, c.session = sid, token_hash(tok)
    c.put("token " + tok)   # cstv_link.py keeps it; it never reaches the game
    c.tell(f"Signed in as {c.name}. This game stays signed in until you type !tvlogout (or sign it out on {SITE}/link).")
    for x in conns:   # the website that made the code
        if x.kind == "web" and x.steamid == sid and not x.legacy:
            x.put({"t": "game_linked", "label": label})
    game_identity_changed(c)


def game_logout(c):
    if c.session:
        db.x("DELETE FROM sessions WHERE hash=?", c.session)
    was = c.steamid
    c.steamid = c.session = None
    c.put("token -")
    c.tell("Signed out of CS:TV." if was else "This game wasn't signed in.")
    game_identity_changed(c)


def game_status(c):
    c.tell(f"Signed in as {c.name}." if c.steamid else f"Not signed in (get a code at {SITE}/link, then !tvlogin <code>).")
    r = c.room
    if not r:
        c.tell("Not in a room: join a server, or a lobby with !lobby <code>.")
    elif r.kind == "lobby":
        c.tell(f"In lobby {r.code} \"{r.name}\" ({r.viewers()} watching){' with a password' if r.password else ''}.")
    else:
        others = sum(1 for x in r.conns if x.kind == "game") - 1
        c.tell(f"In this server's room ({others} other player{'s' if others != 1 else ''} with CS:TV). !lobby <code> joins a lobby.")
    if r and r.url:
        c.tell(f"Playing: {r.title or short_url(r.url)} at {clock_label(r.pos() / 1000)}{' (paused)' if r.paused else ''}.")


def game_lobby(c, a):
    sub, r = a[0], c.room
    if sub == "create":
        lb = new_lobby(c, b64d(a[1]) if len(a) > 1 else "", b64d(a[2]) if len(a) > 2 else "")
        enter(c, lb)
        c.tell(f"Lobby \"{lb.name}\" created, code {lb.code}{' (password protected)' if lb.password else ''}. "
               f"Others join with !lobby {lb.code} or at {SITE}/l/{lb.code}")
        return
    if sub == "join":
        code = a[1].upper()
        lb = load_lobby(code)
        if not lb:
            c.tell(JOIN_FAIL["nolobby"])
            c.put(f"joinfail {code} nolobby")
            return
        if lb is r:
            return
        why = lb.admit(c, b64d(a[2]) if len(a) > 2 else "")
        if why:
            c.tell(JOIN_FAIL[why])
            c.put(f"joinfail {lb.code} {why}")
            return
        enter(c, lb)   # (says "X joined")
        if not c.steamid:
            c.tell(f"You're watching as a guest. To control the TV or chat, sign in: get a code at {SITE}/link, then !tvlogin <code>.")
        return
    if not r or r.kind != "lobby":
        raise Denied("you're not in a lobby")
    if sub == "leave":
        c.leaving = True
        go_home(c)   # (says "X left")
        c.leaving = False
        return
    me = r.perms_of(c)
    if not me & ADMIN:
        raise Denied("only the lobby's owner and admins can do that")
    if sub in ("kick", "ban"):
        t = next((x for x in r.conns if x.id == int(a[1])), None)
        if not t or t is c:
            return
        admin_kick(r, c, sid=t.steamid, conn=None if t.steamid else t, ban=sub == "ban")
    elif sub == "unban":
        if a[1] in r.bans:
            r.bans.discard(a[1])
            db.x("DELETE FROM bans WHERE code=? AND steamid=?", r.code, a[1])
            c.tell("Unbanned.")
            r.send_roster()
        else:
            c.tell("That SteamID isn't banned here.")
    elif sub == "owner":
        if not me & OWNER:
            raise Denied("only the owner can hand the lobby over")
        t = next((x for x in r.conns if x.id == int(a[1])), None)
        if not t or t is c:
            return
        if not t.steamid:
            raise Denied("they have to sign in first (!tvlogin)")
        if owned_lobby(t.steamid):
            raise Denied(f"{t.name} already has a lobby (one each)")
        old = r.owner
        r.owner = t.steamid
        r.remember(old)
        r.members[old]["admin"] = True   # the old owner stays an admin
        db.x("UPDATE members SET admin=1 WHERE code=? AND steamid=?", r.code, old)
        save_lobby(r)
        r.send_rooms()
        r.send_roster()
        r.say(f"{t.name} owns the lobby now.")
    elif sub == "name":
        label = clean_text(b64d(a[1]), LABEL_MAX)
        if label:
            r.name = label
            r.send_rooms()
            r.say(f"Lobby renamed to \"{label}\".")
    elif sub == "password":
        r.password = b64d(a[1])[:PASS_MAX] if len(a) > 1 else ""
        r.pass_fails.clear()
        r.send_rooms()
        r.say("The lobby has a password now." if r.password else "The lobby's password was removed.")
    elif sub == "set" and len(a) >= 3:   # the lobby Settings tab: set <sites|slow|gifs|emotes|perms> <value>
        key, val = a[1], a[2]
        if key == "sites" and val in ("any", "yt_twitch"):
            r.sites = val
        elif key == "slow":
            slow = int(val)
            if slow not in SLOW_CHOICES:
                raise Denied("pick a slow mode from the list")
            if slow != r.slow:
                r.slow = slow
                r.system(f"Slow mode: one message every {slow} s" if slow else "Slow mode is off")
        elif key == "gifs" and val in ("0", "1"):
            r.gifs = val == "1"
        elif key == "emotes":
            ch = b64d(val).strip().lower().lstrip("@")
            if ch and not re.fullmatch(r"[a-z0-9_]{1,25}", ch):
                raise Denied("that's not a Twitch channel name")
            r.emotes = ch or None
            r.to_games("lchat_emotes " + (r.emote_channel() or "-"))
        elif key == "perms":
            r.default_perms = int(val) & PERM_ALL
        else:
            return
        r.send_rooms()
    elif sub == "delete":
        if not me & OWNER:
            raise Denied("only the owner can delete the lobby")
        delete_lobby(r.code, "The owner deleted the lobby.")
    elif sub == "perm" and a[1] in ("all", "owner"):
        r.default_perms = PERM_ALL if a[1] == "all" else 0
        r.send_rooms()
        r.say("Everyone can control the TV now." if a[1] == "all" else "Only the owner and admins can control the TV now.")


def game_chat(c, a):
    lb = c.room
    if not isinstance(lb, Lobby):
        raise Denied("lobby chat only works in a lobby")
    text = clean_text(b64d(a[0]), 300)
    if not text:
        return
    chat_allowed(lb, c, time.time())
    lb.post_chat(c, text)


def game_gifs(c, a):
    """gifs <query b64|-> <offset>: a page of GIF search results for the Lobby page's picker."""
    lb = c.room
    if not isinstance(lb, Lobby) or not lb.gifs:
        raise Denied("GIFs are off in this lobby")
    if not c.steamid:
        raise Denied(lb.refusal(c, "post GIFs"))
    if not giphy_key():
        raise Denied("GIFs aren't set up on this hub yet")
    try:
        offset = min(max(int(a[1]) if len(a) > 1 else 0, 0), GIF_MAX_OFFSET)
    except ValueError:
        offset = 0
    asyncio.ensure_future(gif_search(c, clean_text(b64d(a[0]) if a else "", 50), offset))


def game_chatgif(c, a):
    lb = c.room
    if not isinstance(lb, Lobby):
        raise Denied("lobby chat only works in a lobby")
    if not lb.gifs:
        raise Denied("GIFs are off in this lobby")
    gif = c.gifs.get(a[0] if a else "")
    if not gif:
        raise Denied("pick the GIF from the search again")
    chat_allowed(lb, c, time.time())
    lb.post_chat(c, "", gif)


def game_line(c, w):
    verb, a = w[0], w[1:]
    if verb == "ping":
        c.put("pong " + (a[0] if a else "0"))
        return
    if verb == "auth":   # from cstv_link.py, before hello: the token saved by an earlier !tvlogin
        if not c.hello and a and a[0] != "-" and len(a[0]) <= 100:
            c.token = a[0]
        return
    if verb == "hello":
        if not c.hello:
            game_hello(c, a)
        return
    if not c.hello:
        return
    if verb == "where":
        game_where(c, a)
    elif verb == "login":
        if c.spend():
            game_login(c, a)
    elif verb == "logout":
        game_logout(c)
    elif verb == "status":
        game_status(c)
    elif verb == "lobby":
        if c.spend():
            game_lobby(c, a)
    elif verb == "chat":
        game_chat(c, a)
    elif verb == "lobbies":
        game_lobbies(c, a)
    elif verb == "gifs":
        game_gifs(c, a)
    elif verb == "chatgif":
        game_chatgif(c, a)
    elif c.room:
        r = c.room
        if verb == "started":
            r.on_started(int(a[0]))
        elif verb == "ended":
            r.on_ended(int(a[0]), b64d(a[1]) if len(a) > 1 else "", b64d(a[2]) if len(a) > 2 else "")
        elif verb == "title":
            r.on_title(int(a[0]), b64d(a[1]))
        elif verb in GAME_CMDS and c.spend():
            GAME_CMDS[verb](r, c, a)


GAME_CMDS = {
    "place": lambda r, c, a: r.cmd_place(c, a),
    "remove": lambda r, c, a: r.cmd_remove(c),
    "link": lambda r, c, a: r.cmd_link(c, b64d(a[0])),
    "queue": lambda r, c, a: r.cmd_queue(c, b64d(a[0])),
    "unqueue": lambda r, c, a: r.cmd_unqueue(c, int(a[0])),
    "playnow": lambda r, c, a: r.cmd_playnow(c, int(a[0])),
    "clearqueue": lambda r, c, a: r.cmd_clearqueue(c),
    "next": lambda r, c, a: r.cmd_next(c),
    "prev": lambda r, c, a: r.cmd_prev(c),
    "pause": lambda r, c, a: r.cmd_pause(c),
    "seek": lambda r, c, a: r.cmd_seek(c, a[0]),
    "speed": lambda r, c, a: r.cmd_speed(c, int(a[0])),
    "loop": lambda r, c, a: r.cmd_loop(c, a[0] == "1"),
    "stream": lambda r, c, a: r.cmd_stream(c, a[0]),
}


# ---------------------------------------------------------------- the website's messages (/ws)
def web_join(c, code, password="", silent=False):
    lb = load_lobby(code)
    if not lb:
        c.put({"t": "error", "msg": "no lobby with that code", "code": "nolobby"})
        return
    silent = bool(silent) and is_site_admin(c.steamid)
    if c.room is lb:
        if c.silent == silent:
            c.put(lb.snapshot(c))
            return
        leave_room(c)   # silent <-> seen: in again as the other
    why = lb.admit(c, str(password or ""))
    if why:
        c.put({"t": "error", "msg": JOIN_FAIL[why], "code": why, "lobby": lb.code, "name": lb.name})
        return
    leave_room(c)
    c.silent = silent
    enter(c, lb)


def need_lobby(c):
    if not isinstance(c.room, Lobby):
        raise Denied("join a lobby first")
    return c.room


def web_message(c, m):
    t = m.get("t")
    if t == "time":
        c.put({"t": "time", "c": m.get("c"), "h": now_ms()})
        return
    # rate limit everything else: 20 messages per 5 s
    now = time.time()
    c.cmds.append(now)
    if len(c.cmds) >= 20 and now - c.cmds[-20] < 5:
        raise Denied("slow down")
    lb = c.room

    if t == "create":
        own = owned_lobby(c.steamid)
        if own:
            c.put({"t": "error", "code": "haslobby", "lobby": own, "msg": "You already have a lobby"})
            return
        lb = new_lobby(c, clean_text(m.get("name") or "", LABEL_MAX), str(m.get("password") or ""))
        c.put({"t": "created", "code": lb.code})
        enter(c, lb)
    elif t == "join":
        web_join(c, m.get("code"), m.get("password"), m.get("silent"))
    elif t == "leave":
        leave_room(c)
        c.silent = False
    elif t == "lobbies":   # the home page: every lobby, a page at a time
        items, page, pages, total = lobby_page(is_site_admin(c.steamid), m.get("page"), LOBBY_PAGE_WEB)
        c.put({"t": "lobbies", "items": items, "page": page, "pages": pages, "total": total})
    elif t == "site":
        site_action(c, m)
    elif t == "mylobbies":
        if not c.steamid:
            c.put({"t": "mylobbies", "owned": [], "recent": []})
            return
        owned = [dict(code=r["code"], name=r["name"], locked=bool(r["password"])) for r in
                 db.q("SELECT code, name, password FROM lobbies WHERE owner=? ORDER BY last DESC", c.steamid)]
        recent = [dict(code=r["code"], name=r["name"], locked=bool(r["password"])) for r in
                  db.q("SELECT l.code, l.name, l.password FROM members m JOIN lobbies l ON l.code=m.code "
                       "WHERE m.steamid=? AND l.owner<>? ORDER BY l.last DESC LIMIT 10", c.steamid, c.steamid)]
        c.put({"t": "mylobbies", "owned": owned, "recent": recent})
    elif t == "delete":
        code = str(m.get("code", ""))
        r = db.one("SELECT owner FROM lobbies WHERE code=?", code)
        if not r or (r["owner"] != c.steamid and not is_site_admin(c.steamid)):
            raise Denied("only the owner can delete a lobby")
        delete_lobby(code, "The owner deleted the lobby." if r["owner"] == c.steamid else "A cstv.xyz admin deleted the lobby.")
        c.put({"t": "deleted", "code": code})
    elif t == "watching":
        if lb:
            on = bool(m.get("on"))
            if on != c.watching:
                c.watching = on
                lb.update_tol()
                lb.send_roster()
    elif t == "link":
        need_lobby(c).cmd_link(c, str(m.get("url", "")))
    elif t == "queue":
        need_lobby(c).cmd_queue(c, str(m.get("url", "")))
    elif t == "queue_action":
        lb = need_lobby(c)
        uid, act = m.get("uid"), m.get("action")
        if act == "clear":
            lb.cmd_clearqueue(c)
        elif act == "remove":
            lb.cmd_unqueue(c, uid)
        elif act == "next":
            lb.cmd_tofront(c, uid)
        elif act == "play":
            lb.cmd_playnow(c, uid)
    elif t == "pause":
        need_lobby(c).cmd_pause(c, m.get("paused"))
    elif t == "seek":
        lb = need_lobby(c)
        lb.cmd_seek(c, ("rel", int(m["rel"])) if "rel" in m else int(m.get("ms", 0)))
    elif t == "speed":
        need_lobby(c).cmd_speed(c, max(25, min(400, int(m.get("pct", 100)))))
    elif t == "loop":
        need_lobby(c).cmd_loop(c, m.get("on"))
    elif t == "skip":
        need_lobby(c).cmd_next(c)
    elif t == "prev":
        need_lobby(c).cmd_prev(c)
    elif t == "stop":
        need_lobby(c).cmd_stop(c)
    elif t == "started":
        if lb:
            lb.on_started(m.get("rev"), m.get("seek"))
    elif t == "ended":
        if lb:
            lb.on_ended(m.get("rev"))
    elif t == "duration":   # a viewer's player knows the length: the hub moves on at the end even if nobody watches
        if lb and m.get("rev") == lb.rev and lb.url and not lb.dur:
            ms = int(m.get("ms", 0))
            if 1000 <= ms <= 48 * 3600 * 1000:
                lb.dur = ms
    elif t == "chat":
        lb = need_lobby(c)
        text = clean_text(m.get("text", ""), 300)
        gif = None
        if m.get("gif"):
            if not lb.gifs:
                raise Denied("GIFs are off in this lobby")
            gif = c.gifs.get(str(m["gif"]))
            if not gif:
                raise Denied("pick the GIF from the search again")
        if not text and not gif:
            return
        chat_allowed(lb, c, now)
        lb.post_chat(c, text, gif)
    elif t == "gif_search":
        if not isinstance(lb, Lobby) or not lb.gifs:
            raise Denied("GIFs are off in this lobby")
        if not c.steamid:
            raise Denied("sign in with Steam to post GIFs")
        if not giphy_key():
            raise Denied("GIFs aren't set up on this hub yet")
        try:
            offset = min(max(int(m.get("offset") or 0), 0), GIF_MAX_OFFSET)
        except (TypeError, ValueError):
            offset = 0
        asyncio.ensure_future(gif_search(c, clean_text(m.get("q", ""), 50), offset))
    elif t == "admin":
        web_admin(c, m)
    elif t == "hls":   # watch the lobby's live stream over HLS (browsers whose WebRTC can't show it)
        hls_issue(c, need_lobby(c))
    elif t == "vstats":   # the stream player's numbers
        vstats(c, m)
    elif t == "xchat":   # the stream chat panel opened / closed (YouTube's chat comes through the hub)
        yt_panel(c, bool(m.get("on")))
    elif t == "whep":   # watch the lobby's live stream (WebRTC)
        offer = str(m.get("offer") or "")
        if not offer.startswith("v=0") or len(offer) > 30000:
            raise Denied("bad stream request")
        asyncio.ensure_future(whep(c, need_lobby(c), offer))
    elif t == "pair_code":   # website "Connect game": a code to type in game (!tvlogin <code>), 10 min
        if not c.steamid or c.legacy:
            raise Denied("sign in with Steam first")
        for code in [k for k, v in pair_codes.items() if v[0] == c.steamid]:
            pair_codes.pop(code)
        code = new_code()
        while code in pair_codes:
            code = new_code()
        pair_codes[code] = (c.steamid, time.time() + 600)
        c.put({"t": "pair_code", "code": code, "expires_in": 600})
    elif t == "pair":   # an older game build redeems a code
        if not c.legacy:
            raise Denied("only a game can do that")
        code = str(m.get("code", "")).upper().strip()
        sid, exp = pair_codes.pop(code, (None, 0))
        if not sid or exp < time.time():
            raise Denied("that code isn't valid (it lasts 10 minutes; get a new one on the website)")
        label = clean_text(f"{c.ingame_name or 'CS:S'} on {clean_text(m.get('os') or '?', 10)}", 60)
        tok = new_session(sid, "game", label)
        c.steamid, c.session = sid, token_hash(tok)
        c.put({"t": "token", "token": tok, "steamid": sid, "name": c.name})
        for x in conns:
            if x.kind == "web" and x.steamid == sid and not x.legacy:
                x.put({"t": "game_linked", "label": label})
        if isinstance(lb, Lobby):
            c.put(lb.you_msg(c))
            lb.send_roster()
    elif t == "logout":   # an older game build forgets its sign-in
        if c.legacy and c.session:
            db.x("DELETE FROM sessions WHERE hash=?", c.session)
            c.steamid = c.session = None
            c.put({"t": "you", "steamid": None, "name": None, "perms": 0})
    elif t == "games":   # website: my connected games
        if not c.steamid:
            raise Denied("sign in with Steam first")
        rows = db.q("SELECT hash, label, created, last FROM sessions WHERE steamid=? AND kind='game' ORDER BY last DESC", c.steamid)
        c.put({"t": "games", "items": [{"id": r["hash"][:16], "label": r["label"] or "a game",
                                         "created": r["created"], "last": r["last"]} for r in rows]})
    elif t == "revoke":   # website: sign a game out
        if not c.steamid:
            raise Denied("sign in with Steam first")
        gid = str(m.get("id", ""))
        if not re.fullmatch(r"[0-9a-f]{16}", gid):
            raise Denied("unknown game")
        for r in db.q("SELECT hash FROM sessions WHERE steamid=? AND kind='game' AND hash LIKE ?", c.steamid, gid + "%"):
            db.x("DELETE FROM sessions WHERE hash=?", r["hash"])
            for x in [x for x in conns if x.session == r["hash"]]:
                x.steamid = x.session = None
                if x.kind == "game":
                    x.put("token -")
                    x.tell("This game was signed out of CS:TV on the website.")
                    game_identity_changed(x)
                else:
                    x.put({"t": "signed_out", "msg": "this game was signed out on the website"})
                    if isinstance(x.room, Lobby):
                        x.put(x.room.you_msg(x))
        web_message(c, {"t": "games"})
    else:
        raise Denied(f"unknown message {t!r}")


async def whep(c, lb, offer):
    """The website's stream player: its WebRTC offer to mediamtx (WHEP), the answer back over the WebSocket. The
    video then goes from mediamtx straight to the browser (8189); mediamtx asks stream_auth about the read."""
    s = lb.stream
    if not s or not s["live"] or lb.url != s["url"]:
        c.put({"t": "whep_fail", "msg": "the stream isn't live"})
        return
    try:
        answer = await asyncio.to_thread(http_post_sdp, f"{WHEP_URL}/live/{s['id']}/whep", offer)
    except Exception as e:
        log.info("whep %s: %s", s["id"], e)
        c.put({"t": "whep_fail", "msg": "the stream can't be reached right now"})
        return
    c.put({"t": "whep", "id": s["id"], "answer": answer})


# ---------------------------------------------------------------- streams over HLS (the WebRTC fallback)
hls_tokens = {}   # token -> (conn, stream id, expires): only that viewer, only while that stream is live in its lobby
HLS_FILE_RE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,79}")
# what may pass: LL-HLS's blocking reloads, and mediamtx's reader session (it puts ?session=<id> in every link of the
# main playlist when that is asked for with ?cookieCheck=1 and no cookie: no cookies to keep per viewer)
HLS_QUERY = {"_HLS_msn": r"\d{1,10}", "_HLS_part": r"\d{1,5}", "_HLS_skip": r"YES|v2", "session": r"[0-9a-f-]{36}"}


def hls_issue(c, lb):
    s = lb.stream
    if not s or not s["live"] or lb.url != s["url"]:
        c.put({"t": "hls_fail", "msg": "the stream isn't live"})
        return
    for k in [k for k, v in hls_tokens.items() if v[0] is c]:   # one per viewer
        del hls_tokens[k]
    tok = secrets.token_urlsafe(24)
    hls_tokens[tok] = (c, s["id"], time.time() + 12 * 3600)
    c.put({"t": "hls", "id": s["id"], "url": f"/hls/{tok}/index.m3u8"})


def hls_stream(tok):
    """The stream id a token reads, or None (unknown, expired, its viewer gone or moved, the stream over)."""
    v = hls_tokens.get(tok)
    if not v:
        return None
    c, sid, exp = v
    s = c.room.stream if c.room else None
    if exp < time.time() or c not in conns or not s or s["id"] != sid or not s["live"] or c.room.url != s["url"]:
        hls_tokens.pop(tok, None)
        return None
    return sid


async def local_get(base, target, timeout=9.0, limit=64 << 20):
    """GET base+target (a local HTTP/1.1 server: mediamtx) without a thread: LL-HLS holds playlist requests open
    until the next part exists, and many at once would use up the thread pool. -> (status, content type, body)"""
    u = urllib.parse.urlsplit(base)
    reader, writer = await asyncio.wait_for(asyncio.open_connection(u.hostname, u.port or 80), 3)
    try:
        writer.write(f"GET {target} HTTP/1.1\r\nHost: {u.netloc}\r\nConnection: close\r\n\r\n".encode())
        await writer.drain()
        head = (await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout)).decode("latin-1").split("\r\n")
        status = int(head[0].split()[1])
        hdr = {}
        for line in head[1:]:
            k, _, v = line.partition(":")
            if k:
                hdr[k.strip().lower()] = v.strip()
        if hdr.get("transfer-encoding", "").lower() == "chunked":
            body = bytearray()
            while True:
                size = int((await asyncio.wait_for(reader.readuntil(b"\r\n"), timeout)).split(b";")[0], 16)
                if size == 0 or len(body) + size > limit:
                    break
                body += await asyncio.wait_for(reader.readexactly(size + 2), timeout)
                del body[-2:]
            body = bytes(body)
        elif "content-length" in hdr:
            n = int(hdr["content-length"])
            if n > limit:
                raise ValueError("too big")
            body = await asyncio.wait_for(reader.readexactly(n), timeout)
        else:
            body = await asyncio.wait_for(reader.read(limit), timeout)
        return status, hdr.get("content-type", "application/octet-stream"), body
    finally:
        writer.close()


async def hls_proxy(path, qs):
    parts = path.split("/")   # "", "hls", token, file
    if len(parts) != 4 or not HLS_FILE_RE.fullmatch(parts[3]):
        return respond(404, b"not found")
    sid = hls_stream(parts[2])
    if not sid:
        return respond(403, b"this stream link has ended")
    q = urllib.parse.parse_qs(qs)
    keep = [(k, q[k][0]) for k, rx in HLS_QUERY.items() if k in q and re.fullmatch(rx, q[k][0])]
    if parts[3] == "index.m3u8":
        keep.append(("cookieCheck", "1"))
    target = f"/live/{sid}/{parts[3]}" + ("?" + urllib.parse.urlencode(keep) if keep else "")
    try:
        status, ctype, body = await local_get(HLS_URL, target)
    except (OSError, asyncio.TimeoutError, asyncio.IncompleteReadError, asyncio.LimitOverrunError, ValueError, IndexError) as e:
        log.info("hls %s %s: %s", sid, parts[3], e or type(e).__name__)
        return respond(503, b"the stream can't be reached right now")
    return respond(status if status in (200, 400, 404) else 503, body, ctype, cache="no-store")


def vstats(c, m):
    """The website's stream player, every 10 s: how playback goes (one log line; nothing kept)."""
    if not isinstance(c.room, Lobby) or time.monotonic() - getattr(c, "vstats_at", 0) < 5:
        return
    c.vstats_at = time.monotonic()

    def num(k, nd=0):
        v = m.get(k)
        return round(float(v), nd) if isinstance(v, (int, float)) and math.isfinite(v) else "-"

    word = lambda k, n: re.sub(r"[^\w./ ;:()-]", "", str(m.get(k) or "-"))[:n] or "-"
    log.info("vstats %s %s mode=%s codec=%s ua=[%s] res=%s fps=%s decoded=%s dropped=%s freezes=%s/%ss lost=%s jitter=%sms kbps=%s stalls=%s",
             c.room.code, c.name if c.steamid else f"guest{c.id}", word("mode", 8), word("codec", 20), word("ua", 60), word("res", 12),
             num("fps"), num("fd"), num("fdrop"), num("fz"), num("fzd", 1), num("lost"), num("jit", 1), num("kbps"), num("stalls"))


# ---------------------------------------------------------------- YouTube live chat for the website
# The site's chat panel next to a YouTube live stream, in its own look: YouTube has no open chat feed, so the hub
# reads the chat the way YouTube's own page does (live_chat page -> youtubei get_live_chat, the "Live chat" view
# with every message; as cstv_overlay.py's YouTubeChat for the game) while someone has the panel open, and passes
# it on over /ws. One reader per video, shared by every lobby playing it.
YT_UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36"
YT_HEADERS = {"User-Agent": YT_UA, "Accept-Language": "en-US,en;q=0.9", "Cookie": "SOCS=CAI; CONSENT=YES+"}   # skip the EU consent page
YT_BASE = os.environ.get("CSTV_HUB_YT_URL", "https://www.youtube.com").rstrip("/")
yt_chats = {}   # video id -> YTChat


def yt_video_id(url):
    try:
        u = urllib.parse.urlsplit(url or "")
    except ValueError:
        return None
    host = re.sub(r"^(www|m|music)\.", "", (u.hostname or "").lower())
    vid = ""
    if host == "youtu.be":
        vid = u.path[1:12]
    elif host == "youtube.com":
        if u.path == "/watch":
            vid = urllib.parse.parse_qs(u.query).get("v", [""])[0][:11]
        else:
            m = re.match(r"^/(?:live|shorts|embed)/([A-Za-z0-9_-]{11})", u.path)
            vid = m.group(1) if m else ""
    return vid if re.fullmatch(r"[A-Za-z0-9_-]{11}", vid) else None


def yt_http(path, body=None):
    """GET (or POST JSON) on YouTube; a function of its own so the tests can stand in for YouTube."""
    h = dict(YT_HEADERS, **({"Content-Type": "application/json"} if body is not None else {}))
    req = urllib.request.Request(YT_BASE + path, data=body, headers=h)
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.read(8 << 20).decode("utf-8", "replace")


class YTChat:
    BACKLOG = 150
    POLL = 2.0   # seconds between reads of new messages

    def __init__(self, video):
        self.video = video
        self.state = "checking"   # checking -> ok (it has a chat) | none (no live chat: not live, off, ended)
        self.note = None
        self.checked = 0.0        # time.monotonic() of the last page check
        self.subs = set()         # web conns with the panel open
        self.items = deque(maxlen=self.BACKLOG)
        self.task = None
        self.used = time.monotonic()

    def status_msg(self):
        return {"t": "ytchat_status", "v": self.video, "ok": self.state == "ok", "state": self.state, "note": self.note}

    def watchers(self):
        """Web conns in a lobby playing this video (they're told whether it has a chat)."""
        return [c for lb in lobbies.values() if yt_video_id(lb.url) == self.video for c in lb.conns if c.kind == "web"]

    def announce(self):
        msg = self.status_msg()
        for c in self.watchers():
            c.put(msg)

    def kick(self):
        self.used = time.monotonic()
        if self.task is None or self.task.done():
            self.task = asyncio.ensure_future(self.run())

    async def run(self):
        fails = 0
        while True:
            try:
                await self.session()
                return
            except Exception as e:
                fails += 1
                log.info("youtube chat %s: %s", self.video, e)
                if fails >= 3:
                    self.state, self.note = "none", "YouTube's chat can't be read right now"
                    self.checked = time.monotonic()
                    self.announce()
                    return
                await asyncio.sleep(5)

    async def session(self):
        html = await asyncio.to_thread(yt_http, f"/live_chat?is_popout=1&v={self.video}")
        self.checked = time.monotonic()
        ver = re.search(r'"INNERTUBE_CONTEXT_CLIENT_VERSION":"([^"]+)"', html)
        m = re.search(r'(?:window\["ytInitialData"\]|var ytInitialData)\s*=\s*(\{.+?\});\s*</script>', html, re.S)
        if not ver or not m:
            raise RuntimeError("live chat page without chat data")
        data = json.loads(m.group(1))
        lcr = data.get("contents", {}).get("liveChatRenderer")
        if not lcr:
            runs = data.get("contents", {}).get("messageRenderer", {}).get("text", {}).get("runs", [])
            self.state, self.note = "none", "".join(r.get("text", "") for r in runs) or "No live chat"
            self.announce()
            return
        try:   # "Live chat" (every message) instead of the default "Top chat"
            items = lcr["header"]["liveChatHeaderRenderer"]["viewSelector"]["sortFilterSubMenuRenderer"]["subMenuItems"]
            cont = items[-1]["continuation"]["reloadContinuationData"]["continuation"]
        except (KeyError, IndexError, TypeError):
            self.handle(lcr.get("actions", []))
            cont = self.next_cont(lcr.get("continuations", []))
        if self.state != "ok":
            self.state, self.note = "ok", None
            self.announce()
        ctx = {"client": {"clientName": "WEB", "clientVersion": ver.group(1), "hl": "en", "gl": "US"}}
        while cont:
            if not self.subs:   # nobody reads it: wait a while, then let the next reader start over
                if time.monotonic() - self.used > 90:
                    return
                await asyncio.sleep(1)
                continue
            self.used = time.monotonic()
            body = json.dumps({"context": ctx, "continuation": cont}).encode()
            r = json.loads(await asyncio.to_thread(yt_http, "/youtubei/v1/live_chat/get_live_chat?prettyPrint=false", body))
            lcc = r.get("continuationContents", {}).get("liveChatContinuation")
            if not lcc:
                self.state, self.note = "none", "The stream's chat has ended"
                self.announce()
                return
            self.handle(lcc.get("actions", []))
            cont = self.next_cont(lcc.get("continuations", []))
            await asyncio.sleep(self.POLL)

    @staticmethod
    def next_cont(conts):
        for c in conts:
            for k in ("invalidationContinuationData", "timedContinuationData", "reloadContinuationData"):
                if k in c:
                    return c[k].get("continuation")
        return None

    def send(self, msg):
        for c in list(self.subs):
            c.put(msg)

    def handle(self, actions):
        new, gone = [], []
        for a in actions:
            if "addChatItemAction" in a:
                m = self.message(a["addChatItemAction"].get("item", {}))
                if m:
                    new.append(m)
            elif "removeChatItemAction" in a:
                gone.append(a["removeChatItemAction"].get("targetItemId"))
            elif "markChatItemAsDeletedAction" in a:
                gone.append(a["markChatItemAsDeletedAction"].get("targetItemId"))
            elif "markChatItemsByAuthorAsDeletedAction" in a:
                ch = a["markChatItemsByAuthorAsDeletedAction"].get("externalChannelId")
                if ch:
                    self.items = deque((x for x in self.items if x["ch"] != ch), maxlen=self.BACKLOG)
                    self.send({"t": "ytchat_clear", "v": self.video, "ch": ch})
        gone = [g for g in gone if isinstance(g, str)]
        if gone:
            self.items = deque((x for x in self.items if x["id"] not in gone), maxlen=self.BACKLOG)
            self.send({"t": "ytchat_del", "v": self.video, "ids": gone})
        if new:
            self.items.extend(new)
            self.send({"t": "ytchat", "v": self.video, "items": new})

    @staticmethod
    def message(item):
        paid = item.get("liveChatPaidMessageRenderer")
        r = item.get("liveChatTextMessageRenderer") or paid
        if not r:
            return None   # welcome banners, gifts, membership notices
        kinds = set()
        for b in r.get("authorBadges", []):
            br = b.get("liveChatAuthorBadgeRenderer", {})
            icon = br.get("icon", {}).get("iconType")
            kinds.add(icon if icon else ("MEMBER" if "customThumbnail" in br else ""))
        runs = []
        for run in r.get("message", {}).get("runs", []):
            if "text" in run:
                runs.append([str(run["text"])[:500]])
            elif "emoji" in run:
                e = run["emoji"]
                eid = str(e.get("emojiId", ""))
                thumbs = e.get("image", {}).get("thumbnails", [])
                alt = (e.get("shortcuts") or [eid])[0] if isinstance(e.get("shortcuts"), list) else eid
                if (e.get("isCustomEmoji") or "/" in eid) and thumbs:
                    url = str(max(thumbs, key=lambda t: t.get("width", 0)).get("url", ""))
                    if url.startswith("https://"):
                        runs.append([str(alt)[:60], url])
                else:
                    runs.append([eid[:16]])   # a standard emoji: the character itself
        if not runs and not paid:
            return None
        return {"id": str(r.get("id", ""))[:120], "n": str(r.get("authorName", {}).get("simpleText", "?"))[:60],
                "k": "o" if "OWNER" in kinds else "m" if "MODERATOR" in kinds else "s" if "MEMBER" in kinds else "",
                "p": str(paid.get("purchaseAmountText", {}).get("simpleText", ""))[:20] if paid else "",
                "r": runs, "ch": str(r.get("authorExternalChannelId", ""))[:40]}


def yt_chat(video):
    ch = yt_chats.get(video)
    if ch is None:
        ch = yt_chats[video] = YTChat(video)
    return ch


def yt_announce(lb, c=None):
    """Tell a lobby's browsers (or one) whether its YouTube video has a live chat; look it up when it isn't known."""
    vid = yt_video_id(lb.url)
    if not vid:
        return
    targets = [c] if c else [x for x in lb.conns if x.kind == "web"]
    if not targets:
        return
    ch = yt_chat(vid)
    if ch.state == "checking" or (ch.state == "none" and time.monotonic() - ch.checked > 180):
        ch.kick()   # it announces what it found
    if ch.state != "checking":
        for x in targets:
            x.put(ch.status_msg())


def yt_unsubscribe(c, keep=None):
    for ch in yt_chats.values():
        if ch is not keep:
            ch.subs.discard(c)


def yt_panel(c, on):
    """A browser opened (or closed) the stream chat panel: the lobby's YouTube video's chat, backlog first."""
    vid = yt_video_id(c.room.url) if isinstance(c.room, Lobby) else None
    if not on or not vid:
        yt_unsubscribe(c)
        return
    ch = yt_chat(vid)
    yt_unsubscribe(c, keep=ch)
    if c not in ch.subs:
        ch.subs.add(c)
        c.put({"t": "ytchat", "v": vid, "reset": True, "items": list(ch.items)})
    c.put(ch.status_msg())
    if ch.state != "none" or time.monotonic() - ch.checked > 180:
        ch.kick()


async def gif_search(c, q, offset=0):
    try:
        items, nxt = await giphy(q, giphy_key(), offset)
    except Exception as e:
        log.info("giphy %r: %s", q, e)
        c.deny("GIF search isn't working right now")
        return
    if len(c.gifs) > 2000:
        c.gifs.clear()
    for it in items:
        c.gifs[it["id"]] = {"id": it["id"], "title": it["title"], **it["full"]}
    if c.kind == "game":   # gifs <query> <offset> <next|-> id,w,h,preview...
        c.put(f"gifs {b64o(q)} {offset} {nxt if nxt is not None else '-'}" +
              "".join(f" {it['id']},{it['preview']['w']},{it['preview']['h']},{b64e(it['preview']['url'])}" for it in items))
    else:
        c.put({"t": "gifs", "q": q, "offset": offset, "next": nxt, "items": items})


def web_admin(c, m):
    lb = need_lobby(c)
    me = lb.perms_of(c)
    if not me & ADMIN:
        raise Denied(lb.refusal(c, "change lobby settings"))
    act = m.get("action")
    sid = str(m.get("steamid", ""))
    if act == "default_perms":
        lb.default_perms = int(m.get("perms", 0)) & PERM_ALL
        lb.send_rooms()
    elif act == "settings":
        if "name" in m:
            lb.name = clean_text(m["name"], LABEL_MAX) or lb.name
        if m.get("sites") in ("any", "yt_twitch"):
            lb.sites = m["sites"]
        if "slow" in m:
            slow = int(m["slow"])
            if slow not in SLOW_CHOICES:
                raise Denied("pick a slow mode from the list")
            if slow != lb.slow:
                lb.slow = slow
                lb.system(f"Slow mode: one message every {slow} s" if slow else "Slow mode is off")
        if "gifs" in m:
            lb.gifs = bool(m["gifs"])
        if "emotes" in m:
            ch = str(m["emotes"] or "").strip().lower().lstrip("@")
            if ch and not re.fullmatch(r"[a-z0-9_]{1,25}", ch):
                raise Denied("that's not a Twitch channel name")
            lb.emotes = ch or None
            lb.to_games("lchat_emotes " + (lb.emote_channel() or "-"))
        if "password" in m:
            pw = str(m["password"] or "")[:PASS_MAX]
            if pw != lb.password:
                lb.password = pw
                lb.pass_fails.clear()
                lb.system("The lobby has a password now." if pw else "The lobby's password was removed.")
        lb.send_rooms()
    elif act == "delete_msg":
        mid = int(m.get("id", 0))
        msg = next((x for x in lb.chat if x["id"] == mid), None)
        if msg and msg.get("steamid") == lb.owner and not me & OWNER:
            raise Denied("only the owner can delete the owner's messages")
        purge(lb, lambda x: x["id"] == mid)
    elif act in ("kick", "ban") and "conn" in m:   # a guest (not signed in), by connection id
        t = next((x for x in lb.conns if x.id == int(m["conn"]) and not x.steamid), None)
        if t:
            admin_kick(lb, c, conn=t, ban=act == "ban")
    else:
        if not re.fullmatch(r"\d{17}", sid):
            raise Denied("pick a player")
        if sid == lb.owner:
            raise Denied("the owner can't be changed")
        target = lb.perms_sid(sid)
        if target & SITEADMIN:
            raise Denied("a cstv.xyz admin can't be changed here")
        if target & ADMIN and not me & OWNER:
            raise Denied("only the owner can change another admin")
        if act == "perms":   # perms: number, or null = the lobby default
            p = m.get("perms")
            lb.remember(sid)
            lb.members[sid]["perms"] = None if p is None else int(p) & PERM_ALL
            db.x("UPDATE members SET perms=? WHERE code=? AND steamid=?", lb.members[sid]["perms"], lb.code, sid)
        elif act == "admin":
            if not me & OWNER:
                raise Denied("only the owner makes admins")
            lb.remember(sid)
            lb.members[sid]["admin"] = bool(m.get("on"))
            db.x("UPDATE members SET admin=? WHERE code=? AND steamid=?", int(lb.members[sid]["admin"]), lb.code, sid)
        elif act == "mute":   # a timeout; minutes 0 = lift it
            mins = max(0, min(10080, int(m.get("minutes", 10))))
            if mins:
                lb.mutes[sid] = time.time() + mins * 60
                purge(lb, lambda x: x.get("steamid") == sid)   # like Twitch: their messages go too
                lb.system(f"{user_name(sid)} was timed out for " + (f"{mins // 1440} d" if mins % 1440 == 0 else f"{mins // 60} h" if mins % 60 == 0 else f"{mins} min"))
            elif lb.mutes.pop(sid, None):
                lb.system(f"{user_name(sid)}'s timeout was lifted")
        elif act in ("kick", "ban"):
            admin_kick(lb, c, sid=sid, ban=act == "ban")
            return
        elif act == "unban":
            lb.bans.discard(sid)
            db.x("DELETE FROM bans WHERE code=? AND steamid=?", lb.code, sid)
        else:
            raise Denied("unknown admin action")
        for x in lb.conns:
            if x.steamid == sid:
                x.put(lb.room_line(x) if x.kind == "game" else lb.you_msg(x))
    lb.send_roster()


def public_stats():
    """The home page's numbers: right now, across the whole hub."""
    rooms = list(srv_rooms.values()) + list(lobbies.values())
    return {"watching": sum(r.viewers() for r in rooms),
            "in_game": sum(1 for c in conns if c.kind == "game" and c.hello),
            "lobbies": sum(1 for lb in lobbies.values() if lb.conns),
            "servers": sum(1 for r in srv_rooms.values() if any(c.kind == "game" for c in r.conns))}


# ---------------------------------------------------------------- timers
async def fast_tick():
    """Clocks waiting for "started" (LOAD_TIMEOUT), videos ending on the clock."""
    while True:
        await asyncio.sleep(1)
        for r in list(srv_rooms.values()) + list(lobbies.values()):
            try:
                r.tick()
            except Exception as e:
                log.warning("tick %s: %s", r.key, e)


async def sweep():
    """Every SYNC_EVERY s the games get the clock again (each corrects its drift); rooms nobody uses go."""
    while True:
        await asyncio.sleep(SYNC_EVERY)
        for r in list(srv_rooms.values()) + list(lobbies.values()):
            if r.url and any(c.kind == "game" for c in r.conns):
                r.to_games(r.state_line())
        t = time.time()
        for key, r in list(srv_rooms.items()):
            if not r.conns and t - r.empty_since > SERVER_TTL:
                del srv_rooms[key]   # its screens stay in the database


async def housekeeping():
    last_sweep = 0
    while True:
        await asyncio.sleep(5)
        t = time.time()
        if t - last_sweep >= 60:
            last_sweep = t
            expire_lobbies(t)
            db.x("DELETE FROM visits WHERE at<?", t - VISITS_KEEP)
        for code, lb in list(lobbies.items()):
            if lb.dirty:
                save_lobby(lb)
            if lb.conns:
                lb.empty_since = t
            elif t - lb.empty_since > LOBBY_UNLOAD:
                save_lobby(lb)
                del lobbies[code]
        for code, (sid, exp) in list(pair_codes.items()):
            if exp < t:
                pair_codes.pop(code, None)
        for tok in list(hls_tokens):
            hls_stream(tok)   # drops the dead ones
        mono = time.monotonic()
        for vid, ch in list(yt_chats.items()):   # forget chats nobody has looked at for a while
            if not ch.subs and (ch.task is None or ch.task.done()) and mono - ch.used > 600 and not ch.watchers():
                del yt_chats[vid]


def expire_lobbies(t):
    """Lobbies nobody has been in for LOBBY_EXPIRE close for good (seen = when someone was last in it)."""
    for code, lb in lobbies.items():
        if lb.conns:
            db.x("UPDATE lobbies SET seen=? WHERE code=?", t, code)
    for r in db.q("SELECT code FROM lobbies WHERE seen<?", t - LOBBY_EXPIRE):
        lb = lobbies.get(r["code"])
        if lb and lb.conns:
            continue
        delete_lobby(r["code"], "Nobody was in the lobby for an hour.")
        log.info("lobby %s closed: nobody in it for %d min", r["code"], LOBBY_EXPIRE // 60)


# ---------------------------------------------------------------- mediamtx asks: may this ip publish / read?
def stream_auth(req):
    """Reads of a known stream are open, publishing needs the stream's key, the rest (api, metrics, unknown
    paths) is refused (daf's master)."""
    m = STREAM_PATH_RE.match(str(req.get("path") or ""))
    s = None
    if m:
        s = next((r.stream for r in list(srv_rooms.values()) + list(lobbies.values())
                  if r.stream and r.stream["id"] == m.group(1)), None)
    if not s:
        return False
    action = req.get("action")
    if action in ("read", "playback"):
        return True
    if action == "publish":
        key = urllib.parse.parse_qs(str(req.get("query") or "").lstrip("?")).get("key", [""])[0]
        return hmac.compare_digest(key.encode(), s["secret"].encode())
    return False


def stream_ready(path, ready):
    """mediamtx: a path got its publisher's tracks (ready) or lost them. A stream goes live in its room only then,
    so nobody's player asks the relay for it too early."""
    m = STREAM_PATH_RE.match(path)
    room = m and next((r for r in list(srv_rooms.values()) + list(lobbies.values()) if r.stream and r.stream["id"] == m.group(1)), None)
    if not room:
        return False
    s = room.stream
    s["ready"] = ready
    log.info("stream %s %s on the relay", s["id"], "ready" if ready else "not ready any more")
    if ready and s.get("claimed"):
        room.stream_go_live()
    return True


async def on_auth_http(reader, writer):
    ok, req = False, None
    try:
        head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 5)
        lines = head.decode("latin-1").split("\r\n")
        method, path = lines[0].split()[:2]
        length = 0
        for h in lines[1:]:
            k, _, v = h.partition(":")
            if k.strip().lower() == "content-length":
                length = int(v)
        if method == "POST" and path == "/auth" and 0 <= length <= 65536:
            req = json.loads(await asyncio.wait_for(reader.readexactly(length), 5))
            ok = isinstance(req, dict) and stream_auth(req)
        elif method == "POST" and (path.startswith("/ready/") or path.startswith("/notready/")):
            ok = stream_ready(path.split("/", 2)[2], path.startswith("/ready/"))
    except (asyncio.TimeoutError, asyncio.IncompleteReadError, asyncio.LimitOverrunError, ValueError, ConnectionError):
        pass
    if isinstance(req, dict) and req.get("action") == "publish":
        log.info("publish %s from %s: %s", req.get("path"), req.get("ip"), "allowed" if ok else "refused")
    try:
        writer.write(b"HTTP/1.1 " + (b"200 OK" if ok else b"403 Forbidden") + b"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
        await writer.drain()
    except ConnectionError:
        pass
    writer.close()


# ---------------------------------------------------------------- WebSockets
async def ws_handler(ws):
    path = ws.request.path.partition("?")[0]
    ip = client_ip(ws.request.headers, ws.remote_address)
    if sum(1 for x in conns if x.ip == ip) >= MAX_CONN_PER_IP:
        await ws.close(4008, "too many connections")
        return
    if path == "/game":
        await game_ws(ws, ip)
    else:
        await web_ws(ws, ip)


async def game_ws(ws, ip):
    if ws.request.headers.get("Origin"):
        await ws.close(4003, "not for browsers")
        return
    if ip_banned(ip):
        await ws.close(4003, "banned")
        return
    c = Conn(ws, "game", ip)
    conns.add(c)
    writer = asyncio.ensure_future(c.writer())
    try:
        async for raw in ws:
            if not isinstance(raw, str):
                continue
            for line in raw.split("\n")[:64]:
                w = line.split()
                if not w:
                    continue
                now = time.monotonic()
                c.lines.append(now)
                if len(c.lines) == c.lines.maxlen and now - c.lines[0] < 10:
                    c.close(4008, "flood")
                    break
                try:
                    game_line(c, w)
                except Denied as e:
                    c.deny(str(e))
                except (ValueError, IndexError, TypeError, UnicodeDecodeError) as e:
                    log.debug("game %d: bad line %r: %s", c.id, line[:200], e)
            if c.closing:
                break
    except ConnectionClosed:
        pass
    finally:
        conns.discard(c)
        visit_end(c)
        leave_room(c)
        await asyncio.sleep(0.2)   # let the writer send a last "kicked"/"msg"
        writer.cancel()


async def web_ws(ws, ip):
    req = ws.request
    origin = req.headers.get("Origin")
    if origin and origin != ORIGIN:
        await ws.close(4003, "wrong origin")
        return
    steamid = session_user(cookie(req.headers, "cstv_session")) if origin else None
    c = Conn(ws, "web", ip, legacy=not origin)
    c.steamid = steamid
    if (steamid and globally_banned(steamid)) or (ip_banned(ip) and not is_site_admin(steamid)):
        await ws.send(json.dumps({"t": "error", "msg": "you're banned from cstv.xyz", "code": "sitebanned"}))
        await ws.close(4003, "banned")
        return
    conns.add(c)
    writer = asyncio.ensure_future(c.writer())
    try:
        hello = json.loads(await asyncio.wait_for(ws.recv(), 15))
        if hello.get("t") != "hello":
            return
        if c.legacy:   # cstv_hublink.py of an older game build
            c.ingame_name = clean_text(hello.get("name") or "", 32) or None
            c.steamid = session_user(hello.get("token"))
            if c.steamid:
                c.session = token_hash(hello["token"])
            if c.steamid and globally_banned(c.steamid):
                c.put({"t": "error", "msg": "banned", "code": "banned"})
                return
        visit_start(c, req.headers.get("User-Agent"))
        ur = user_row(c.steamid) if c.steamid else None
        c.put({"t": "welcome", "steamid": c.steamid, "name": c.name if c.steamid else None,
               "avatar": ur["avatar"] if ur else None, "now": now_ms(), "public_url": PUBLIC_URL,
               "gifs": bool(giphy_key()), "stats": public_stats(), "admin": is_site_admin(c.steamid)})
        async for raw in ws:
            try:
                m = json.loads(raw)
                if isinstance(m, dict):
                    web_message(c, m)
            except Denied as e:
                c.put({"t": "denied", "msg": str(e)})
            except (ValueError, TypeError, KeyError) as e:
                c.put({"t": "error", "msg": f"bad message: {e}"})
            if c.closing:
                break
    except (ConnectionClosed, asyncio.TimeoutError, json.JSONDecodeError):
        pass
    finally:
        conns.discard(c)
        visit_end(c)
        leave_room(c)
        yt_unsubscribe(c)
        for k in [k for k, v in hls_tokens.items() if v[0] is c]:
            del hls_tokens[k]
        await asyncio.sleep(0.2)
        writer.cancel()


# ---------------------------------------------------------------- HTTP (GET only)
def client_ip(headers, remote):
    if TRUST_CF and headers.get("Cf-Connecting-Ip"):
        return headers["Cf-Connecting-Ip"].strip()
    return (remote or ("?",))[0]


def cookie(headers, name):
    raw = headers.get("Cookie")
    if not raw:
        return None
    try:
        ck = SimpleCookie()
        ck.load(raw)
        return ck[name].value if name in ck else None
    except Exception:
        return None


def set_cookie(name, value, max_age):
    return (f"{name}={value}; Path=/; HttpOnly; SameSite=Lax; Max-Age={max_age}" + ("; Secure" if SECURE else ""))


CSP = ("default-src 'self'; script-src 'self' https://www.youtube.com https://s.ytimg.com https://player.twitch.tv "
       "https://embed.twitch.tv; frame-src https://www.youtube.com https://www.youtube-nocookie.com https://player.twitch.tv "
       "https://embed.twitch.tv https://www.twitch.tv; img-src 'self' data: https://*.steamstatic.com https://steamcdn-a.akamaihd.net "
       "https://i.ytimg.com https://steamcommunity.com https://cdn.betterttv.net https://cdn.7tv.app https://*.giphy.com "
       "https://static-cdn.jtvnw.net https://yt3.ggpht.com https://yt4.ggpht.com https://*.googleusercontent.com; "
       "media-src 'self' blob:; style-src 'self' 'unsafe-inline'; connect-src 'self' "
       + ORIGIN.replace("http", "ws", 1) + "; base-uri 'none'; "
       "form-action 'self' https://steamcommunity.com")


def respond(status, body=b"", ctype="text/plain; charset=utf-8", headers=(), cache="no-cache"):
    reason = {200: "OK", 301: "Moved Permanently", 302: "Found", 400: "Bad Request", 403: "Forbidden",
              404: "Not Found", 503: "Service Unavailable"}.get(status, "OK")
    h = Headers()
    h["Content-Type"] = ctype
    h["Content-Length"] = str(len(body))
    h["Cache-Control"] = cache
    h["X-Content-Type-Options"] = "nosniff"
    h["Referrer-Policy"] = "same-origin"
    if SECURE:
        h["Strict-Transport-Security"] = "max-age=86400"
    if ctype.startswith("text/html"):
        h["Content-Security-Policy"] = CSP
        h["X-Frame-Options"] = "DENY"
    for k, v in headers:
        h[k] = v
    return Response(status, reason, h, body)


def redirect(to, headers=()):
    return respond(302, b"", headers=(("Location", to),) + tuple(headers))


HLSJS = "hls-1.7.3.min.js"   # the HLS player (loaded only when a stream falls back to HLS); a new version = a new name
STATIC = {"app.js": "application/javascript; charset=utf-8", "app.css": "text/css; charset=utf-8",
          "favicon.svg": "image/svg+xml", "robotomono.woff2": "font/woff2", "steam-signin.png": "image/png",
          HLSJS: "application/javascript; charset=utf-8"}
FIXED = ("robotomono.woff2", "steam-signin.png", HLSJS)   # never change: browsers may keep them a week

STEAM_OPENID = "https://steamcommunity.com/openid/login"


def app_html():
    """app.html with ?v=<content hash> on its script and stylesheet: Cloudflare tells browsers to keep files for
    hours whatever we send, so every change gets a new address instead."""
    with open(os.path.join(WEB_DIR, "app.html"), "rb") as f:
        page = f.read()
    for name in ("app.js", "app.css", "favicon.svg"):
        with open(os.path.join(WEB_DIR, name), "rb") as f:
            v = hashlib.sha256(f.read()).hexdigest()[:10]
        page = page.replace(f"/static/{name}".encode(), f"/static/{name}?v={v}".encode())
    return page


def safe_next(n):
    return n if isinstance(n, str) and n.startswith("/") and not n.startswith("//") and len(n) < 200 else "/"


async def http_handler(conn, req):
    path, _, qs = req.path.partition("?")
    q = urllib.parse.parse_qs(qs)
    host = req.headers.get("Host", "")
    if SECURE and host and host != PUBLIC_HOST and path not in ("/healthz", "/ws", "/game"):   # www.cstv.xyz etc. -> the one address
        return respond(301, b"", headers=(("Location", PUBLIC_URL + req.path),))
    if path in ("/ws", "/game"):
        return None   # WebSocket handshake
    if path.startswith("/hls/"):
        return await hls_proxy(path, qs)
    if path in ("/", "/link", "/admin") or re.fullmatch(r"/l/[A-Za-z0-9]{4,8}", path):
        return respond(200, app_html(), "text/html; charset=utf-8")
    if path.startswith("/static/") and path[8:] in STATIC:
        with open(os.path.join(WEB_DIR, path[8:]), "rb") as f:
            # versioned by app_html(): a new file has a new address, so browsers may keep each one for good
            return respond(200, f.read(), STATIC[path[8:]], cache="public, max-age=31536000, immutable" if q.get("v")
                           else "public, max-age=604800" if path[8:] in FIXED else "no-cache")
    if path == "/login":
        nonce = secrets.token_urlsafe(16)
        ret = PUBLIC_URL + "/auth?" + urllib.parse.urlencode({"n": nonce, "next": safe_next(q.get("next", ["/"])[0])})
        params = {"openid.ns": "http://specs.openid.net/auth/2.0", "openid.mode": "checkid_setup",
                  "openid.return_to": ret, "openid.realm": PUBLIC_URL + "/",
                  "openid.identity": "http://specs.openid.net/auth/2.0/identifier_select",
                  "openid.claimed_id": "http://specs.openid.net/auth/2.0/identifier_select"}
        return redirect(STEAM_OPENID + "?" + urllib.parse.urlencode(params), [("Set-Cookie", set_cookie("cstv_login", nonce, 600))])
    if path == "/auth":
        return await auth_return(req, q)
    if path == "/logout":
        tok = cookie(req.headers, "cstv_session")
        if tok:
            db.x("DELETE FROM sessions WHERE hash=?", token_hash(tok))
        return redirect("/", [("Set-Cookie", set_cookie("cstv_session", "", 0))])
    if path == "/api/emotes":   # BTTV + 7TV: global, plus a Twitch channel's (?c=<login>)
        ch = q.get("c", [""])[0].lower()
        if ch and not re.fullmatch(r"[a-z0-9_]{1,25}", ch):
            return respond(400, b"bad channel")
        em = dict(await cached("global", 3600, emotes_global))
        if ch:
            em.update(await cached("ch:" + ch, 1800, lambda: emotes_channel(ch)))
        return respond(200, json.dumps({"channel": ch or None, "emotes": em}, separators=(",", ":")).encode(),
                       "application/json", cache="public, max-age=600")
    if path == "/favicon.ico":
        return redirect("/static/favicon.svg")
    if path == "/healthz":
        return respond(200, b"ok")
    return respond(404, b"not found")


async def auth_return(req, q):
    one = {k: v[0] for k, v in q.items()}
    nxt = safe_next(one.get("next", "/"))
    fail = redirect(nxt + ("&" if "?" in nxt else "?") + "login=failed")
    nonce = cookie(req.headers, "cstv_login")
    if not nonce or one.get("n") != nonce or one.get("openid.mode") != "id_res":
        return fail
    if one.get("openid.op_endpoint") != STEAM_OPENID:
        return fail
    ret = one.get("openid.return_to", "")
    if not ret.startswith(PUBLIC_URL + "/auth?"):
        return fail
    m = re.fullmatch(r"https://steamcommunity\.com/openid/id/(\d{17})", one.get("openid.claimed_id", ""))
    if not m:
        return fail
    check = {k: v for k, v in one.items() if k.startswith("openid.")}
    check["openid.mode"] = "check_authentication"
    try:
        body = (await asyncio.to_thread(http_post, STEAM_OPENID, check)).decode(errors="replace")
    except Exception as e:
        log.warning("steam check_authentication: %s", e)
        return fail
    if "is_valid:true" not in body.split("\n"):
        return fail
    steamid = m.group(1)
    if globally_banned(steamid) or (ip_banned(client_ip(req.headers, None)) and not is_site_admin(steamid)):
        return fail
    await fetch_profile(steamid)
    if not user_row(steamid):
        db.x("INSERT OR IGNORE INTO users VALUES(?,?,?,?)", steamid, None, None, time.time())
    tok = new_session(steamid, "web")
    log.info("login %s (%s)", steamid, user_name(steamid))
    return redirect(nxt, [("Set-Cookie", set_cookie("cstv_session", tok, 30 * 86400)),
                          ("Set-Cookie", set_cookie("cstv_login", "", 0))])


# ---------------------------------------------------------------- main
async def main():
    global db
    db = DB(DB_PATH)
    for fn in (housekeeping, fast_tick, sweep):
        asyncio.ensure_future(fn())
    auth = None
    if STREAM_HOST and AUTH_PORT:
        auth = await asyncio.start_server(on_auth_http, "127.0.0.1", AUTH_PORT)
    loop = asyncio.get_running_loop()
    stop = loop.create_future()
    for sig in (signal.SIGTERM, signal.SIGINT):   # systemd's stop / restart: save the lobbies first
        loop.add_signal_handler(sig, lambda: stop.done() or stop.set_result(None))
    async with serve(ws_handler, HOST, PORT, process_request=http_handler, max_size=1 << 16, ping_interval=20,
                     server_header="cstv-hub"):
        log.info("cstv-hub on http://%s:%d (public %s)%s", HOST, PORT, PUBLIC_URL,
                 f", streams via {STREAM_HOST}:{RTMP_PORT} (auth on 127.0.0.1:{AUTH_PORT})" if auth else ", no streams")
        try:
            await stop
            log.info("stopping")
        finally:
            for lb in lobbies.values():
                save_lobby(lb)
            if auth:
                auth.close()


def cli(argv):
    global db
    db = DB(DB_PATH)
    if argv[0] == "ban" and len(argv) >= 2:
        db.x("INSERT OR REPLACE INTO global_bans VALUES(?,?,?)", argv[1], " ".join(argv[2:]), time.time())
        db.x("DELETE FROM sessions WHERE steamid=?", argv[1])
        print("banned", argv[1], "(takes effect on their next connection)")
    elif argv[0] == "unban" and len(argv) == 2:
        db.x("DELETE FROM global_bans WHERE steamid=?", argv[1])
        print("unbanned", argv[1])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    logging.getLogger("websockets").setLevel(logging.WARNING)   # no line per connection
    if len(sys.argv) > 1:
        sys.exit(cli(sys.argv[1:]))
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
