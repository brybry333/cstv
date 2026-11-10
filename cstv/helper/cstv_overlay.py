#!/usr/bin/env python3
"""cstv_overlay — draws the stream chat panel (Twitch or YouTube live) and the info card for CS:TV.

Started by cstvd when needed. Commands arrive on stdin, one JSON object per line:
  {"cmd": "channel", "login": "lofigirl"}   show this Twitch channel's chat (login null = chat off)
  {"cmd": "youtube", "video": "<id>"}       show this YouTube live stream's chat
  {"cmd": "youtube", "url": "<link>"}       the same for a youtube.com/@channel/live link (id looked up)
  {"cmd": "info", "login": "lofigirl"}      show the info card for 10 s
  {"cmd": "quit"}
Outputs (layouts in ../common/cstv_proto.h):
  /tmp/cstv/chat   chat panel 512x768 BGRX, triple-buffered; the game shows it on the panel beside the TV
  /tmp/cstv/card   info card 1080x315 RGBA; cstvd blends it into the video while it is visible
  /tmp/cstv/lchat  the lobby chat for the game's Lobby page (and its emote / GIF pickers), RGBA at the size the
                   game asks for (LobbyChat below); clicks in the pickers answer on stdout
  (Windows: the named mappings Local\\cstv_chat_v1 / Local\\cstv_card_v1)

Chat is read anonymously (justinfan) from Twitch IRC. Emotes: Twitch, BTTV and 7TV (global and the
channel's), animated ones animate. YouTube live chat is polled anonymously from YouTube's own web
endpoint (live_chat page -> youtubei get_live_chat, "Live chat" view = all messages); YouTube has no
BTTV/7TV, so its emoji are YouTube's: standard ones from the color emoji font, channel/member emotes as
images. Images live in memory only; nothing is written to disk.

Test without the game:  cstv_overlay.py --test <login> <seconds> <out_prefix>
                        cstv_overlay.py --test yt:<video id> <seconds> <out_prefix>
"""
import hashlib
import io
import json
import mmap
import os
import random
import re
import socket
import ssl
import struct
import sys
import threading
import time
import urllib.request
from concurrent.futures import Future, ThreadPoolExecutor

from PIL import GifImagePlugin, Image, ImageDraw, ImageFont

# Pillow decodes GIF frames after the first without transparency by default (black boxes around
# animated BTTV/Twitch emotes); RGB_ALWAYS keeps every frame RGBA.
GifImagePlugin.LOADING_STRATEGY = GifImagePlugin.LoadingStrategy.RGB_ALWAYS
# invisible characters chatters append to get around Twitch's duplicate-message filter
INVISIBLE = re.compile("[\U000E0000-\U000E007F\u200b\u200c\u2060\ufeff\u034f\u180e]")

WINDOWS = sys.platform == "win32"
HERE = os.path.dirname(os.path.abspath(__file__))
if WINDOWS:   # named file mappings (cstv_proto.h): the name carries the layout version
    CHAT_PATH, CARD_PATH, LCHAT_PATH = "Local\\cstv_chat_v1", "Local\\cstv_card_v1", "Local\\cstv_lchat_v1"
else:
    CHAT_PATH, CARD_PATH, LCHAT_PATH = "/tmp/cstv/chat", "/tmp/cstv/card", "/tmp/cstv/lchat"
CHAT_W, CHAT_H = 512, 768
CARD_SCALE = 1.5                     # the card was laid out for a 1280x720 video; the video is 1920x1080 now
CARD_W, CARD_H = 1080, 315


def S(v):
    return int(round(v * CARD_SCALE))
GQL_CLIENT_ID = "kimne78kx3ncx6brgo4mv6wki5h1ko"   # Twitch's public web client id (same as streamlink/yt-dlp)
UA = "Mozilla/5.0 (X11; Linux x86_64) cstv-overlay"

BG = (24, 24, 27)            # Twitch dark mode
TEXT = (239, 239, 241)
MUTED = (173, 173, 184)
PURPLE = (145, 71, 255)
YT_NAME = (170, 170, 170)    # YouTube dark mode: names are grey, except
YT_OWNER = (255, 214, 0)     # the streamer
YT_MOD = (94, 132, 241)      # moderators
YT_MEMBER = (43, 166, 64)    # channel members
YT_UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36"
YT_HEADERS = {"User-Agent": YT_UA, "Accept-Language": "en-US,en;q=0.9", "Cookie": "SOCS=CAI; CONSENT=YES+"}   # skip the EU consent page
PAD, FONT_PX, LINE_H, EMOTE_H, GAP = 12, 22, 30, 28, 6
HEADER_H = 40
if WINDOWS:   # the Windows build carries its fonts
    FONT, FONT_BOLD, FONT_EMOJI = (os.path.join(HERE, "fonts", n) for n in
                                   ("NotoSans-Regular.ttf", "NotoSans-Bold.ttf", "NotoColorEmoji.ttf"))
else:
    FONT = "/usr/share/fonts/noto/NotoSans-Regular.ttf"
    FONT_BOLD = "/usr/share/fonts/noto/NotoSans-Bold.ttf"
    FONT_EMOJI = "/usr/share/fonts/noto/NotoColorEmoji.ttf"
NAME_COLORS = [(255, 0, 0), (0, 0, 255), (0, 128, 0), (178, 34, 34), (255, 127, 80), (154, 205, 50), (255, 69, 0),
               (46, 139, 87), (218, 165, 32), (210, 105, 30), (95, 158, 160), (30, 144, 255), (255, 105, 180),
               (138, 43, 226), (0, 255, 127)]


def log(*a):
    print("cstv-overlay:", *a, file=sys.stderr, flush=True)


def now_ns():
    return time.monotonic_ns()   # = CLOCK_MONOTONIC on Linux, QueryPerformanceCounter on Windows (as cstvd)


def http_get(url, data=None, headers=None, timeout=8):
    h = {"User-Agent": UA}
    if headers:
        h.update(headers)
    req = urllib.request.Request(url, data=data, headers=h)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def http_json(url, **kw):
    return json.loads(http_get(url, **kw))


