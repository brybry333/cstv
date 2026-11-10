# The hub against fake games (the /game line protocol, as cstv_link.py speaks it for cstvd) and fake browsers
# (/ws JSON). Ported from daf's test_master.py where the behaviour is the same.
#   python3 -m unittest test_hub        (needs `websockets`)
# Every check reads up to a ping/pong (or time/time) barrier, so nothing depends on timing or leftover lines.
import asyncio
import datetime
import json
import os
import re
import tempfile
import time
import unittest

os.environ["CSTV_HUB_STREAM_HOST"] = "127.0.0.1"
import hub as m   # noqa: E402
from websockets.asyncio.client import connect   # noqa: E402
from websockets.asyncio.server import serve   # noqa: E402

GEO = "0 0 64  -1 0 0  0 1 0  0 0 1"
SRV = "1.2.3.4:27015"
A_SID, B_SID, C_SID = "76561190000000001", "76561190000000002", "76561190000000003"


async def no_title(url):
    return None


m.fetch_title = no_title   # no network in tests: every title is asked of a game

YT_PAGE = ('<script>var ytInitialData = %s;</script><script>ytcfg.set({"INNERTUBE_CONTEXT_CLIENT_VERSION":"2.20261019"});</script>')
YT_NOCHAT = YT_PAGE % json.dumps({"contents": {"messageRenderer": {"text": {"runs": [{"text": "Chat is disabled for this live stream."}]}}}})


def yt_nochat(path, body=None):
    return YT_NOCHAT


m.yt_http = yt_nochat   # no network: YouTube (the site's chat panel) says "no chat" unless a test stands in for it
m.YTChat.POLL = 0.05