# ------------------------------------------------------------------ shared memory writers
def open_shm(path, size):
    if WINDOWS:
        return mmap.mmap(-1, size, tagname=path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
    os.ftruncate(fd, size)
    m = mmap.mmap(fd, size)
    os.close(fd)
    return m


class ChatShm:
    HDR = struct.Struct("<4I 2I Q Q")

    def __init__(self):
        self.mm = open_shm(CHAT_PATH, 4096 + 3 * CHAT_W * CHAT_H * 4)
        self.mm[0:self.HDR.size] = self.HDR.pack(0x54414843, 1, CHAT_W, CHAT_H, 0xFFFFFFFF, 0xFFFFFFFF, 0, now_ns())
        self.seq = 0

    def publish(self, bgrx):
        latest, reading = struct.unpack_from("<2I", self.mm, 16)
        w = next(i for i in range(3) if i != latest and i != reading)
        off = 4096 + w * CHAT_W * CHAT_H * 4
        self.mm[off:off + len(bgrx)] = bgrx
        self.seq += 1
        struct.pack_into("<I", self.mm, 16, w)                 # latest
        struct.pack_into("<Q", self.mm, 24, self.seq)          # seq
        self.beat()

    def beat(self):
        struct.pack_into("<Q", self.mm, 32, now_ns())


class CardShm:
    HDR = struct.Struct("<6I Q Q")

    def __init__(self):
        self.mm = open_shm(CARD_PATH, 4096 + CARD_W * CARD_H * 4)
        self.mm[0:self.HDR.size] = self.HDR.pack(0x44524143, 1, CARD_W, CARD_H, S(24), S(24), 0, 0)
        self.seq = 0

    def show(self, rgba, seconds=10):
        struct.pack_into("<Q", self.mm, 32, 0)                 # hide while writing
        self.mm[4096:4096 + len(rgba)] = rgba
        self.seq += 1
        struct.pack_into("<Q", self.mm, 24, self.seq)
        struct.pack_into("<Q", self.mm, 32, now_ns() + int(seconds * 1e9))


# ------------------------------------------------------------------ emotes
class Emote:
    def __init__(self, frames, durations):
        self.frames, self.durations = frames, durations
        self.total = sum(durations) or 1
        self.w = frames[0].width

    @property
    def animated(self):
        return len(self.frames) > 1

    def frame_at(self, t_ms):
        if not self.animated:
            return self.frames[0]
        t = t_ms % self.total
        for f, d in zip(self.frames, self.durations):
            if t < d:
                return f
            t -= d
        return self.frames[-1]


EMOTE_CACHE = os.path.join(os.environ.get("LOCALAPPDATA") if WINDOWS and os.environ.get("LOCALAPPDATA")
                           else os.path.expanduser("~/.cache"), "cstv", "emotes")   # channel emote NAMES only


class Emotes:
    """BTTV + 7TV emote names -> image links. Global ones load at start; a channel's load when its chat
    is joined: BTTV and 7TV at the same time, 7TV through its GraphQL API (just names and ids: ~2.8 s
    for a big channel, against ~9 s for the full /v3/users list). A channel's list is also kept in
    ~/.cache/cstv/emotes so coming back to it shows its emotes at once (then refreshed)."""

    def __init__(self, height=EMOTE_H):
        self.height = height                  # emote images are scaled to this many pixels high
        self.global_codes = {}
        self.chan_codes = {}
        self.codes = {}                       # word -> image url (channel first, then global)
        self.room = None
        self.on_change = None                 # called with the newly added names (the renderer re-lays out those)
        self.cache = {}                       # url -> Emote or None (failed)
        self.lock = threading.Lock()
        self.pool = ThreadPoolExecutor(10)
        self.inflight = {}                    # url -> Future of a download in progress
        self.thumbs = {}                      # url -> first frame (RGBA, self.height high) or None: the picker
        self.thumb_pool = ThreadPoolExecutor(16)
        self.thumb_inflight = {}

    def _rebuild(self):
        merged = dict(self.global_codes)
        merged.update(self.chan_codes)
        added = {k for k, v in merged.items() if self.codes.get(k) != v}
        self.codes = merged
        if added and self.on_change:
            self.on_change(added)

    @staticmethod
    def _bttv_global():
        return {e["code"]: f"https://cdn.betterttv.net/emote/{e['id']}/2x" for e in http_json("https://api.betterttv.net/3/cached/emotes/global")}

    def _7tv_global(self):
        return {e["name"]: self._7tv_url(e) for e in http_json("https://7tv.io/v3/emote-sets/global").get("emotes", []) if self._7tv_url(e)}

    @staticmethod
    def _bttv_channel(room_id):
        d = http_json(f"https://api.betterttv.net/3/cached/users/twitch/{room_id}")
        return {e["code"]: f"https://cdn.betterttv.net/emote/{e['id']}/2x" for e in d.get("channelEmotes", []) + d.get("sharedEmotes", [])}

    @staticmethod
    def _gql(query):
        d = http_json("https://7tv.io/v3/gql", data=json.dumps({"query": query}).encode(), headers={"Content-Type": "application/json"}, timeout=12)
        return d.get("data") or {}

    def _7tv_channel(self, room_id):
        conns = ((self._gql('{userByConnection(platform:TWITCH,id:"%s"){connections{platform emote_set_id}}}' % room_id)
                  .get("userByConnection") or {}).get("connections")) or []
        set_id = next((c["emote_set_id"] for c in conns if c.get("platform") == "TWITCH" and c.get("emote_set_id")), None)
        if not set_id or not re.fullmatch(r"[A-Za-z0-9]+", set_id):
            return {}
        emotes = ((self._gql('{emoteSet(id:"%s"){emotes{id name}}}' % set_id).get("emoteSet") or {}).get("emotes")) or []
        return {e["name"]: f"https://cdn.7tv.app/emote/{e['id']}/2x.webp" for e in emotes if e.get("name") and e.get("id")}

    def _fetch(self, jobs):
        """Run (name, fn) jobs at the same time -> {name: dict or None (failed)}."""
        futs = {name: self.pool.submit(fn) for name, fn in jobs}
        out = {}
        for name, fut in futs.items():
            try:
                out[name] = fut.result()
            except Exception as ex:
                log(f"{name} emotes:", ex)
                out[name] = None
        return out

    def load_global(self):
        """At login the network may not be up yet, so a failed source is retried (10 s, 20 s, ... 5 min)."""
        jobs = {"BTTV global": self._bttv_global, "7TV global": self._7tv_global}
        got, wait = {}, 10
        while True:
            for name, d in self._fetch([(n, fn) for n, fn in jobs.items() if got.get(n) is None]).items():
                got[name] = d
            g = {}
            for d in got.values():
                g.update(d or {})
            self.global_codes = g
            self._rebuild()
            if all(got.get(n) is not None for n in jobs):
                return
            time.sleep(wait)
            wait = min(wait * 2, 300)

    def load_channel(self, room_id):
        if not room_id.isdigit():
            return
        self.room = room_id
        path = os.path.join(EMOTE_CACHE, room_id + ".json")
        cached = {}
        try:
            with open(path) as f:
                cached = json.load(f)
            self.chan_codes = {**cached.get("bttv", {}), **cached.get("7tv", {})}
            self._rebuild()
            log(f"emotes from last time: {len(self.chan_codes)} for this channel")
        except (OSError, ValueError):
            self.chan_codes = {}
            self._rebuild()
        got = self._fetch([("bttv", lambda: self._bttv_channel(room_id)), ("7tv", lambda: self._7tv_channel(room_id))])
        if self.room != room_id:
            return   # switched channels meanwhile
        fresh = {k: (v if v is not None else cached.get(k, {})) for k, v in got.items()}   # a failed source keeps last time's
        self.chan_codes = {**fresh["bttv"], **fresh["7tv"]}
        self._rebuild()
        try:
            os.makedirs(EMOTE_CACHE, exist_ok=True)
            tmp = path + ".tmp"
            with open(tmp, "w") as f:
                json.dump(fresh, f)
            os.replace(tmp, path)
        except OSError:
            pass
        log(f"emotes ready: {len(self.codes)} BTTV/7TV codes ({len(self.chan_codes)} for this channel)")

    @staticmethod
    def _7tv_url(e):
        host = ((e.get("data") or {}).get("host") or {})
        return ("https:" + host["url"] + "/2x.webp") if host.get("url") else None

    def fetch(self, url):
        """Future of the emote at url; downloads it once, however many messages ask meanwhile."""
        with self.lock:
            if url in self.cache:
                f = Future()
                f.set_result(self.cache[url])
                return f
            fut = self.inflight.get(url)
            if fut is None:
                fut = self.pool.submit(self._get_once, url)
                self.inflight[url] = fut
            return fut

    def _get_once(self, url):
        try:
            return self.get(url)
        finally:
            with self.lock:
                self.inflight.pop(url, None)

    @staticmethod
    def image_bytes(url):
        """The image file at url, from ~/.cache/cstv/emotes/img when it was fetched before (emote links never
        change what they point at)."""
        path = os.path.join(EMOTE_CACHE, "img", hashlib.sha1(url.encode()).hexdigest())
        try:
            with open(path, "rb") as f:
                return f.read()
        except OSError:
            pass
        data = http_get(url)
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path + ".tmp", "wb") as f:
                f.write(data)
            os.replace(path + ".tmp", path)
        except OSError:
            pass
        return data

    def fetch_thumb(self, url):
        """Future of the emote's first frame only (the picker): no decoding of every frame of an animation."""
        with self.lock:
            em = self.cache.get(url)
            if url in self.thumbs or em:
                f = Future()
                f.set_result(self.thumbs.get(url) or (em.frames[0] if em else None))
                return f
            fut = self.thumb_inflight.get(url)
            if fut is None:
                fut = self.thumb_pool.submit(self._thumb_once, url)
                self.thumb_inflight[url] = fut
            return fut

    def _thumb_once(self, url):
        fr = None
        try:
            im = Image.open(io.BytesIO(self.image_bytes(url)))
            im.seek(0)
            f = im.convert("RGBA")
            fr = f.resize((max(1, round(f.width * self.height / f.height)), self.height), Image.BILINEAR, reducing_gap=2.0)
        except Exception as ex:
            log("emote", url, ex)
        with self.lock:
            self.thumbs[url] = fr
            self.thumb_inflight.pop(url, None)
            if len(self.thumbs) > 3000:
                self.thumbs.pop(next(iter(self.thumbs)))
        return fr

    def prefetch_thumbs(self, n=240):
        """The picker's first screens before it's opened: channel emotes first (as it lists them), then global."""
        names = sorted(self.chan_codes)[:n] + sorted(k for k in self.global_codes if k not in self.chan_codes)[:n // 2]
        for name in names:
            url = self.codes.get(name)
            if url:
                self.fetch_thumb(url)

    def get(self, url):
        """Load (or return cached) emote image, scaled to self.height; None on failure."""
        with self.lock:
            if url in self.cache:
                return self.cache[url]
        em = None
        try:
            im = Image.open(io.BytesIO(self.image_bytes(url)))
            n = min(getattr(im, "n_frames", 1), 60)   # long animations: the first 60 frames
            frames, durs = [], []
            for i in range(n):
                im.seek(i)
                f = im.convert("RGBA")
                w = max(1, round(f.width * self.height / f.height))
                frames.append(f.resize((w, self.height), Image.BILINEAR, reducing_gap=2.0))   # ~5x faster than LANCZOS at 28 px
                durs.append(max(20, int(im.info.get("duration", 100) or 100)))
            em = Emote(frames, durs)
        except Exception as ex:
            log("emote", url, ex)
        with self.lock:
            self.cache[url] = em
            if len(self.cache) > 800:
                self.cache.pop(next(iter(self.cache)))
        return em


# ------------------------------------------------------------------ text with emoji
class Text:
    def __init__(self, px=FONT_PX, regular=None, bold=None):
        self.px = px
        self.font = ImageFont.truetype(regular or FONT, px)
        self.bold = ImageFont.truetype(bold or FONT_BOLD, px)
        try:
            self.emoji_font = ImageFont.truetype(FONT_EMOJI, 109)   # the only size the color emoji font has
        except Exception:
            self.emoji_font = None
        self.emoji_cache = {}

    @staticmethod
    def is_emoji(ch):
        o = ord(ch)
        return 0x1F000 <= o <= 0x1FAFF or 0x2600 <= o <= 0x27BF or 0x1F900 <= o <= 0x1F9FF or 0x2B00 <= o <= 0x2BFF

    def emoji(self, ch):
        if ch not in self.emoji_cache:
            img = None
            if self.emoji_font:
                try:
                    im = Image.new("RGBA", (136, 128), (0, 0, 0, 0))
                    ImageDraw.Draw(im).text((0, 0), ch, font=self.emoji_font, embedded_color=True)
                    box = im.getbbox()
                    if box:
                        im = im.crop(box)
                        img = im.resize((max(1, round(im.width * self.px / im.height)), self.px), Image.LANCZOS)
                except Exception:
                    img = None
            self.emoji_cache[ch] = img
        return self.emoji_cache[ch]


# ------------------------------------------------------------------ one chat message, laid out
class Message:
    def __init__(self, img, animated, user_id, msg_id):
        self.img, self.animated, self.user_id, self.msg_id = img, animated, user_id, msg_id
        self.h = img.height


def layout_message(name, color, text, twitch_emotes, emotes, txt, user_id, msg_id, action=False):
    """Pieces: ('text', str, font, rgb) | ('emote', Emote).
    Twitch: text + emote ranges. YouTube: text is None and twitch_emotes holds the message already split
    into ('t', text) / ('e', image url) segments (no BTTV/7TV codes there)."""
    pieces = [("text", name, txt.bold, color), ("text", " " if action else ": ", txt.font, MUTED)]
    body_rgb = color if action else TEXT
    use_codes = text is not None
    if text is None:
        text, twitch_emotes, presplit = "", [], twitch_emotes
    else:
        presplit = None
    # Twitch emote ranges are code point indices into the message
    ranges = []
    for eid, spans in twitch_emotes:
        for a, b in spans:
            ranges.append((a, b, f"https://static-cdn.jtvnw.net/emoticons/v2/{eid}/default/dark/2.0"))
    ranges.sort()
    segs, pos = [], 0
    for a, b, url in ranges:
        if a < pos or b >= len(text):
            continue
        if a > pos:
            segs.append(("t", text[pos:a]))
        segs.append(("e", url))
        pos = b + 1
    if pos < len(text):
        segs.append(("t", text[pos:]))
    if presplit is not None:
        segs = presplit
    futures = {}
    words = []
    for kind, val in segs:
        if kind == "e":
            words.append(("emote", val))
            continue
        for w in re.split(r"(\s+)", val):
            if not w:
                continue
            if use_codes and w.strip() and w in emotes.codes:
                words.append(("emote", emotes.codes[w]))
            else:
                words.append(("text", w))
    for kind, val in words:
        if kind == "emote" and val not in futures:
            futures[val] = emotes.fetch(val)
    for kind, val in words:
        if kind == "emote":
            em = futures[val].result()
            pieces.append(("emote", em) if em else ("text", "?", txt.font, body_rgb))
        else:
            pieces.append(("text", val, txt.font, body_rgb))

    # wrap into lines
    maxw = CHAT_W - 2 * PAD
    lines, line, x = [], [], 0
    draw_probe = ImageDraw.Draw(Image.new("RGB", (1, 1)))

    def text_width(s, font):
        return int(draw_probe.textlength(s, font=font))

    def push(item, w):
        nonlocal line, x
        if x + w > maxw and line:
            lines.append(line)
            line, x = [], 0
            if item[0] == "text" and not item[1].strip():
                return
        line.append((x, item))
        x += w

    for p in pieces:
        if p[0] == "emote":
            push(p, p[1].w + 2)
            continue
        _, s, font, rgb = p
        # split out emoji characters so they can use the color emoji font
        buf = ""
        for ch in s:
            if txt.is_emoji(ch) and txt.emoji(ch) is not None:
                if buf:
                    push(("text", buf, font, rgb), text_width(buf, font))
                    buf = ""
                push(("emoji", txt.emoji(ch)), txt.emoji(ch).width + 1)
            else:
                buf += ch
        if buf:
            w = text_width(buf, font)
            if w > maxw:   # a single very long word: hard-break it
                for ch in buf:
                    push(("text", ch, font, rgb), text_width(ch, font))
            else:
                push(("text", buf, font, rgb), w)
    if line:
        lines.append(line)

    img = Image.new("RGBA", (CHAT_W, max(1, len(lines)) * LINE_H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    animated = []
    for li, ln in enumerate(lines):
        y = li * LINE_H
        for x0, item in ln:
            if item[0] == "text":
                d.text((PAD + x0, y + 2), item[1], font=item[2], fill=item[3])
            elif item[0] == "emoji":
                img.alpha_composite(item[1], (PAD + x0, y + (LINE_H - item[1].height) // 2))
            else:
                em = item[1]
                ey = y + (LINE_H - EMOTE_H) // 2
                if em.animated:
                    animated.append((PAD + x0, ey, em))
                else:
                    img.alpha_composite(em.frames[0], (PAD + x0, ey))
    return Message(img, animated, user_id, msg_id)


# ------------------------------------------------------------------ Twitch chat connection
def unescape_tag(v):
    return re.sub(r"\\(.)", lambda m: {"s": " ", ":": ";", "\\": "\\", "r": "\r", "n": "\n"}.get(m.group(1), m.group(1)), v)


def hex_rgb(h):
    try:
        return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))
    except Exception:
        return None


class ChatClient(threading.Thread):
    def __init__(self, login, renderer):
        super().__init__(daemon=True)
        self.login, self.r = login.lower(), renderer
        self.stopped = False
        self.sock = None

    def stop(self):
        self.stopped = True
        try:
            self.sock and self.sock.close()
        except Exception:
            pass

    def run(self):
        while not self.stopped:
            try:
                raw = socket.create_connection(("irc.chat.twitch.tv", 6697), timeout=20)
                self.sock = ssl.create_default_context().wrap_socket(raw, server_hostname="irc.chat.twitch.tv")
                s = self.sock
                s.sendall(b"CAP REQ :twitch.tv/tags twitch.tv/commands\r\n")
                s.sendall(f"NICK justinfan{random.randint(10000, 99999)}\r\nJOIN #{self.login}\r\n".encode())
                s.settimeout(1.0)
                buf = b""
                log(f"joined #{self.login} chat")
                while not self.stopped:
                    try:
                        data = s.recv(65536)
                    except socket.timeout:
                        continue
                    if not data:
                        break
                    buf += data
                    while b"\r\n" in buf:
                        line, buf = buf.split(b"\r\n", 1)
                        self.handle(line.decode("utf-8", "replace"))
            except Exception as ex:
                if not self.stopped:
                    log("chat connection:", ex)
            if not self.stopped:
                time.sleep(3)

    def handle(self, line):
        if line.startswith("PING"):
            self.sock.sendall(("PONG" + line[4:] + "\r\n").encode())
            return
        tags = {}
        if line.startswith("@"):
            t, line = line[1:].split(" ", 1)
            for kv in t.split(";"):
                k, _, v = kv.partition("=")
                tags[k] = unescape_tag(v)
        parts = line.split(" ", 3)
        if len(parts) < 2:
            return
        cmd = parts[1]
        if cmd == "ROOMSTATE" and tags.get("room-id"):
            self.r.on_room(tags["room-id"])
        elif cmd == "PRIVMSG" and len(parts) >= 4:
            text = parts[3][1:] if parts[3].startswith(":") else parts[3]
            if not tags.get("emotes"):     # emote positions count code points, so only strip when there are none
                text = INVISIBLE.sub("", text).rstrip()
            action = text.startswith("\x01ACTION ") and text.endswith("\x01")
            if action:
                text = text[8:-1]
            emotes = []
            for spec in filter(None, tags.get("emotes", "").split("/")):
                eid, _, rngs = spec.partition(":")
                spans = []
                for r in rngs.split(","):
                    a, _, b = r.partition("-")
                    if a.isdigit() and b.isdigit():
                        spans.append((int(a), int(b)))
                emotes.append((eid, spans))
            name = tags.get("display-name") or parts[0][1:].split("!")[0]
            color = hex_rgb(tags.get("color", "")) or NAME_COLORS[hash(name) % len(NAME_COLORS)]
            self.r.on_message(name, color, text, emotes, tags.get("user-id", ""), tags.get("id", ""), action)
        elif cmd == "CLEARCHAT":
            self.r.on_clear(tags.get("target-user-id"))
        elif cmd == "CLEARMSG":
            self.r.on_delete(tags.get("target-msg-id"))


# ------------------------------------------------------------------ YouTube live chat
class YouTubeChat(threading.Thread):
    POLL = 2.0   # seconds between fetches; each batch is spread over that time so chat flows instead of jumping

    def __init__(self, video, renderer, url=None):
        super().__init__(daemon=True)
        self.video, self.r, self.url = video, renderer, url
        self.stopped = False

    def stop(self):
        self.stopped = True

    def sleep(self, secs):
        end = time.monotonic() + secs
        while not self.stopped and time.monotonic() < end:
            time.sleep(min(0.1, end - time.monotonic()))

    def run(self):
        while not self.stopped:
            try:
                if self.session() == "disabled":
                    return
            except Exception as ex:
                if not self.stopped:
                    log("youtube chat:", ex)
            self.sleep(5)

    def session(self):
        if not self.video:   # youtube.com/@channel/live: the page names the live video
            page = http_get(self.url, headers=YT_HEADERS, timeout=10).decode("utf-8", "replace")
            m = re.search(r'<link rel="canonical" href="https://www\.youtube\.com/watch\?v=([A-Za-z0-9_-]{11})"', page)
            if not m:
                self.r.set_note("Couldn't find this stream's chat.")
                return "disabled"
            self.video = m.group(1)
        html = http_get(f"https://www.youtube.com/live_chat?is_popout=1&v={self.video}", headers=YT_HEADERS, timeout=10).decode("utf-8", "replace")
        ver = re.search(r'"INNERTUBE_CONTEXT_CLIENT_VERSION":"([^"]+)"', html)
        m = re.search(r'(?:window\["ytInitialData"\]|var ytInitialData)\s*=\s*(\{.+?\});\s*</script>', html, re.S)
        if not ver or not m:
            raise RuntimeError("live chat page without chat data")
        data = json.loads(m.group(1))
        lcr = data.get("contents", {}).get("liveChatRenderer")
        if not lcr:
            note = "".join(r.get("text", "") for r in data.get("contents", {}).get("messageRenderer", {}).get("text", {}).get("runs", []))
            log(f"youtube chat {self.video}: {note or 'no chat'}")
            self.r.set_note(note or "This stream has no chat.")
            return "disabled"
        # "Live chat" (every message) instead of the default "Top chat"
        cont, wait = None, 1000
        try:
            items = lcr["header"]["liveChatHeaderRenderer"]["viewSelector"]["sortFilterSubMenuRenderer"]["subMenuItems"]
            cont = items[-1]["continuation"]["reloadContinuationData"]["continuation"]
        except (KeyError, IndexError, TypeError):
            self.handle(lcr.get("actions", []), spread=0)
            cont, wait = self.next_cont(lcr.get("continuations", []))
        log(f"youtube chat {self.video}: connected")
        self.r.set_note(None)
        ctx = {"client": {"clientName": "WEB", "clientVersion": ver.group(1), "hl": "en", "gl": "US"}}
        first = True
        while not self.stopped and cont:
            body = json.dumps({"context": ctx, "continuation": cont}).encode()
            r = json.loads(http_get("https://www.youtube.com/youtubei/v1/live_chat/get_live_chat?prettyPrint=false", data=body,
                                    headers=dict(YT_HEADERS, **{"Content-Type": "application/json"}), timeout=10))
            lcc = r.get("continuationContents", {}).get("liveChatContinuation")
            if not lcc:
                log(f"youtube chat {self.video}: ended")
                return
            t0 = time.monotonic()
            self.handle(lcc.get("actions", []), spread=0 if first else self.POLL)
            first = False
            cont, wait = self.next_cont(lcc.get("continuations", []))
            self.sleep(max(0.0, self.POLL - (time.monotonic() - t0)))

    @staticmethod
    def next_cont(conts):
        for c in conts:
            for k in ("invalidationContinuationData", "timedContinuationData", "reloadContinuationData"):
                if k in c:
                    return c[k].get("continuation"), c[k].get("timeoutMs", 1000)
        return None, 0

    def handle(self, actions, spread):
        msgs = []
        for a in actions:
            if "addChatItemAction" in a:
                m = self.message(a["addChatItemAction"].get("item", {}))
                if m:
                    msgs.append(m)
            elif "removeChatItemAction" in a:
                self.r.on_delete(a["removeChatItemAction"].get("targetItemId"))
            elif "markChatItemAsDeletedAction" in a:
                self.r.on_delete(a["markChatItemAsDeletedAction"].get("targetItemId"))
            elif "markChatItemsByAuthorAsDeletedAction" in a:
                self.r.on_clear(a["markChatItemsByAuthorAsDeletedAction"].get("externalChannelId"))
        gap = spread / len(msgs) if msgs and spread else 0
        for m in msgs:
            if self.stopped:
                return
            self.r.on_message(*m)
            if gap:
                self.sleep(gap)

    @staticmethod
    def message(item):
        paid = item.get("liveChatPaidMessageRenderer")
        r = item.get("liveChatTextMessageRenderer") or paid
        if not r:
            return None   # welcome banners, gifts, membership notices
        name = r.get("authorName", {}).get("simpleText", "?")
        kinds = set()
        for b in r.get("authorBadges", []):
            br = b.get("liveChatAuthorBadgeRenderer", {})
            icon = br.get("icon", {}).get("iconType")
            kinds.add(icon if icon else ("MEMBER" if "customThumbnail" in br else ""))
        color = YT_OWNER if "OWNER" in kinds else YT_MOD if "MODERATOR" in kinds else YT_MEMBER if "MEMBER" in kinds else YT_NAME
        segs = []
        if paid:
            name = f"{name} {paid.get('purchaseAmountText', {}).get('simpleText', '')}".strip()
            color = YT_OWNER
        for run in r.get("message", {}).get("runs", []):
            if "text" in run:
                segs.append(("t", run["text"]))
            elif "emoji" in run:
                e = run["emoji"]
                eid = e.get("emojiId", "")
                thumbs = e.get("image", {}).get("thumbnails", [])
                if e.get("isCustomEmoji") or "/" in eid:
                    if thumbs:
                        segs.append(("e", max(thumbs, key=lambda t: t.get("width", 0))["url"]))
                else:
                    segs.append(("t", eid))   # a standard emoji: drawn with the color emoji font
        if not segs:
            return None
        return (name, color, None, segs, r.get("authorExternalChannelId", ""), r.get("id", ""), False)


# ------------------------------------------------------------------ the renderer
class Renderer:
    def __init__(self, chat_shm=True):
        self.txt = Text()
        self.emotes = Emotes()
        self.emotes.on_change = self.request_relayout
        threading.Thread(target=self.emotes.load_global, daemon=True).start()
        self.msgs = []              # laid-out Message objects, oldest first
        self.lock = threading.Lock()
        self.incoming = []          # raw messages waiting for layout
        self.raw = []               # the last 40 raw messages (laid out again when emotes arrive)
        self.pending = []           # ids of messages to lay out again (they contain newly known emote names)
        self.cv = threading.Condition()
        self.client = None
        self.login = None
        self.label = None           # header text on the right ("#login" / "YouTube live"); None = chat off
        self.note = None            # e.g. "Chat is disabled for this live stream."
        self.dirty = True
        self.chat = ChatShm() if chat_shm else None
        self.card = CardShm() if chat_shm else None
        self.loaded_room = None
        threading.Thread(target=self.layout_worker, daemon=True).start()

    # --- chat events (from the IRC thread)
    def on_room(self, room_id):
        if room_id != self.loaded_room:
            self.loaded_room = room_id
            threading.Thread(target=self.emotes.load_channel, args=(room_id,), daemon=True).start()

    def request_relayout(self, added):
        with self.cv:
            for m in self.raw:
                if m[2] and m[5] and m[5] not in self.pending and any(w in added for w in m[2].split()):
                    self.pending.append(m[5])
            if self.pending:
                self.cv.notify()

    def _prefetch(self, msg):
        """Start downloading a message's emotes the moment it arrives (in parallel with the others),
        so laying it out doesn't wait on the network one message at a time."""
        _, _, text, tw = msg[:4]
        if text is None:   # YouTube: ("t", text) / ("e", url) pieces
            urls = [v for k, v in tw if k == "e"]
        else:
            urls = [f"https://static-cdn.jtvnw.net/emoticons/v2/{eid}/default/dark/2.0" for eid, _ in tw]
            codes = self.emotes.codes
            urls += [codes[w] for w in text.split() if w in codes]
        for u in urls:
            self.emotes.fetch(u)

    def on_message(self, *msg):
        self._prefetch(msg)
        with self.cv:
            self.raw.append(msg)
            del self.raw[:-40]
            self.incoming.append(msg)
            if len(self.incoming) > 22:          # very busy chat: only the newest ~20 fit on the panel anyway
                del self.incoming[:-22]
            self.cv.notify()

    def on_clear(self, user_id):
        with self.cv:
            self.raw = [m for m in self.raw if user_id and m[4] != user_id]
        with self.lock:
            self.msgs = [m for m in self.msgs if user_id and m.user_id != user_id]
            self.dirty = True

    def on_delete(self, msg_id):
        with self.cv:
            self.raw = [m for m in self.raw if m[5] != msg_id]
        with self.lock:
            self.msgs = [m for m in self.msgs if m.msg_id != msg_id]
            self.dirty = True

    def layout_worker(self):
        while True:
            with self.cv:
                while not self.incoming and not self.pending:
                    self.cv.wait()
                again = None
                if not self.incoming:   # new messages first; then ones whose emotes only just became known
                    pid = self.pending.pop(0)
                    again = next((m for m in self.raw if m[5] == pid), None)
                    if again is None:
                        continue
                else:
                    name, color, text, emotes, uid, mid, action = self.incoming.pop(0)
            if again is not None:
                try:
                    lm = layout_message(*again[:4], self.emotes, self.txt, *again[4:])
                except Exception as ex:
                    log("layout:", ex)
                    continue
                with self.lock:
                    self.msgs = [lm if m.msg_id == again[5] else m for m in self.msgs]
                    self.dirty = True
                continue
            try:
                m = layout_message(name, color, text, emotes, self.emotes, self.txt, uid, mid, action)
            except Exception as ex:
                log("layout:", ex)
                continue
            with self.lock:
                self.msgs.append(m)
                del self.msgs[:-40]
                self.dirty = True

    def set_note(self, note):
        with self.lock:
            self.note = note
            self.dirty = True

    # --- commands
    def _switch(self, label, client):
        if self.client:
            self.client.stop()
            self.client = None
        with self.cv:
            self.raw = []
            self.pending = []
            self.incoming.clear()
        with self.lock:
            self.msgs = []
            self.note = None
            self.label = label
            self.dirty = True
        if client:
            self.client = client
            client.start()

    def set_channel(self, login):
        self.login = login
        self._switch("#" + login if login else None, ChatClient(login, self) if login else None)

    def set_youtube(self, video, url=None):
        self.login = None
        ok = bool(video) and re.fullmatch(r"[A-Za-z0-9_-]{6,20}", video)
        if not ok and url and re.match(r"https?://(www\.|m\.)?youtube\.com/", url):
            self._switch("YouTube live", YouTubeChat(None, self, url))
            return
        self._switch("YouTube live" if ok else None, YouTubeChat(video, self) if ok else None)

    def show_info(self, login):
        try:
            img = render_card(login)
            if self.card:
                self.card.show(img.tobytes("raw", "RGBA"))
            return img
        except Exception as ex:
            log("info card:", ex)

    # --- drawing
    def compose(self, t_ms):
        panel = Image.new("RGB", (CHAT_W, CHAT_H), BG)
        d = ImageDraw.Draw(panel)
        d.rectangle((0, 0, CHAT_W, HEADER_H - 1), fill=(14, 14, 16))
        d.text((PAD, 8), "STREAM CHAT", font=self.txt.bold, fill=TEXT)
        if self.label:
            d.text((CHAT_W - PAD - d.textlength(self.label, font=self.txt.font), 8), self.label, font=self.txt.font, fill=MUTED)
        d.line((0, HEADER_H - 1, CHAT_W, HEADER_H - 1), fill=(48, 48, 54))
        y = CHAT_H - PAD
        any_anim = False
        with self.lock:
            msgs = list(self.msgs)
        for m in reversed(msgs):
            y -= m.h
            if y < HEADER_H:
                break
            panel.paste(m.img, (0, y), m.img)
            for x, ey, em in m.animated:
                fr = em.frame_at(t_ms)
                panel.paste(fr, (x, y + ey), fr)
                any_anim = True
            y -= GAP
        if not msgs:
            d.text((PAD, HEADER_H + 12), self.note or ("Waiting for chat..." if self.label else "Chat is off"), font=self.txt.font, fill=MUTED)
        return panel, any_anim

    def run(self):
        last_anim = 0
        while True:
            t = time.monotonic()
            with self.lock:
                dirty = self.dirty
                self.dirty = False
            if self.label and (dirty or (self.has_anim and t - last_anim >= 1 / 20)):
                panel, self.has_anim = self.compose(t * 1000)
                last_anim = t
                if self.chat:
                    self.chat.publish(panel.tobytes("raw", "BGRX"))
            elif self.chat:
                self.chat.beat()
            time.sleep(0.02)

    has_anim = False


# ------------------------------------------------------------------ the lobby chat (the game's Lobby page)
# Fed by cstvd: {"cmd": "hub", "line": ...} with the hub's lobby lines (lchat_reset, lchat <id> <steamid> <name>
# <text> <web|game|sys> <gif url>, lchat_del <ids>, lchat_emotes <channel|->, gifs <q> <offset> <next|-> <id,w,h,url>...)
# and {"cmd": "lchat_ui", "line": ...} from the game (size <w> <h> <scale*100>, show 0|1, mode chat|emotes|gifs,
# scroll <notches, + = up>, click <x> <y>, search <b64|->). Answers on stdout, a line each (base64url like the
# protocol): pick <emote name>, gifpick <GIF id>, gifmore <query> <offset>.
LCHAT_MAGIC = 0x5448434C        # "LCHT"
LCHAT_MAXW, LCHAT_MAXH = 800, 1200
# the website's look (cstv.xyz: Roboto Mono, its greys and blue), as the CS:TV window has it
LC_BG, LC_TEXT, LC_SUB, LC_FAINT = (13, 13, 13), (228, 228, 228), (138, 138, 138), (90, 90, 90)
LC_ROW, LC_ACCENT = (23, 23, 23), (74, 143, 240)
LC_FONT, LC_FONT_BOLD = (os.path.join(HERE, "fonts", n) for n in ("RobotoMono-Regular.ttf", "RobotoMono-Bold.ttf"))
if not os.path.exists(LC_FONT):
    LC_FONT, LC_FONT_BOLD = FONT, FONT_BOLD


def b64url(s):
    if not s or s == "-":
        return ""
    import base64
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4)).decode("utf-8", "replace")


def web_name_color(steamid):
    """The website's nameColor(): hsl(x % 360, 70%, 70%) of a hash of the SteamID."""
    import colorsys
    x = 0
    for ch in str(steamid):
        x = (x * 31 + ord(ch)) & 0xFFFFFFFF
    r, g, b = colorsys.hls_to_rgb((x % 360) / 360, 0.70, 0.70)
    return (round(r * 255), round(g * 255), round(b * 255))


class LChatShm:
    def __init__(self):
        self.mm = open_shm(LCHAT_PATH, 4096 + 3 * LCHAT_MAXW * LCHAT_MAXH * 4)
        self.mm[0:ChatShm.HDR.size] = ChatShm.HDR.pack(LCHAT_MAGIC, 1, 0, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0, now_ns())
        self.seq = 0

    def publish(self, w, h, bgrx):
        latest, reading = struct.unpack_from("<2I", self.mm, 16)
        i = next(k for k in range(3) if k != latest and k != reading)
        off = 4096 + i * LCHAT_MAXW * LCHAT_MAXH * 4
        self.mm[off:off + len(bgrx)] = bgrx
        struct.pack_into("<2I", self.mm, 8, w, h)
        self.seq += 1
        struct.pack_into("<I", self.mm, 16, i)
        struct.pack_into("<Q", self.mm, 24, self.seq)
        self.beat()

    def beat(self):
        struct.pack_into("<Q", self.mm, 32, now_ns())


class LobbyChat:
    def __init__(self):
        self.lock = threading.RLock()
        self.shm = None
        self.w = self.h = 0
        self.shown = False
        self.mode = "chat"
        self.msgs = []             # dicts: id sid name text kind gif
        self.laid = {}             # id -> (image, [(x, y, Emote)]) at the current size
        self.scroll = 0            # chat: px up from the newest; pickers: px down from the top
        self.content_h = 0
        self.search = ""
        self.gifs, self.gif_q, self.gif_next, self.gif_asked = [], None, None, None
        self.media = {}            # (url, w, h) -> Emote (animated frames) or None
        self.hits = []             # (x0, y0, x1, y1, kind, value) in the last picker frame
        self.channel = None
        self.draft, self.draft_laid = "", None   # the message being written (shown with its emotes)
        self.me = ""                              # this player's cstv.xyz name: mentions of it are highlighted
        self.view_h = 0                           # the picture's height above the draft strip
        self.dirty = True
        self.pool = ThreadPoolExecutor(6)
        try:   # now, not at the first picture: the file may still hold the last game's picture (another size)
            self.shm = LChatShm()
        except Exception as ex:
            log("lobby chat shm:", ex)
        self.set_scale(1.0)
        self.emotes = Emotes(self.emote_h)
        self.emotes.on_change = self._emotes_changed
        threading.Thread(target=self.emotes.load_global, daemon=True).start()
        threading.Thread(target=self.run, daemon=True).start()

    def _emotes_changed(self, added):
        self.relayout()
        self.dirty = True   # the emote picker too (it showed "Loading" until now)
        threading.Thread(target=self.emotes.prefetch_thumbs, daemon=True).start()

    def set_scale(self, sc):
        self.sc = sc
        self.px = max(9, round(12 * sc))
        self.line_h = max(12, round(18 * sc))
        self.emote_h = max(12, round(22 * sc))
        self.pad = max(6, round(10 * sc))
        self.gap = max(2, round(5 * sc))
        self.gif_h = max(40, round(110 * sc))
        self.txt = Text(self.px, LC_FONT, LC_FONT_BOLD)
        self.small = ImageFont.truetype(LC_FONT_BOLD, max(8, round(10 * sc)))

    @staticmethod
    def out(*words):
        try:
            sys.stdout.write(" ".join(str(w) for w in words) + "\n")
            sys.stdout.flush()
        except Exception:
            pass

    @staticmethod
    def b64(s):
        import base64
        return base64.urlsafe_b64encode(s.encode()).decode().rstrip("=") if s else "-"

    def relayout(self, only=None):
        """Lay messages out again (all, or those `only` picks); each keeps its old picture until the new one is ready."""
        with self.lock:
            msgs = [m for m in self.msgs if only is None or only(m)]
            draft = self.draft
        for m in msgs:
            self.pool.submit(self._layout_job, m)
        if draft:
            self.pool.submit(self._draft_job, draft)

    # --- from the hub
    def on_hub(self, line):
        w = line.split()
        if not w:
            return
        v = w[0]
        if v == "lchat_reset":
            with self.lock:
                self.msgs, self.laid, self.scroll, self.dirty = [], {}, 0, True
        elif v == "lchat" and len(w) >= 7:
            m = {"id": w[1], "sid": None if w[2] == "-" else w[2], "name": b64url(w[3]), "text": b64url(w[4]),
                 "kind": w[5], "gif": b64url(w[6])}
            with self.lock:
                self.msgs.append(m)
                for old in self.msgs[:-80]:
                    self.laid.pop(old["id"], None)
                del self.msgs[:-80]
            self.pool.submit(self._layout_job, m)
        elif v == "lchat_del":
            ids = set(w[1:])
            with self.lock:
                self.msgs = [m for m in self.msgs if m["id"] not in ids]
                self.dirty = True
        elif v == "lchat_emotes" and len(w) >= 2:
            ch = None if w[1] == "-" else w[1].lower()
            if ch != self.channel:
                self.channel = ch
                threading.Thread(target=self._load_channel, args=(ch,), daemon=True).start()
        elif v == "gifs" and len(w) >= 4:
            q, off = b64url(w[1]), int(w[2]) if w[2].isdigit() else 0
            with self.lock:
                if q != (self.gif_q or ""):
                    return   # an older search
                if off == 0:
                    self.gifs, self.scroll = [], 0
                seen = {g[0] for g in self.gifs}
                for item in w[4:]:
                    f = item.split(",")
                    if len(f) == 4 and f[0] not in seen and f[1].isdigit() and f[2].isdigit():
                        self.gifs.append((f[0], max(1, int(f[1])), max(1, int(f[2])), b64url(f[3])))
                        seen.add(f[0])
                self.gif_next = int(w[3]) if w[3].isdigit() else None
                self.dirty = True

    def _load_channel(self, login):
        if not login:
            self.emotes.chan_codes = {}
            self.emotes._rebuild()
            return
        try:
            q = {"query": "query($l:String!){user(login:$l){id}}", "variables": {"l": login}}
            d = json.loads(http_get("https://gql.twitch.tv/gql", data=json.dumps(q).encode(),
                                    headers={"Client-Id": GQL_CLIENT_ID, "Content-Type": "application/json"}))
            uid = ((d.get("data") or {}).get("user") or {}).get("id")
            if uid and self.channel == login:
                self.emotes.load_channel(uid)
        except Exception as ex:
            log("lobby chat emotes:", ex)

    # --- from the game
    def on_ui(self, line):
        w = line.split()
        if not w:
            return
        v = w[0]
        with self.lock:
            if v == "size" and len(w) >= 4:
                nw, nh = min(LCHAT_MAXW, max(64, int(w[1]))), min(LCHAT_MAXH, max(64, int(w[2])))
                sc = max(0.5, min(3.0, int(w[3]) / 100))
                if (nw, nh, sc) != (self.w, self.h, self.sc):
                    rescale = sc != self.sc or nw != self.w
                    self.w, self.h = nw, nh
                    if sc != self.sc:
                        self.set_scale(sc)
                        self.emotes.height = self.emote_h
                        self.emotes.cache = {}
                    self.media = {}
                    self.dirty = True
                    if rescale:
                        self.laid = {}
                        self.relayout()
            elif v == "show" and len(w) >= 2:
                self.shown = w[1] == "1"
                self.dirty = True
            elif v == "draft" and len(w) >= 2:
                d = b64url(w[1])
                if d != self.draft:
                    self.draft = d
                    if d:
                        self.pool.submit(self._draft_job, d)
                    else:
                        self.draft_laid, self.dirty = None, True
            elif v == "me" and len(w) >= 2:
                me = b64url(w[1])
                if me != self.me:
                    self.me = me
                    self.laid = {}
                    self.relayout()
            elif v == "mode" and len(w) >= 2 and w[1] in ("chat", "emotes", "gifs"):
                if w[1] != self.mode:
                    self.mode, self.scroll, self.dirty = w[1], 0, True
                    if w[1] == "gifs":
                        self.gif_q, self.gifs, self.gif_next, self.gif_asked = "", [], None, None
            elif v == "search" and len(w) >= 2:
                self.search = b64url(w[1])
                self.scroll = 0
                if self.mode == "gifs":
                    self.gif_q, self.gifs, self.gif_next, self.gif_asked = self.search, [], None, None
                self.dirty = True
            elif v == "scroll" and len(w) >= 2:
                step = self.line_h * 3 if self.mode == "chat" else round(60 * self.sc)
                self.scroll += int(w[1]) * step if self.mode == "chat" else -int(w[1]) * step
                self.scroll = max(0, min(self.scroll, max(0, self.content_h - (self.view_h or self.h))))
                self.dirty = True
            elif v == "click" and len(w) >= 3:
                x, y = int(w[1]), int(w[2])
                for x0, y0, x1, y1, kind, val in self.hits:
                    if x0 <= x < x1 and y0 <= y < y1 and y < (self.view_h or self.h):
                        if kind == "emote":
                            self.out("pick", self.b64(val))
                        else:
                            self.out("gifpick", val)
                        break

    # --- laying out
    def _media(self, url, w, h):
        """An image or GIF at url, scaled to fit w x h (animated frames kept); cached; None while loading/failed."""
        key = (url, w, h)
        with self.lock:
            if key in self.media:
                return self.media[key]
            self.media[key] = None
        def load():
            em = None
            try:
                im = Image.open(io.BytesIO(http_get(url, timeout=12)))
                n = min(getattr(im, "n_frames", 1), 80)
                frames, durs = [], []
                for i in range(n):
                    im.seek(i)
                    f = im.convert("RGBA")
                    k = min(w / f.width, h / f.height)
                    frames.append(f.resize((max(1, round(f.width * k)), max(1, round(f.height * k))), Image.BILINEAR))
                    durs.append(max(20, int(im.info.get("duration", 100) or 100)))
                em = Emote(frames, durs)
            except Exception as ex:
                log("lobby chat image", url, ex)
            with self.lock:
                if key in self.media:
                    self.media[key] = em
                    self.dirty = True
                    if self.mode == "chat":
                        self.relayout(lambda m: m.get("gif") == url)
        self.pool.submit(load)
        return None

    def _layout_job(self, m):
        try:
            img = self._layout(m)
        except Exception as ex:
            log("lobby chat layout:", ex)
            return
        with self.lock:
            self.laid[m["id"]] = img
            self.dirty = True

    def _draft_job(self, text):
        try:
            laid = self._layout({"id": "draft", "sid": None, "name": "", "text": text, "kind": "draft", "gif": ""})
            has_emote = any(self.emotes.codes.get(wd) for wd in text.split())
        except Exception as ex:
            log("lobby chat draft:", ex)
            return
        with self.lock:
            if text == self.draft:
                self.draft_laid = (laid, has_emote)
                self.dirty = True

    def mentions_me(self, m):
        me = self.me.strip().lower()
        return bool(me) and m["kind"] in ("web", "game") and ("@" + me) in (m["text"] or "").lower()

    def _layout(self, m):
        w, pad, txt = self.w or 300, self.pad, self.txt
        maxw = w - 2 * pad
        if m["kind"] == "sys":
            pieces = [("text", m["text"], txt.font, LC_FAINT)]
        else:
            pieces = [] if m["kind"] == "draft" else \
                [("text", m["name"] or "?", txt.bold, web_name_color(m["sid"] or m["name"])), ("text", "  ", txt.font, LC_SUB)]
            for word in re.split(r"(\s+)", m["text"] or ""):
                if not word:
                    continue
                url = self.emotes.codes.get(word) if word.strip() else None
                em = self.emotes.fetch(url).result() if url else None
                if em:
                    pieces.append(("emote", em))
                elif word.startswith("@") and len(word) > 1:   # a mention
                    pieces.append(("text", word, txt.bold, LC_ACCENT))
                else:
                    pieces.append(("text", word, txt.font, LC_TEXT))
        lines, line, x = [], [], 0
        probe = ImageDraw.Draw(Image.new("RGB", (1, 1)))

        def tw(s, font):
            return int(probe.textlength(s, font=font))

        def push(item, iw):
            nonlocal line, x
            if x + iw > maxw and line:
                lines.append(line)
                line, x = [], 0
                if item[0] == "text" and not item[1].strip():
                    return
            line.append((x, item))
            x += iw

        for p in pieces:
            if p[0] == "emote":
                push(p, p[1].w + 2)
                continue
            _, s_, font, rgb = p
            buf = ""
            for ch in s_:
                if txt.is_emoji(ch) and txt.emoji(ch) is not None:
                    if buf:
                        push(("text", buf, font, rgb), tw(buf, font))
                        buf = ""
                    push(("emoji", txt.emoji(ch)), txt.emoji(ch).width + 1)
                else:
                    buf += ch
            if buf:
                bw = tw(buf, font)
                if bw > maxw:
                    for ch in buf:
                        push(("text", ch, font, rgb), tw(ch, font))
                else:
                    push(("text", buf, font, rgb), bw)
        if line:
            lines.append(line)
        lh = max(self.line_h, self.emote_h + 2)
        gif = self._media(m["gif"], maxw, self.gif_h) if m.get("gif") else None
        gif_h = (gif.frames[0].height + self.gap) if gif else (self.gif_h // 3 if m.get("gif") else 0)
        img = Image.new("RGBA", (w, max(1, len(lines)) * lh + gif_h), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        if self.mentions_me(m):   # this player is mentioned: a tinted row with a blue bar, like the website
            d.rectangle((0, 0, w - 1, img.height - 1), fill=(22, 30, 44, 255))
            d.rectangle((0, 0, max(2, round(3 * self.sc)) - 1, img.height - 1), fill=LC_ACCENT + (255,))
        anim = []
        for li, ln in enumerate(lines):
            y = li * lh
            for x0, item in ln:
                if item[0] == "text":
                    d.text((pad + x0, y + (lh - self.px) // 2 - 1), item[1], font=item[2], fill=item[3])
                elif item[0] == "emoji":
                    img.alpha_composite(item[1], (pad + x0, y + (lh - item[1].height) // 2))
                else:
                    em, ey = item[1], y + (lh - self.emote_h) // 2
                    if em.animated:
                        anim.append((pad + x0, ey, em))
                    else:
                        img.alpha_composite(em.frames[0], (pad + x0, ey))
        if gif:
            gy = max(1, len(lines)) * lh
            if gif.animated:
                anim.append((pad, gy, gif))
            else:
                img.alpha_composite(gif.frames[0], (pad, gy))
        return img, anim

    # --- drawing
    def compose(self, t_ms):
        W, H = self.w, self.h
        panel = Image.new("RGB", (W, H), LC_BG)
        anim = False
        with self.lock:
            mode = self.mode
            laid = self.draft_laid if self.draft else None
        # the message being written, at the bottom: in a picker (the message box is out of sight there) or when
        # it has emotes (so they're seen before it's sent)
        strip = None
        if laid and (mode != "chat" or laid[1]):
            img, moving = laid[0]
            sp = max(4, round(6 * self.sc))
            sh = min(img.height, H * 2 // 5)
            strip = (img, moving, sp, sh)
            self.view_h = H - sh - 2 * sp - 1
        else:
            self.view_h = H
        if mode == "chat":
            anim = self._chat(panel, t_ms)
        elif mode == "emotes":
            self._emotes(panel)
        else:
            anim = self._gifs(panel, t_ms)
        if strip:
            img, moving, sp, sh = strip
            y0 = self.view_h
            d = ImageDraw.Draw(panel)
            d.rectangle((0, y0, W - 1, H - 1), fill=LC_ROW)
            d.line((0, y0, W - 1, y0), fill=(54, 54, 54))
            top = y0 + 1 + sp - (img.height - sh)   # a long draft: its last lines
            panel.paste(img.crop((0, img.height - sh, W, img.height)), (0, y0 + 1 + sp), img.crop((0, img.height - sh, W, img.height)))
            for x, ey, em in moving:
                if top + ey >= y0 + 1:
                    fr = em.frame_at(t_ms)
                    panel.paste(fr, (x, top + ey), fr)
                    anim = True
        return panel, anim

    def _chat(self, panel, t_ms):
        H, pad = self.view_h or self.h, self.pad
        with self.lock:
            items = [self.laid.get(m["id"]) for m in self.msgs]
            scroll = self.scroll
        total = sum(it[0].height + self.gap for it in items if it)
        self.content_h = total + 2 * pad
        y = H - pad + scroll
        anim = False
        for it in reversed(items):
            if not it:
                continue
            img, moving = it
            y -= img.height
            if y + img.height < 0:
                break
            if y < H:
                panel.paste(img, (0, y), img)
                for x, ey, em in moving:
                    fr = em.frame_at(t_ms)
                    panel.paste(fr, (x, y + ey), fr)
                    anim = True
            y -= self.gap
        if not any(items):
            ImageDraw.Draw(panel).text((pad, pad), "No messages yet", font=self.txt.font, fill=LC_FAINT)
        return anim

    def _emotes(self, panel):
        W, H, pad, sc = self.w, self.view_h or self.h, self.pad, self.sc
        d = ImageDraw.Draw(panel)
        cw, ch = max(20, round(40 * sc)), max(18, round(36 * sc))
        cols = max(1, (W - 2 * pad) // cw)
        q = self.search.lower()
        e = self.emotes
        groups = [("CHANNEL", sorted(e.chan_codes)), ("GLOBAL", sorted(k for k in e.global_codes if k not in e.chan_codes))]
        hits, y = [], pad - self.scroll
        label_h = round(22 * sc)
        for label, names in groups:
            names = [n for n in names if not q or q in n.lower()]
            if not names:
                continue
            if -label_h < y < H:
                d.text((pad, y + 2), label, font=self.small, fill=LC_SUB)
            y += label_h
            for i, n in enumerate(names):
                cx, cy = pad + (i % cols) * cw, y + (i // cols) * ch
                if cy + ch < 0 or cy > H:
                    continue
                fut = e.fetch_thumb(e.codes.get(n) or (e.chan_codes.get(n) or e.global_codes.get(n)))
                if fut.done() and fut.result():
                    fr = fut.result()
                    k = min((cw - 4) / fr.width, (ch - 4) / fr.height, 1.0)
                    if k < 1:
                        fr = fr.resize((max(1, round(fr.width * k)), max(1, round(fr.height * k))), Image.BILINEAR)
                    panel.paste(fr, (cx + (cw - fr.width) // 2, cy + (ch - fr.height) // 2), fr)
                elif not fut.done():
                    fut.add_done_callback(lambda f: setattr(self, "dirty", True))
                hits.append((cx, cy, cx + cw, cy + ch, "emote", n))
            y += -(-len(names) // cols) * ch + round(8 * sc)
        if not hits and y <= pad - self.scroll:
            d.text((pad, pad), "No emotes" if e.global_codes else "Loading emotes...", font=self.txt.font, fill=LC_FAINT)
        self.content_h = y + self.scroll + pad
        with self.lock:
            self.hits = hits

    def _gifs(self, panel, t_ms):
        W, H, pad = self.w, self.view_h or self.h, self.pad
        gap = self.gap
        colw = (W - 2 * pad - gap) // 2
        tall = [0, 0]
        hits, anim = [], False
        with self.lock:
            gifs = list(self.gifs)
            nxt, asked, q = self.gif_next, self.gif_asked, self.gif_q
        for gid, gw, gh, url in gifs:
            i = 0 if tall[0] <= tall[1] else 1
            h = max(1, round(colw * gh / gw))
            x, y = pad + i * (colw + gap), pad + tall[i] - self.scroll
            tall[i] += h + gap
            if y + h < 0 or y > H:
                continue
            em = self._media(url, colw, h)
            if em:
                fr = em.frame_at(t_ms)
                panel.paste(fr, (x, y), fr)
                anim = anim or em.animated
            else:
                ImageDraw.Draw(panel).rectangle((x, y, x + colw - 1, y + h - 1), fill=LC_ROW)
            hits.append((x, y, x + colw, y + h, "gif", gid))
        self.content_h = max(tall) + 2 * pad
        if not gifs:
            ImageDraw.Draw(panel).text((pad, pad), "Loading..." if nxt is None and q is not None else "None",
                                       font=self.txt.font, fill=LC_FAINT)
        # near the end: the next page (once per page)
        if nxt is not None and asked != nxt and self.content_h - self.scroll - H < 400 * self.sc:
            with self.lock:
                self.gif_asked = nxt
            self.out("gifmore", self.b64(q or ""), nxt)
        with self.lock:
            self.hits = hits
        return anim

    def run(self):
        last = 0
        anim = False
        while True:
            t = time.monotonic()
            with self.lock:
                go = self.shown and self.w and self.h and (self.dirty or (anim and t - last >= 1 / 20))
                if go:
                    self.dirty = False
            if go:
                try:
                    panel, anim = self.compose(t * 1000)
                    if self.shm is None:
                        self.shm = LChatShm()
                    self.shm.publish(self.w, self.h, panel.convert("RGBA").tobytes())   # RGBA: the game hands it to VGUI as is
                except Exception as ex:
                    log("lobby chat draw:", ex)
                last = t
            elif self.shm is not None:
                self.shm.beat()
            time.sleep(0.02)


# ------------------------------------------------------------------ info card
_card_text = None


def rich_width(d, s, font, size):
    return sum(size if Text.is_emoji(ch) else d.textlength(ch, font=font) for ch in s)


def draw_rich(img, d, xy, s, font, fill, size):
    """Draw text, using the color emoji font for emoji characters (plain fonts show them as boxes)."""
    global _card_text
    if _card_text is None:
        _card_text = Text()
    x, y = xy
    run = ""
    for ch in s + "\0":
        if ch != "\0" and not Text.is_emoji(ch):
            run += ch
            continue
        if run:
            d.text((x, y), run, font=font, fill=fill)
            x += d.textlength(run, font=font)
            run = ""
        if ch != "\0":
            e = _card_text.emoji(ch)
            if e is not None:
                e = e.resize((max(1, round(e.width * size / e.height)), size), Image.LANCZOS)
                img.alpha_composite(e, (int(x), int(y) + 3))
                x += e.width + 2


def fmt_uptime(created):
    from datetime import datetime, timezone
    start = datetime.fromisoformat(created.replace("Z", "+00:00"))
    s = int((datetime.now(timezone.utc) - start).total_seconds())
    return f"{s // 3600}:{s % 3600 // 60:02d}:{s % 60:02d}"


def render_card(login):
    q = {"query": 'query($l:String!){user(login:$l){displayName profileImageURL(width:150) '
                  'stream{title viewersCount createdAt game{displayName}}}}', "variables": {"l": login}}
    d = json.loads(http_get("https://gql.twitch.tv/gql", data=json.dumps(q).encode(),
                            headers={"Client-Id": GQL_CLIENT_ID, "Content-Type": "application/json"}))
    user = (d.get("data") or {}).get("user") or {}
    stream = user.get("stream")
    font = ImageFont.truetype(FONT, S(24))
    small = ImageFont.truetype(FONT, S(20))
    bold = ImageFont.truetype(FONT_BOLD, S(30))
    img = Image.new("RGBA", (CARD_W, CARD_H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, CARD_W - 1, CARD_H - 1), radius=S(14), fill=(18, 18, 22, 235))
    d.rounded_rectangle((0, 0, S(8), CARD_H - 1), radius=S(4), fill=PURPLE + (255,))
    x = S(28)
    try:
        pic = Image.open(io.BytesIO(http_get(user["profileImageURL"]))).convert("RGBA").resize((S(96), S(96)), Image.LANCZOS)
        mask = Image.new("L", (S(96), S(96)), 0)
        ImageDraw.Draw(mask).ellipse((0, 0, S(96) - 1, S(96) - 1), fill=255)
        img.paste(pic, (x, S(24)), mask)
        x += S(116)
    except Exception:
        pass
    name = user.get("displayName") or login
    d.text((x, S(18)), name, font=bold, fill=TEXT)
    nx = x + d.textlength(name, font=bold) + S(14)
    if stream:
        d.rounded_rectangle((nx, S(24), nx + S(64), S(50)), radius=S(5), fill=(235, 4, 0, 255))
        d.text((nx + S(9), S(24)), "LIVE", font=small, fill=(255, 255, 255))
        d.text((x, S(62)), f"{stream['viewersCount']:,} viewers   ·   live for {fmt_uptime(stream['createdAt'])}", font=font, fill=TEXT)
        game = (stream.get("game") or {}).get("displayName") or ""
        d.text((x, S(96)), game, font=font, fill=(191, 148, 255))
        title = INVISIBLE.sub("", stream.get("title") or "")
        maxw = CARD_W - x - S(24)
        # two lines of title
        words, l1 = title.split(" "), ""
        while words and rich_width(d, l1 + words[0] + " ", small, S(20)) <= maxw:
            l1 += words.pop(0) + " "
        draw_rich(img, d, (x, S(134)), l1.strip(), small, MUTED, S(20))
        l2 = " ".join(words)
        while l2 and rich_width(d, l2, small, S(20)) > maxw:
            l2 = l2[:-4] + "..."
        draw_rich(img, d, (x, S(160)), l2, small, MUTED, S(20))
    else:
        d.text((x, S(70)), "offline", font=font, fill=MUTED)
    return img


# ------------------------------------------------------------------ entry points
def stdin_loop(r):
    lobby = None
    for line in sys.stdin:
        try:
            c = json.loads(line)
        except Exception:
            continue
        if c.get("cmd") in ("hub", "lchat_ui"):
            if lobby is None:
                lobby = LobbyChat()
            try:
                (lobby.on_hub if c["cmd"] == "hub" else lobby.on_ui)(str(c.get("line") or ""))
            except Exception as ex:
                log("lobby chat:", ex)
            continue
        if c.get("cmd") == "channel":
            r.set_channel(c.get("login"))
        elif c.get("cmd") == "youtube":
            r.set_youtube(c.get("video"), c.get("url"))
        elif c.get("cmd") == "info" and c.get("login"):
            threading.Thread(target=r.show_info, args=(c["login"],), daemon=True).start()
        elif c.get("cmd") == "quit":
            os._exit(0)
    os._exit(0)   # cstvd went away


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--test":
        login, secs, prefix = sys.argv[2], float(sys.argv[3]), sys.argv[4]
        r = Renderer(chat_shm=False)
        if login.startswith("yt:"):
            r.set_youtube(login[3:])
        else:
            r.set_channel(login)
        t0 = time.time()
        while time.time() - t0 < secs:
            time.sleep(0.5)
        panel, anim = r.compose(time.monotonic() * 1000)
        panel.save(prefix + "_chat.png")
        if login.startswith("yt:"):
            log(f"test: {len(r.msgs)} messages, animated on screen: {anim}, note: {r.note}")
            return
        card = render_card(login)
        bg = Image.new("RGBA", (CARD_W + 40, CARD_H + 40), (60, 90, 60, 255))
        bg.alpha_composite(card, (20, 20))
        bg.save(prefix + "_card.png")
        log(f"test: {len(r.msgs)} messages, {len(r.emotes.codes)} emote codes, animated on screen: {anim}")
        return
    r = Renderer()
    threading.Thread(target=stdin_loop, args=(r,), daemon=True).start()
    r.run()


if __name__ == "__main__":
    main()