async def http_get(port, path):
    """A plain GET to the hub (what a browser or hls.js does) -> (status, body)."""
    r, w = await asyncio.open_connection("127.0.0.1", port)
    w.write(f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n".encode())
    data = await r.read()
    w.close()
    head, _, body = data.partition(b"\r\n\r\n")
    return int(head.split()[1]), body


class Game:
    seq = 0

    def __init__(self, name, steamid, token=None):
        self.name, self.steamid, self.token = name, steamid, token
        self.lines = asyncio.Queue()

    async def connect(self, port, where=None):
        self.ws = await connect(f"ws://127.0.0.1:{port}/game")
        self.task = asyncio.create_task(self.pump())
        if self.token:
            self.send(f"auth {self.token}")
        self.send(f"hello {m.PROTO} {self.steamid} {m.b64e(self.name)}")
        got = await self.sync()
        self.id = int(first(got, "welcome")[2])
        self.you = first(got, "you")
        if where:
            self.send(f"where {where}")
            await self.sync()

    async def pump(self):
        async for msg in self.ws:
            for line in msg.split("\n"):
                await self.lines.put(line.strip())

    def send(self, line):
        asyncio.ensure_future(self.ws.send(line))

    async def sync(self):
        """Everything the hub sent this game up to now."""
        await asyncio.sleep(0.01)   # let title lookups and other small tasks run first
        Game.seq += 1
        token = f"pong {Game.seq}"
        self.send(f"ping {Game.seq}")
        got = []
        while (line := await asyncio.wait_for(self.lines.get(), 2)) != token:
            got.append(line)
        return got

    async def close(self):
        await self.ws.close()
        self.task.cancel()


class Web:
    def __init__(self, token=None):
        self.token = token
        self.msgs = asyncio.Queue()
        self.n = 0

    async def connect(self, port):
        hdr = {"Origin": m.ORIGIN}
        if self.token:
            hdr["Cookie"] = "cstv_session=" + self.token
        self.ws = await connect(f"ws://127.0.0.1:{port}/ws", additional_headers=hdr)
        self.task = asyncio.create_task(self.pump())
        self.send({"t": "hello", "kind": "web"})
        self.welcome = first_t(await self.sync(), "welcome")

    async def pump(self):
        async for msg in self.ws:
            await self.msgs.put(json.loads(msg))

    def send(self, obj):
        asyncio.ensure_future(self.ws.send(json.dumps(obj)))

    async def sync(self):
        await asyncio.sleep(0.01)
        self.n += 1
        self.send({"t": "time", "c": self.n})
        got = []
        while True:
            x = await asyncio.wait_for(self.msgs.get(), 2)
            if x.get("t") == "time" and x.get("c") == self.n:
                return got
            got.append(x)

    async def close(self):
        await self.ws.close()
        self.task.cancel()


async def sync(*clients):
    # the one that acted goes first: once its pong is back, the hub has sent the others everything its line caused
    return [await c.sync() for c in clients]


def all_of(lines, verb):
    return [l.split() for l in lines if l and l.split()[0] == verb]


def first(lines, verb):
    found = all_of(lines, verb)
    if not found:
        raise AssertionError(f"no {verb} in {lines}")
    return found[0]


def last(lines, verb):
    found = all_of(lines, verb)
    if not found:
        raise AssertionError(f"no {verb} in {lines}")
    return found[-1]


def first_t(msgs, t):
    found = [x for x in msgs if x.get("t") == t]
    if not found:
        raise AssertionError(f"no {t} in {msgs}")
    return found[0]


def msgs(lines):
    return [m.b64d(w[1]) for w in all_of(lines, "msg")]


def roster(lines):
    w = last(lines, "roster")
    return w[1], [tuple(e.split(",")) for e in w[2:]]


def queue(lines):
    return [(int(u), m.b64d(url), m.b64d(t), m.b64d(by))
            for u, url, t, by in (e.split(",") for e in last(lines, "queue")[1:])]


async def post_path(port, path):
    """A bodyless POST (mediamtx's runOnReady / runOnNotReady hooks); returns the status code."""
    r, w = await asyncio.open_connection("127.0.0.1", port)
    w.write(f"POST {path} HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n".encode())
    status = await r.readline()
    w.close()
    return int(status.split()[1])


async def post(port, obj):
    r, w = await asyncio.open_connection("127.0.0.1", port)
    body = json.dumps(obj).encode()
    w.write(b"POST /auth HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
            b"Content-Length: %d\r\n\r\n" % len(body) + body)
    status = await r.readline()
    w.close()
    return int(status.split()[1])


class HubTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        m.db = m.DB(os.path.join(self.tmp.name, "hub.db"))
        for x in (m.conns, m.lobbies, m.srv_rooms, m.pair_codes, m.hls_tokens, m.yt_chats):
            x.clear()
        m.yt_http = yt_nochat
        for sid, name in ((A_SID, "alice"), (B_SID, "bob"), (C_SID, "carol")):
            m.db.x("INSERT INTO users VALUES(?,?,?,?)", sid, name, None, time.time())
        self.tok = {sid: m.new_session(sid, "game") for sid in (A_SID, B_SID, C_SID)}
        self.server = await serve(m.ws_handler, "127.0.0.1", 0, process_request=m.http_handler, max_size=1 << 16)
        self.port = self.server.sockets[0].getsockname()[1]
        self.http = await asyncio.start_server(m.on_auth_http, "127.0.0.1", 0)
        self.http_port = self.http.sockets[0].getsockname()[1]
        self.clients = []
        self.a = await self.game("alice", A_SID, signed=True)
        self.b = await self.game("bob", B_SID, signed=True)

    async def game(self, name, steamid, signed=False, where=f"{SRV} bhop_test", token=None):
        g = Game(name, steamid, token or (self.tok.get(steamid) if signed else None))
        await g.connect(self.port, where)
        self.clients.append(g)
        return g

    async def web(self, steamid=None):
        w = Web(m.new_session(steamid, "web") if steamid else None)
        await w.connect(self.port)
        self.clients.append(w)
        return w

    async def asyncTearDown(self):
        for c in self.clients:
            try:
                await c.close()
            except Exception:
                pass
        self.server.close()
        self.http.close()
        await asyncio.sleep(0.25)   # the handlers' last sends
        m.db.c.close()
        self.tmp.cleanup()

    async def lobby(self):
        """alice creates a lobby, bob joins it; returns the code."""
        self.a.send("lobby create")
        la, = await sync(self.a)
        code = first(la, "room")[2]
        self.b.send("lobby join " + code.lower())
        await sync(self.b, self.a)
        return code

    # ---- daf's master tests
    async def test_place_link_started_pause(self):
        self.a.send(f"place 0.5 0 {GEO}")
        la, lb = await sync(self.a, self.b)
        self.assertEqual(first(lb, "screen")[1:3], ["0.5", "0"])
        self.assertIn("placed a TV", msgs(lb)[0])

        self.a.send("link " + m.b64e("https://www.youtube.com/watch?v=abc"))
        la, lb = await sync(self.a, self.b)
        url = first(lb, "url")
        self.assertEqual(m.b64d(url[2]), "https://www.youtube.com/watch?v=abc")
        self.assertEqual(first(lb, "state")[6], "0")   # held until a viewer starts
        self.assertEqual(first(lb, "nowtitle")[1], "-")
        self.assertEqual(len(all_of(la, "title")) + len(all_of(lb, "title")), 1)

        self.b.send(f"started {url[1]}")
        lb, la = await sync(self.b, self.a)
        self.assertEqual(first(la, "state")[6], "1")

        self.a.send("pause")
        la, lb = await sync(self.a, self.b)
        self.assertEqual(first(lb, "state")[2], "1")
        self.assertIn("paused", msgs(lb)[0])

    async def test_link_timestamp_starts_the_clock_there(self):
        self.a.send("link " + m.b64e("https://youtu.be/u5Im4E3myoM?si=Zef6CD5YSksg2awY&t=715"))
        la, lb = await sync(self.a, self.b)
        self.assertEqual(first(lb, "state")[3], "715000")   # not 0: the first sync pulled players back to the start
        for u, ms in (("https://www.youtube.com/watch?v=x&t=11m55s", 715000), ("https://youtube.com/watch?v=x&t=1h2m3s", 3723000),
                      ("https://www.youtube.com/embed/x?start=90", 90000), ("https://youtu.be/x#t=42", 42000),
                      ("https://www.twitch.tv/videos/1?t=01h02m03s", 3723000), ("https://www.twitch.tv/chan?t=5m", 0),
                      ("https://youtu.be/x?t=abc", 0), ("https://youtu.be/x", 0), ("https://vimeo.com/1?t=30", 0)):
            self.assertEqual(m.link_start(u), ms, u)

    async def test_scale_range(self):
        for scale, ok in (("0.05", False), ("0.1", True), ("0.25", True), ("2.5", True), ("4", True), ("4.5", False), ("nan", False)):
            self.a.send(f"place {scale} 1 {GEO}")
            la, lb = await sync(self.a, self.b)
            self.assertEqual(bool(all_of(lb, "screen")), ok, scale)
            if ok:
                self.assertEqual(first(lb, "screen")[1], f"{float(scale):g}")

    async def test_screens_survive_a_restart(self):
        self.a.send(f"place 2 0 {GEO}")
        await sync(self.a, self.b)
        m.srv_rooms.clear()   # as after a restart
        c = await self.game("carol", C_SID)
        lc, = await sync(c)
        await asyncio.sleep(0)
        c.send(f"where {SRV} de_dust2")
        c.send(f"where {SRV} bhop_test")
        lc, = await sync(c)
        self.assertEqual(last(lc, "screen")[1], "2")

    async def test_queue_lines_and_ended(self):
        self.a.send("link " + m.b64e("https://example.com/one"))
        la, lb = await sync(self.a, self.b)
        rev = first(lb, "url")[1]
        self.b.send("queue " + m.b64e("https://example.com/two"))
        self.b.send("queue " + m.b64e("https://example.com/three"))
        lb, la = await sync(self.b, self.a)
        q = queue(la)
        self.assertEqual([(i[1], i[2], i[3]) for i in q],
                         [("https://example.com/two", "", "bob"), ("https://example.com/three", "", "bob")])
        self.assertEqual(m.b64d(first(la, "upnext")[1]), "https://example.com/two")
        two, three = q[0][0], q[1][0]

        # title lookups land in the queue line
        asked = all_of(la + lb, "title")
        self.assertEqual({int(w[1]) for w in asked}, {two, three})
        self.a.send(f"title {two} {m.b64e('Video Two')}")
        la, lb = await sync(self.a, self.b)
        self.assertEqual(queue(lb)[0][2], "Video Two")

        # both viewers report the end; only the first one counts
        self.a.send(f"ended {rev} - -")
        self.b.send(f"ended {rev} - -")
        la, lb = await sync(self.a, self.b)
        url = first(lb, "url")
        self.assertEqual(m.b64d(url[2]), "https://example.com/two")
        self.assertEqual(int(url[1]), int(rev) + 1)
        self.assertEqual(m.b64d(first(lb, "nowtitle")[1]), "Video Two")
        self.assertEqual([i[0] for i in queue(lb)], [three])

        self.a.send("queue " + m.b64e("https://example.com/four"))
        la, lb = await sync(self.a, self.b)
        four = queue(lb)[1][0]
        self.b.send(f"unqueue {three}")
        lb, la = await sync(self.b, self.a)
        self.assertEqual([i[0] for i in queue(la)], [four])
        self.b.send(f"playnow {four}")
        lb, la = await sync(self.b, self.a)
        self.assertEqual(m.b64d(first(la, "url")[2]), "https://example.com/four")
        self.assertEqual(queue(la), [])

        self.a.send("prev")
        la, lb = await sync(self.a, self.b)
        self.assertEqual(m.b64d(first(lb, "url")[2]), "https://example.com/two")
        self.assertEqual([i[1] for i in queue(lb)], ["https://example.com/four"])

    async def test_last_video_ends_stops(self):
        self.a.send("link " + m.b64e("https://example.com/one"))
        la, lb = await sync(self.a, self.b)
        rev = first(lb, "url")[1]
        self.a.send(f"ended {rev} - -")
        la, lb = await sync(self.a, self.b)
        self.assertTrue(all_of(lb, "stop"))

    async def test_private_links_refused(self):
        for u in ("http://127.0.0.1/x", "http://192.168.1.1/", "http://[::1]/", "http://localhost:8080/",
                  "file:///etc/passwd", "rtmp://127.0.0.1:1935/live/abc"):
            self.a.send("link " + m.b64e(u))
            la, = await sync(self.a)
            self.assertIn("doesn't look like", msgs(la)[0])

    async def test_seek_and_rate_limit(self):
        self.a.send("link " + m.b64e("https://example.com/v"))
        await sync(self.a, self.b)
        self.a.send("seek 1:30")
        la, lb = await sync(self.a, self.b)
        self.assertEqual(first(lb, "state")[3], "90000")
        for _ in range(12):
            self.a.send("pause")
        la, = await sync(self.a)
        self.assertIn("Slow down.", msgs(la))

    async def test_where_and_roster(self):
        c = await self.game("carol", C_SID, where=f"{SRV} de_dust2")
        lc, la, lb = await sync(c, self.a, self.b)
        owner, members = roster(la)
        self.assertEqual(owner, "-")
        self.assertEqual([(int(i), s, m.b64d(n), m.b64d(w)) for i, s, n, w in members],
                         [(self.a.id, A_SID, "alice", f"{SRV}/bhop_test"),
                          (self.b.id, B_SID, "bob", f"{SRV}/bhop_test"),
                          (c.id, "0", "carol", f"{SRV}/de_dust2")])   # carol isn't signed in: no proven SteamID

        # screens are per map inside the server room
        self.a.send(f"place 1 0 {GEO}")
        la, lb, lc = await sync(self.a, self.b, c)
        self.assertTrue(all_of(lb, "screen"))
        self.assertFalse(all_of(lc, "screen"))
        c.send(f"where {SRV} bhop_test")
        lc, = await sync(c)
        self.assertEqual(first(lc, "screen")[1], "1")

        c.send("where - -")
        lc, la = await sync(c, self.a)
        self.assertEqual(first(lc, "room")[:5], ["room", "srv", "-", "0", "all"])
        self.assertEqual(first(lc, "screen"), ["screen", "none"])
        self.assertEqual(len(roster(la)[1]), 2)

        c.send("where 5.6.7.8:27015 surf_x")
        lc, la = await sync(c, self.a)
        self.assertEqual(first(lc, "room")[2], "5.6.7.8:27015")
        self.assertEqual([int(e[0]) for e in roster(lc)[1]], [c.id])
        self.assertFalse(all_of(la, "roster"))

    async def test_lobby_kick_ban(self):
        self.a.send("lobby create")
        la, lb = await sync(self.a, self.b)
        room = first(la, "room")
        self.assertEqual(room[1], "lobby")
        self.assertRegex(room[2], r"^[A-HJ-NP-Z2-9]{6}$")
        self.assertEqual(room[3:5], ["1", "all"])
        self.assertEqual([int(e[0]) for e in roster(lb)[1]], [self.b.id])   # alice left the server room
        code = room[2]

        self.b.send("lobby join " + code.lower())
        lb, la = await sync(self.b, self.a)
        self.assertEqual(first(lb, "room")[:5], ["room", "lobby", code, "0", "all"])
        for lines in (la, lb):
            owner, members = roster(lines)
            self.assertEqual(owner, str(self.a.id))
            self.assertEqual([int(e[0]) for e in members], [self.a.id, self.b.id])

        # only the owner (and admins) kick
        self.b.send(f"lobby kick {self.a.id}")
        lb, = await sync(self.b)
        self.assertIn("Only the lobby's owner and admins", msgs(lb)[0])

        self.a.send(f"lobby kick {self.b.id}")
        la, lb = await sync(self.a, self.b)
        self.assertIn("Kicked", m.b64d(first(lb, "kicked")[1]))
        self.assertEqual(first(lb, "room")[1:3], ["srv", SRV])
        self.assertEqual([int(e[0]) for e in roster(la)[1]], [self.a.id])

        self.b.send("lobby join " + code)
        await sync(self.b, self.a)
        self.a.send(f"lobby ban {self.b.id}")
        la, lb = await sync(self.a, self.b)
        self.assertIn("Banned", m.b64d(first(lb, "kicked")[1]))
        self.b.send("lobby join " + code)
        lb, = await sync(self.b)
        self.assertIn("banned", msgs(lb)[0])
        self.assertFalse(all_of(lb, "room"))

        # a guest (not signed in) is banned by IP
        c = await self.game("carol", C_SID)
        c.send("lobby join " + code)
        lc, la = await sync(c, self.a)
        self.assertEqual(first(lc, "room")[1:3], ["lobby", code])
        self.a.send(f"lobby ban {c.id}")
        await sync(self.a, c)
        guest2 = await self.game("dave", "76561190000000004")
        guest2.send("lobby join " + code)
        lg, = await sync(guest2)
        self.assertIn("banned", msgs(lg)[0])

        self.a.send(f"lobby unban {B_SID}")
        await sync(self.a)
        self.b.send("lobby join " + code)
        lb, = await sync(self.b)
        self.assertEqual(first(lb, "room")[1:3], ["lobby", code])

        self.b.send("lobby join ZZZZZZ")
        lb, = await sync(self.b)
        self.assertIn("no lobby", msgs(lb)[0].lower())

    async def test_lobby_name_password(self):
        self.a.send("lobby create - " + m.b64e("hunter2"))
        la, lb = await sync(self.a, self.b)
        room = first(la, "room")
        code = room[2]
        self.assertEqual(m.b64d(room[5]), "alice's lobby")   # "-" = default name
        self.assertEqual(room[6:], ["1", m.b64e("hunter2")])  # the owner sees the password

        self.b.send("lobby join " + code)
        lb, = await sync(self.b)
        self.assertEqual(first(lb, "joinfail")[1:], [code, "password"])
        self.b.send(f"lobby join {code} {m.b64e('nope')}")
        lb, = await sync(self.b)
        self.assertEqual(first(lb, "joinfail")[2], "wrong")
        self.b.send(f"lobby join {code} {m.b64e('hunter2')}")
        lb, la = await sync(self.b, self.a)
        room = first(lb, "room")
        self.assertEqual(room[1:3], ["lobby", code])
        self.assertEqual(room[6:], ["1", "-"])   # members only see that there is one

        # bob got in with this password before: no need again (until it changes)
        self.b.send("lobby leave")
        await sync(self.b)
        self.b.send("lobby join " + code)
        lb, = await sync(self.b)
        self.assertEqual(first(lb, "room")[1:3], ["lobby", code])

        self.b.send("lobby name " + m.b64e("not yours"))
        lb, = await sync(self.b)
        self.assertIn("owner and admins", msgs(lb)[0])
        self.a.send("lobby name " + m.b64e("movie night\x07"))
        la, lb = await sync(self.a, self.b)
        self.assertEqual(m.b64d(last(lb, "room")[5]), "movie night")
        self.a.send("lobby password -")
        la, lb = await sync(self.a, self.b)
        self.assertEqual(last(lb, "room")[6], "0")

        # five wrong guesses lock that IP out, even once the password is right again
        self.a.send("lobby password " + m.b64e("pw"))
        await sync(self.a)
        c = await self.game("carol", C_SID)
        for _ in range(m.PASS_TRIES):
            c.send(f"lobby join {code} {m.b64e('x')}")
            await sync(c)   # one at a time: the rate limit would eat some
            await asyncio.sleep(0)
        c.bucket = m.BUCKET_MAX
        conn = next(x for x in m.conns if x.id == c.id)
        conn.bucket = m.BUCKET_MAX
        c.send(f"lobby join {code} {m.b64e('pw')}")
        lc, = await sync(c)
        self.assertEqual(first(lc, "joinfail")[2], "locked")

    async def test_lobby_needs_sign_in(self):
        c = await self.game("carol", C_SID)
        c.send("lobby create")
        lc, = await sync(c)
        self.assertIn("!tvlogin", msgs(lc)[0])
        self.assertFalse(all_of(lc, "room"))
        code = await self.lobby()
        c.send("lobby join " + code)
        lc, = await sync(c)
        self.assertEqual(first(lc, "room")[1:3], ["lobby", code])
        self.assertTrue(any("guest" in x for x in msgs(lc)))
        c.send("link " + m.b64e("https://example.com/c"))
        lc, = await sync(c)
        self.assertIn("!tvlogin", msgs(lc)[0])

    async def test_lobby_perm_and_owner(self):
        code = await self.lobby()
        self.a.send("lobby perm owner")
        la, lb = await sync(self.a, self.b)
        self.assertEqual(first(lb, "room")[:5], ["room", "lobby", code, "0", "owner"])
        self.b.send("link " + m.b64e("https://example.com/b"))
        lb, la = await sync(self.b, self.a)
        self.assertIn("not allowed to play links", msgs(lb)[0])
        self.assertFalse(all_of(la, "url"))
        self.a.send("link " + m.b64e("https://example.com/a"))
        la, lb = await sync(self.a, self.b)
        self.assertTrue(all_of(lb, "url"))

        self.a.send(f"lobby owner {self.b.id}")
        la, lb = await sync(self.a, self.b)
        self.assertEqual(first(lb, "room")[3], "1")
        self.assertEqual(roster(la)[0], str(self.b.id))
        self.assertEqual(m.lobbies[code].owner, B_SID)
        self.assertTrue(m.lobbies[code].perms_sid(A_SID) & m.ADMIN)   # the old owner stays an admin

    async def test_stream(self):
        code = await self.lobby()
        self.a.send("stream start")
        la, = await sync(self.a)
        go = m.b64d(first(la, "stream_go")[1])
        mt = re.fullmatch(r"rtmp://127\.0\.0\.1:1935/live/([0-9a-f]{16})\?key=([\w-]+)", go)
        self.assertTrue(mt, go)
        sid, key = mt.groups()
        path = "live/" + sid

        self.b.send("stream start")
        lb, = await sync(self.b)
        self.assertIn("Only the lobby's owner and admins can stream.", msgs(lb))
        m.lobbies[code].members[B_SID]["admin"] = True
        self.b.send("stream start")
        lb, = await sync(self.b)
        self.assertIn("already streaming", msgs(lb)[0])
        m.lobbies[code].members[B_SID]["admin"] = False

        def req(action, p=path, query=""):
            return {"user": "", "password": "", "token": "", "ip": "10.0.0.9", "action": action,
                    "path": p, "protocol": "rtmp", "id": None, "query": query, "userAgent": ""}

        self.assertEqual(await post(self.http_port, req("publish", query="key=" + key)), 200)
        self.assertEqual(await post(self.http_port, req("publish", query="key=wrong")), 403)
        self.assertEqual(await post(self.http_port, req("publish")), 403)
        self.assertEqual(await post(self.http_port, req("read")), 200)
        self.assertEqual(await post(self.http_port, req("playback")), 200)
        self.assertEqual(await post(self.http_port, req("read", "live/0123456789abcdef")), 403)
        self.assertEqual(await post(self.http_port, req("api", "")), 403)
        self.assertEqual(await post(self.http_port, req("metrics", "")), 403)

        self.a.send("stream live")   # the encoder is pushing, but the relay hasn't said "ready": nobody switches yet
        la, lb = await sync(self.a, self.b)
        self.assertFalse(all_of(lb, "url"))
        self.assertEqual(await post_path(self.http_port, "/ready/" + path), 200)
        la, lb = await sync(self.a, self.b)
        self.assertEqual(m.b64d(first(lb, "url")[2]), "rtmp://127.0.0.1:1935/live/" + sid)
        self.assertEqual(m.b64d(first(lb, "nowtitle")[1]), "alice's stream")
        self.b.send(f"ended {first(lb, 'url')[1]}")   # a viewer's player gave up: the stream goes on
        lb, la = await sync(self.b, self.a)
        self.assertFalse(all_of(la, "stream_end"))
        self.assertEqual(await post_path(self.http_port, "/ready/live/0123456789abcdef"), 403)

        self.b.send("seek +30")
        self.b.send("pause")
        lb, la = await sync(self.b, self.a)
        self.assertEqual(msgs(lb), ["A live stream can't be seeked.", "A live stream can't be paused."])
        self.assertFalse(all_of(la, "state"))

        self.a.send("stream stop")
        la, lb = await sync(self.a, self.b)
        self.assertTrue(all_of(la, "stream_end"))
        self.assertTrue(all_of(lb, "stop"))
        self.assertEqual(await post(self.http_port, req("read")), 403)

        # the streamer leaving ends it too; the stream never lands in the history
        self.a.send("stream start")
        la, = await sync(self.a)
        sid2 = re.search(r"/live/([0-9a-f]{16})", m.b64d(first(la, "stream_go")[1])).group(1)
        self.assertEqual(await post_path(self.http_port, f"/ready/live/{sid2}"), 200)   # ready first, then "live"
        self.a.send("stream live")
        await sync(self.a, self.b)
        self.a.send("lobby leave")
        la, lb = await sync(self.a, self.b)
        self.assertTrue(all_of(la, "stream_end"))
        self.assertTrue(all_of(lb, "stop"))
        self.b.send("prev")
        lb, = await sync(self.b)
        self.assertIn("Nothing to go back to.", msgs(lb))

    async def test_stream_perm(self):
        self.b.send("stream start")   # a server room: nobody
        lb, = await sync(self.b)
        self.assertFalse(all_of(lb, "stream_go"))
        self.assertIn("Streaming works in a lobby, for its owner and admins.", msgs(lb))
        code = await self.lobby()
        self.b.send("stream start")   # in the lobby, not an admin
        lb, = await sync(self.b)
        self.assertFalse(all_of(lb, "stream_go"))
        self.assertIn("Only the lobby's owner and admins can stream.", msgs(lb))

        # a link replacing a live stream ends it
        self.a.send("stream start")
        la, = await sync(self.a)
        sid = re.search(r"/live/([0-9a-f]{16})", m.b64d(first(la, "stream_go")[1])).group(1)
        await post_path(self.http_port, f"/ready/live/{sid}")
        self.a.send("stream live")
        await sync(self.a, self.b)
        self.a.send("link " + m.b64e("https://example.com/v"))
        la, lb = await sync(self.a, self.b)
        self.assertTrue(all_of(la, "stream_end"))
        self.assertIsNone(m.lobbies[code].stream)

    async def wait_t(self, w, t, pred=lambda x: True, secs=5):
        """The next message of type t (matching pred) a browser gets, however long the hub takes (threads)."""
        end = time.monotonic() + secs
        while time.monotonic() < end:
            for x in await w.sync():
                if x.get("t") == t and pred(x):
                    return x
            await asyncio.sleep(0.05)
        self.fail(f"no {t} message")

    async def test_hls_fallback(self):
        seen = []

        async def mediamtx(reader, writer):   # mediamtx's HLS server: chunked playlists
            head = (await reader.readuntil(b"\r\n\r\n")).decode()
            seen.append(head.split()[1])
            body = b"#EXTM3U\n#EXT-X-VERSION:10\n"
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\nTransfer-Encoding: chunked\r\n\r\n"
                         + b"%x\r\n" % len(body) + body + b"\r\n0\r\n\r\n")
            await writer.drain()
            writer.close()
        fake = await asyncio.start_server(mediamtx, "127.0.0.1", 0)
        old_url, m.HLS_URL = m.HLS_URL, f"http://127.0.0.1:{fake.sockets[0].getsockname()[1]}"
        try:
            code = await self.lobby()
            w = await self.web(C_SID)
            w.send({"t": "join", "code": code})
            await w.sync()
            w.send({"t": "hls"})   # nothing streaming
            self.assertIn("isn't live", first_t(await w.sync(), "hls_fail")["msg"])

            self.a.send("stream start")
            la, = await sync(self.a)
            sid = re.search(r"/live/([0-9a-f]{16})", m.b64d(first(la, "stream_go")[1])).group(1)
            await post_path(self.http_port, f"/ready/live/{sid}")
            self.a.send("stream live")
            await sync(self.a)
            w.send({"t": "hls"})
            url = first_t(await w.sync(), "hls")["url"]
            self.assertRegex(url, r"^/hls/[\w-]{30,}/index\.m3u8$")

            st, body = await http_get(self.port, url)
            self.assertEqual((st, body), (200, b"#EXTM3U\n#EXT-X-VERSION:10\n"))
            self.assertEqual(seen[-1], f"/live/{sid}/index.m3u8?cookieCheck=1")   # mediamtx then puts ?session= in its links
            uuid = "0123abcd-0000-4000-8000-0123456789ab"
            st, _ = await http_get(self.port, url.replace("index.m3u8", f"video1_stream.m3u8?session={uuid}&_HLS_msn=12&_HLS_part=3&x=1"))
            self.assertEqual(st, 200)
            self.assertEqual(seen[-1], f"/live/{sid}/video1_stream.m3u8?_HLS_msn=12&_HLS_part=3&session={uuid}")   # nothing else passes
            n = len(seen)
            self.assertEqual((await http_get(self.port, url.replace("index.m3u8", "..%2Fx")))[0], 404)
            self.assertEqual((await http_get(self.port, "/hls/made-up-token/index.m3u8"))[0], 403)
            self.assertEqual(len(seen), n)   # neither reached mediamtx

            self.a.send("stream stop")   # the stream is over: so is its link
            await sync(self.a)
            self.assertEqual((await http_get(self.port, url))[0], 403)
        finally:
            m.HLS_URL = old_url
            fake.close()

    async def test_vstats_logged(self):
        code = await self.lobby()
        w = await self.web(C_SID)
        w.send({"t": "join", "code": code})
        await w.sync()
        with self.assertLogs("cstv-hub", "INFO") as logs:
            w.send({"t": "vstats", "mode": "hls", "codec": "avc1.64002a", "ua": "Vivaldi 7.6, Chromium 141; Windows", "res": "1920x1080",
                    "fps": 59.94, "fd": 1200, "fdrop": 3, "kbps": 7900, "stalls": 1, "jit": float("nan")})
            w.send({"t": "vstats", "mode": "hls"})   # too soon after the last: dropped
            await w.sync()
        lines = [x for x in logs.output if "vstats" in x]
        self.assertEqual(len(lines), 1)
        self.assertIn(f"vstats {code} carol mode=hls codec=avc1.64002a ua=[Vivaldi 7.6 Chromium 141; Windows] res=1920x1080 fps=60", lines[0])
        self.assertIn("jitter=-ms", lines[0])

    async def test_youtube_chat_panel(self):
        vid = "LiveVideo01"
        header = {"liveChatHeaderRenderer": {"viewSelector": {"sortFilterSubMenuRenderer": {"subMenuItems": [
            {"continuation": {"reloadContinuationData": {"continuation": "top"}}},
            {"continuation": {"reloadContinuationData": {"continuation": "c1"}}}]}}}}
        live = YT_PAGE % json.dumps({"contents": {"liveChatRenderer": {"header": header}}})

        def text_msg(mid, name, runs, badge=None):
            r = {"id": mid, "authorName": {"simpleText": name}, "authorExternalChannelId": "UC" + name, "message": {"runs": runs}}
            if badge:
                r["authorBadges"] = [{"liveChatAuthorBadgeRenderer": {"icon": {"iconType": badge}}}]
            return {"addChatItemAction": {"item": {"liveChatTextMessageRenderer": r}}}
        polls = {
            "c1": [text_msg("m1", "viewer", [{"text": "hello "}, {"emoji": {"emojiId": "UCx/abc", "isCustomEmoji": True,
                    "shortcuts": [":wave:"], "image": {"thumbnails": [{"url": "https://yt3.ggpht.com/a=w24", "width": 24},
                                                                        {"url": "https://yt3.ggpht.com/a=w48", "width": 48}]}}}]),
                   text_msg("m2", "streamer", [{"text": "welcome"}], "OWNER"),
                   {"addChatItemAction": {"item": {"liveChatViewerEngagementMessageRenderer": {}}}}],
            "c2": [{"markChatItemAsDeletedAction": {"targetItemId": "m1"}}],
        }

        def youtube(path, body=None):
            if path.startswith("/live_chat"):
                return live if f"v={vid}" in path else YT_NOCHAT
            cont = json.loads(body)["continuation"]
            nxt = {"c1": "c2"}.get(cont, cont)
            return json.dumps({"continuationContents": {"liveChatContinuation": {"actions": polls.get(cont, []),
                               "continuations": [{"timedContinuationData": {"continuation": nxt, "timeoutMs": 1000}}]}}})
        m.yt_http = youtube

        w = await self.web(A_SID)
        w.send({"t": "create", "name": "yt"})
        code = first_t(await w.sync(), "created")["code"]
        w.send({"t": "link", "url": f"https://www.youtube.com/watch?v={vid}"})
        st = await self.wait_t(w, "ytchat_status")
        self.assertEqual((st["v"], st["ok"]), (vid, True))

        w.send({"t": "xchat", "on": True})
        got = await self.wait_t(w, "ytchat", lambda x: x["items"])
        a, b = got["items"]
        self.assertEqual((a["n"], a["k"], a["r"]), ("viewer", "", [["hello "], [":wave:", "https://yt3.ggpht.com/a=w48"]]))
        self.assertEqual((b["n"], b["k"]), ("streamer", "o"))
        self.assertEqual((await self.wait_t(w, "ytchat_del"))["ids"], ["m1"])

        late = await self.web(B_SID)   # someone joining later: told at once, the backlog when the panel opens
        late.send({"t": "join", "code": code})
        self.assertTrue(first_t(await late.sync(), "ytchat_status")["ok"])
        late.send({"t": "xchat", "on": True})
        back = first_t(await late.sync(), "ytchat")
        self.assertTrue(back["reset"])
        self.assertEqual([x["id"] for x in back["items"]], ["m2"])

        ch = m.yt_chats[vid]
        self.assertEqual(len(ch.subs), 2)
        late.send({"t": "xchat", "on": False})
        await late.sync()
        self.assertEqual(len(ch.subs), 1)
        w.send({"t": "link", "url": "https://youtu.be/NoChatVid01"})   # another video: its own answer, no panel left open
        st = await self.wait_t(w, "ytchat_status", lambda x: x["v"] == "NoChatVid01")
        self.assertFalse(st["ok"])
        self.assertIn("disabled", st["note"])
        self.assertEqual(len(ch.subs), 0)

    # ---- one lobby each, lobbies nobody is in close, the visitor log
    async def test_one_lobby_each(self):
        code = await self.lobby()   # alice's
        self.a.send("lobby create")
        la, = await sync(self.a)
        self.assertIn(f"You already have a lobby ({code})", msgs(la)[0])
        w = await self.web(A_SID)
        w.send({"t": "create"})
        err = first_t(await w.sync(), "error")
        self.assertEqual((err["code"], err["lobby"]), ("haslobby", code))
        wb = await self.web(B_SID)
        wb.send({"t": "create"})
        mine = first_t(await wb.sync(), "created")["code"]
        self.a.send(f"lobby owner {self.b.id}")   # bob has one now: alice can't hand hers over
        la, = await sync(self.a)
        self.assertIn("Bob already has a lobby", msgs(la)[0])
        self.assertEqual(m.lobbies[code].owner, A_SID)
        self.assertNotEqual(mine, code)
        self.make_admin(C_SID)   # cstv.xyz admins: any number
        c = await self.web(C_SID)
        for _ in range(2):
            c.send({"t": "create"})
            first_t(await c.sync(), "created")

    async def test_lobby_expires(self):
        code = await self.lobby()
        for c in list(m.lobbies[code].conns):
            m.leave_room(c)
        seen = m.db.one("SELECT seen FROM lobbies WHERE code=?", code)["seen"]
        self.assertAlmostEqual(seen, time.time(), delta=2)   # the hour counts from the last one out
        w = await self.web(B_SID)
        w.send({"t": "create"})
        busy = first_t(await w.sync(), "created")["code"]   # bob's website is in it
        m.db.x("UPDATE lobbies SET seen=?", time.time() - m.LOBBY_EXPIRE - 5)
        m.save_lobby(m.lobbies[code])
        del m.lobbies[code]   # unloaded, as after LOBBY_UNLOAD
        m.expire_lobbies(time.time())
        self.assertIsNone(m.db.one("SELECT 1 FROM lobbies WHERE code=?", code))
        self.assertIsNotNone(m.db.one("SELECT 1 FROM lobbies WHERE code=?", busy))
        self.assertAlmostEqual(m.db.one("SELECT seen FROM lobbies WHERE code=?", busy)["seen"], time.time(), delta=2)
        self.a.send("lobby create")   # alice may make a new one
        la, = await sync(self.a)
        self.assertTrue(all_of(la, "room"))

    async def test_visitor_log(self):
        self.make_admin(C_SID)
        w = await self.web(A_SID)
        c = await self.web(C_SID)
        c.send({"t": "site", "action": "visits", "tz": "America/New_York"})
        v = first_t(await c.sync(), "site_visits")
        self.assertEqual(v["day"], v["today"])
        self.assertEqual(v["days"][0], {"day": v["today"], "n": 1})   # everyone here is 127.0.0.1
        got = {(x["steamid"], x["kind"]): x for x in v["items"]}
        self.assertEqual(got[(A_SID, "web")]["name"], "alice")
        self.assertTrue(got[(A_SID, "web")]["online"])
        self.assertEqual(got[(A_SID, "game")]["n"], 1)
        self.assertEqual(got[(B_SID, "game")]["ip"], "127.0.0.1")
        await w.close()
        await asyncio.sleep(0.3)
        c.send({"t": "site", "action": "visits", "tz": "America/New_York", "day": v["today"]})
        v2 = first_t(await c.sync(), "site_visits")
        self.assertFalse({(x["steamid"], x["kind"]): x for x in v2["items"]}[(A_SID, "web")]["online"])
        c.send({"t": "site", "action": "visits", "tz": "Not/AZone"})   # unknown time zone: UTC days
        self.assertEqual(first_t(await c.sync(), "site_visits")["today"],
                         datetime.datetime.now(datetime.timezone.utc).date().isoformat())
        c.send({"t": "site", "action": "visits", "day": "2001-01-01"})
        self.assertEqual(first_t(await c.sync(), "site_visits")["items"], [])
        b = await self.web(B_SID)
        b.send({"t": "site", "action": "visits"})
        self.assertIn("admins", first_t(await b.sync(), "denied")["msg"])

    # ---- sign-in
    async def test_login_pairing(self):
        c = await self.game("carol", C_SID)
        self.assertEqual(c.you[1:], ["-", "-"])
        w = await self.web(C_SID)
        w.send({"t": "pair_code"})
        code = first_t(await w.sync(), "pair_code")["code"]

        dave = await self.game("dave", "76561190000000004")   # someone else's game can't use carol's code
        dave.send(f"login {code} linux")
        ld, = await sync(dave)
        self.assertIn("another one", msgs(ld)[0])

        c.send(f"login {code} linux")
        lc, = await sync(c)
        tok = first(lc, "token")[1]
        self.assertEqual(first(lc, "you")[1:], [C_SID, m.b64e("carol")])
        self.assertTrue(first_t(await w.sync(), "game_linked"))
        c.send(f"login {code} linux")   # a code works once
        lc, = await sync(c)
        self.assertIn("isn't valid", msgs(lc)[0])

        # the token signs the next connection in; another Steam account in that game doesn't get it
        again = await self.game("carol", C_SID, token=tok)
        self.assertEqual(again.you[1], C_SID)
        other = await self.game("eve", "76561190000000005", token=tok)
        self.assertEqual(other.you[1], "-")

        # signing out on the website ends it
        w.send({"t": "games"})
        gid = first_t(await w.sync(), "games")["items"][0]["id"]
        w.send({"t": "revoke", "id": gid})
        await w.sync()
        lc, = await sync(again)
        self.assertEqual(first(lc, "token"), ["token", "-"])
        self.assertEqual(last(lc, "you")[1], "-")
        dead = await self.game("carol", C_SID, token=tok)
        self.assertEqual(dead.you[1], "-")

        self.a.send("logout")
        la, = await sync(self.a)
        self.assertEqual(first(la, "token"), ["token", "-"])
        self.assertEqual(last(la, "you")[1], "-")

    # ---- the website and the game in one lobby
    async def test_web_and_game(self):
        w = await self.web(A_SID)
        w.send({"t": "create", "name": "movie night"})
        got = await w.sync()
        code = first_t(got, "created")["code"]
        snap = first_t(got, "lobby")
        self.assertTrue(snap["you"]["perms"] & m.OWNER)

        self.b.send("lobby join " + code)
        lb, = await sync(self.b)
        self.assertEqual(first(lb, "room")[1:3], ["lobby", code])
        members = first_t(await w.sync(), "members")
        self.assertTrue(any(x["steamid"] == B_SID and x["game"] and x["where"] == f"{SRV}/bhop_test" for x in members["list"]))

        w.send({"t": "link", "url": "https://www.youtube.com/watch?v=xyz"})
        await w.sync()
        lb, = await sync(self.b)
        url = first(lb, "url")
        self.assertEqual(m.b64d(url[2]), "https://www.youtube.com/watch?v=xyz")
        self.assertEqual(first(lb, "state")[6], "0")
        self.b.send(f"started {url[1]}")
        await sync(self.b)
        st = [x for x in await w.sync() if x["t"] == "state"][-1]
        self.assertTrue(st["running"])
        self.assertEqual(st["viewers"], 1)

        # a seek from the website: held until a viewer has the new position
        w.send({"t": "seek", "ms": 60000})
        st = [x for x in await w.sync() if x["t"] == "state"][-1]
        self.assertFalse(st["running"])
        w.send({"t": "started", "rev": st["rev"], "seek": st["seek"]})
        st = [x for x in await w.sync() if x["t"] == "state"][-1]
        self.assertTrue(st["running"])
        self.assertGreaterEqual(st["pos"], 60000)

        # chat both ways
        self.b.send("chat " + m.b64e("hi from the game"))
        lb, = await sync(self.b)
        self.assertEqual([m.b64d(x) for x in first(lb, "chat")[1:3]], ["bob", "hi from the game"])
        msg = first_t(await w.sync(), "chat")["msg"]
        self.assertEqual((msg["name"], msg["text"], msg["from"]), ("bob", "hi from the game", "game"))
        w.send({"t": "chat", "text": "hi from the site"})
        await w.sync()
        lb, = await sync(self.b)
        self.assertEqual(first(lb, "chat")[3], "web")

        # the website's settings reach the game
        w.send({"t": "admin", "action": "default_perms", "perms": 0})
        await w.sync()
        lb, = await sync(self.b)
        self.assertEqual(first(lb, "room")[4], "owner")
        w.send({"t": "admin", "action": "settings", "password": "pw"})
        await w.sync()
        lb, = await sync(self.b)
        self.assertEqual(last(lb, "room")[6], "1")

        # a password from the website
        w2 = await self.web(C_SID)
        w2.send({"t": "join", "code": code})
        self.assertEqual(first_t(await w2.sync(), "error")["code"], "password")
        w2.send({"t": "join", "code": code, "password": "pw"})
        self.assertTrue(first_t(await w2.sync(), "lobby"))

        # kicked from the website: the game goes back to its server's room
        w.send({"t": "admin", "action": "kick", "steamid": B_SID})
        await w.sync()
        lb, = await sync(self.b)
        self.assertTrue(all_of(lb, "kicked"))
        self.assertEqual(last(lb, "room")[1:3], ["srv", SRV])

    async def test_lobby_saved(self):
        code = await self.lobby()
        self.a.send("link " + m.b64e("https://example.com/saved"))
        self.a.send(f"place 1 1 {GEO}")
        await sync(self.a, self.b)
        lb = m.lobbies[code]
        m.save_lobby(lb)
        for c in list(lb.conns):
            m.leave_room(c)
        del m.lobbies[code]
        lb2 = m.load_lobby(code)
        self.assertEqual(lb2.url, "https://example.com/saved")
        self.assertTrue(lb2.paused)
        self.assertEqual(lb2.screens[f"{SRV}/bhop_test"].split()[:2], ["1", "1"])

    # ---- cstv.xyz admins, the lobby list
    def make_admin(self, sid):
        os.environ["CSTV_HUB_ADMINS"] = sid
        m._admins[0] = 0.0
        self.addCleanup(lambda: (os.environ.pop("CSTV_HUB_ADMINS", None), m._admins.__setitem__(0, 0.0)))

    async def test_site_admin_powers(self):
        self.make_admin(C_SID)
        code = await self.lobby()
        self.a.send("lobby password " + m.b64e("pw"))
        await sync(self.a, self.b)
        c = await self.web(C_SID)
        self.assertTrue(c.welcome["admin"])
        c.send({"t": "join", "code": code})   # no password needed, owner powers
        snap = first_t(await c.sync(), "lobby")
        self.assertTrue(snap["you"]["perms"] & m.SITEADMIN and snap["you"]["perms"] & m.OWNER)
        la, = await sync(self.a)
        cid = next(int(e[0]) for e in roster(la)[1] if e[1] == C_SID)
        self.a.send(f"lobby kick {cid}")   # the lobby's owner can't kick a site admin
        la, = await sync(self.a)
        self.assertIn("A cstv.xyz admin can't be kicked or banned.", msgs(la))
        # a non-admin can't use the admin actions; the admin can delete any lobby
        w = await self.web(B_SID)
        w.send({"t": "site", "action": "overview"})
        self.assertIn("admins", first_t(await w.sync(), "denied")["msg"])
        c.send({"t": "site", "action": "delete_lobby", "code": code})
        ov = first_t(await c.sync(), "site_overview")
        self.assertNotIn(code, [x["code"] for x in ov["lobbies"]])
        la, = await sync(self.a)
        self.assertIn("A cstv.xyz admin deleted the lobby.", [m.b64d(w[1]) for w in all_of(la, "kicked")])

    async def test_silent_join(self):
        self.make_admin(C_SID)
        code = await self.lobby()
        w = await self.web(A_SID)
        w.send({"t": "join", "code": code})
        before = first_t(await w.sync(), "lobby")
        c = await self.web(C_SID)
        c.send({"t": "join", "code": code, "silent": True})
        snap = first_t(await c.sync(), "lobby")
        self.assertTrue(snap["you"]["silent"])
        c.send({"t": "watching", "on": True})
        await c.sync()
        got = await w.sync()
        for x in got:
            if x.get("t") == "members":
                self.assertNotIn(C_SID, [p["steamid"] for p in x["list"] if p["online"]])
                self.assertEqual(x["guests"], 0)
            if x.get("t") == "state":
                self.assertEqual(x["viewers"], before["state"]["viewers"])
        self.assertNotIn(C_SID, m.lobbies[code].members)   # not remembered as a member either
        self.b.send("ping 0")
        lb, = await sync(self.b)
        self.a.send("lobby name " + m.b64e("x"))   # any roster the games get leaves the admin out
        la, = await sync(self.a)
        self.assertNotIn(C_SID, [e[1] for e in roster(la)[1]] if all_of(la, "roster") else [])
        c.send({"t": "lobbies"})
        it = next(x for x in first_t(await c.sync(), "lobbies")["items"] if x["code"] == code)
        self.assertEqual(it["people"], 2)   # alice (game + website) and bob; not the silent admin
        self.assertEqual(sorted(it["names"]), ["alice", "bob"])

    async def test_lobby_list(self):
        code = await self.lobby()   # alice + bob
        self.a.send("link " + m.b64e("https://www.youtube.com/watch?v=abcdefghijk"))
        await sync(self.a, self.b)
        w = await self.web(C_SID)
        w.send({"t": "create", "password": "secret"})
        got = await w.sync()
        priv = first_t(got, "created")["code"]
        g = await self.web()   # a guest
        g.send({"t": "lobbies"})
        items = first_t(await g.sync(), "lobbies")["items"]
        self.assertEqual([x["code"] for x in items][:2], [code, priv])   # most people first
        pub, pv = items[0], items[1]
        self.assertEqual(pub["people"], 2)
        self.assertEqual(sorted(pub["names"]), ["alice", "bob"])
        self.assertEqual(pub["playing"]["kind"], "youtube")
        self.assertTrue(pv["locked"])
        self.assertNotIn("names", pv)
        self.assertNotIn("playing", pv)
        self.b.send("lobbies 1")   # the game's page
        lb, = await sync(self.b)
        w0 = first(lb, "lobbies")
        self.assertEqual(w0[1:4], ["1", "1", "2"])
        self.assertEqual(w0[4].split(",")[:3], [code, "0", "2"])
        self.assertEqual(w0[5].split(",")[:3], [priv, "1", "1"])
        self.assertEqual(w0[5].split(",")[4], "-")   # what a private lobby plays isn't shown

    async def test_site_ban_warn_mute(self):
        self.make_admin(C_SID)
        code = await self.lobby()
        c = await self.web(C_SID)
        wb = await self.web(B_SID)
        wb.send({"t": "join", "code": code})
        await wb.sync()
        c.send({"t": "site", "action": "warn", "steamid": B_SID, "msg": "be nice"})
        await c.sync()
        lb, = await sync(self.b)
        self.assertEqual(m.b64d(first(lb, "warn")[1]), "be nice")
        self.assertEqual(first_t(await wb.sync(), "warn")["msg"], "be nice")
        c.send({"t": "site", "action": "mute", "steamid": B_SID, "minutes": 10})
        await c.sync()
        wb.send({"t": "chat", "text": "hi"})
        self.assertIn("muted on cstv.xyz", first_t(await wb.sync(), "denied")["msg"])
        c.send({"t": "site", "action": "ipban", "steamid": B_SID})
        self.assertIn("your own address", first_t(await c.sync(), "denied")["msg"])   # 127.0.0.1 is the admin's too
        c.send({"t": "site", "action": "ban", "steamid": B_SID, "reason": "spam"})
        ov = first_t(await c.sync(), "site_overview")
        self.assertEqual(ov["bans"][0]["steamid"], B_SID)
        self.assertEqual(ov["ip_bans"], [])   # a Steam ban is the account only
        self.assertTrue(m.globally_banned(B_SID))
        self.assertFalse(m.db.q("SELECT 1 FROM sessions WHERE steamid=?", B_SID))
        # an IP ban keeps everyone from that address out, except the admins
        m.db.x("INSERT INTO ip_bans VALUES(?,?,?,?)", "127.0.0.1", None, "", time.time())
        g = Web()
        hdr = {"Origin": m.ORIGIN}
        ws = await connect(f"ws://127.0.0.1:{self.port}/ws", additional_headers=hdr)
        self.assertEqual(json.loads(await ws.recv())["code"], "sitebanned")
        await ws.close()
        c2 = await self.web(C_SID)
        self.assertTrue(c2.welcome["admin"])

    async def test_game_lobby_page_lines(self):
        """The game's Lobby page: the chat so far on joining, new messages, deletions, the history, emotes."""
        code = await self.lobby()
        w = await self.web(A_SID)
        w.send({"t": "join", "code": code})
        await w.sync()
        w.send({"t": "chat", "text": "hello Kappa"})
        await w.sync()
        lb, = await sync(self.b)
        lc = [l for l in all_of(lb, "lchat")]
        self.assertTrue(lc)
        mine = lc[-1]
        self.assertEqual([mine[2], m.b64d(mine[3]), m.b64d(mine[4]), mine[5], mine[6]], [A_SID, "alice", "hello Kappa", "web", "-"])
        c = await self.game("carol", C_SID, signed=True)   # joining: reset, the chat so far, the emote channel
        c.send(f"lobby join {code}")
        got = await c.sync()
        self.assertTrue(all_of(got, "lchat_reset"))
        self.assertIn("hello Kappa", [m.b64d(x[4]) for x in all_of(got, "lchat")])
        self.assertEqual(first(got, "lchat_emotes")[1], "-")
        self.assertTrue(all_of(got, "history"))
        mid = int(mine[1])
        w.send({"t": "admin", "action": "delete_msg", "id": mid})
        await w.sync()
        lb, = await sync(self.b)
        self.assertEqual(first(lb, "lchat_del")[1:], [str(mid)])
        self.a.send("link " + m.b64e("https://www.twitch.tv/somechannel"))
        la, lb = await sync(self.a, self.b)
        self.assertEqual(all_of(lb, "lchat_emotes")[-1][1], "somechannel")
        self.a.send("link " + m.b64e("https://www.youtube.com/watch?v=abcdefghijk"))
        la, lb = await sync(self.a, self.b)
        hist = last(lb, "history")
        self.assertEqual(m.b64d(hist[1].split(",")[0]), "https://www.twitch.tv/somechannel")
        self.b.send("gifs - 0")   # no GIPHY key in tests
        lb, = await sync(self.b)
        self.assertTrue(any("GIF" in x for x in msgs(lb)))

    async def test_game_lobby_settings(self):
        code = await self.lobby()
        la, = await sync(self.a)
        self.a.send("lobby set slow 5")
        self.a.send("lobby set gifs 0")
        self.a.send("lobby set sites yt_twitch")
        self.a.send("lobby set emotes " + m.b64e("SomeChannel"))
        self.a.send(f"lobby set perms {m.PERM_LINK | m.PERM_QUEUE}")
        la, lb = await sync(self.a, self.b)
        self.assertEqual(last(lb, "lsettings")[1:], ["yt_twitch", "5", "0", "somechannel", str(m.PERM_LINK | m.PERM_QUEUE)])
        self.b.send("lobby set slow 0")   # not an admin
        lb, = await sync(self.b)
        self.assertIn("owner and admins", msgs(lb)[0])
        self.b.send("lobby delete")
        lb, = await sync(self.b)
        self.assertIn(code, m.lobbies)
        self.a.send("lobby delete")
        la, lb = await sync(self.a, self.b)
        self.assertNotIn(code, m.lobbies)
        self.assertTrue(all_of(lb, "kicked"))

    async def test_join_left_notices(self):
        sys_texts = lambda got: [x["msg"]["text"] for x in got if x.get("t") == "chat" and x["msg"].get("sys")]
        lchat_sys = lambda lines: [m.b64d(w[4]) for w in all_of(lines, "lchat") if w[5] == "sys"]
        self.make_admin(C_SID)
        code = await self.lobby()            # alice creates, bob joins (games)
        la, = await sync(self.a)
        lb = m.lobbies[code]
        texts = [x["text"] for x in lb.chat if x.get("sys")]
        self.assertEqual(texts.count("alice joined"), 0)   # she made it: nobody there to tell
        self.assertEqual(texts.count("bob joined"), 1)
        w = await self.web(A_SID)            # alice's browser too: the same person, no second notice
        w.send({"t": "join", "code": code})
        await w.sync()
        g = await self.web()                 # a guest
        g.send({"t": "join", "code": code})
        await g.sync()
        got = await w.sync()
        self.assertEqual(sys_texts(got), ["A guest joined"])
        la, = await sync(self.a)
        self.assertIn("A guest joined", lchat_sys(la))   # games' lobby chat gets it too
        c = await self.web(C_SID)            # a silent site admin: nothing
        c.send({"t": "join", "code": code, "silent": True})
        await c.sync()
        self.assertEqual(sys_texts(await w.sync()), [])
        # the guest reloads: out and back in at once, nothing said
        await g.close()
        g2 = await self.web()
        g2.send({"t": "join", "code": code})
        await g2.sync()
        self.assertEqual(sys_texts(await w.sync()), [])
        # the guest closes the tab for good: "A guest left" once the grace is over
        await g2.close()
        await asyncio.sleep(0.1)
        self.assertEqual(sys_texts(await w.sync()), [])
        lb.left_pending = {k: (t - m.LEFT_GRACE, n) for k, (t, n) in lb.left_pending.items()}
        lb.tick()
        self.assertEqual(sys_texts(await w.sync()), ["A guest left"])
        # bob leaves on purpose (!leave): said at once, also in the games' own chat
        self.b.send("lobby leave")
        await sync(self.b)
        self.assertEqual(sys_texts(await w.sync()), ["bob left"])
        la, = await sync(self.a)
        self.assertIn("bob left", msgs(la))
        # back long after: joined again (his last "joined" is old enough)
        lb.joined_said[B_SID] -= m.JOIN_QUIET
        self.b.send("lobby join " + code)
        await sync(self.b)
        self.assertEqual(sys_texts(await w.sync()), ["bob joined"])

    async def test_legacy_hublink(self):
        """An older game build (cstv_hublink.py: JSON at /ws, no Origin) can still join a lobby."""
        code = await self.lobby()
        ws = await connect(f"ws://127.0.0.1:{self.port}/ws")
        try:
            await ws.send(json.dumps({"t": "hello", "kind": "game", "token": self.tok[C_SID], "name": "carol"}))
            self.assertEqual(json.loads(await ws.recv())["steamid"], C_SID)
            await ws.send(json.dumps({"t": "join", "code": code}))
            snap = json.loads(await ws.recv())
            self.assertEqual(snap["t"], "lobby")
        finally:
            await ws.close()


if __name__ == "__main__":
    unittest.main()
