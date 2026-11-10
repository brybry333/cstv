#!/usr/bin/env python3
"""A throwaway hub for the browser tests: hub.py on a free local port with a temp DB, plus a signed-in fake
game (the /game line protocol, as cstv_link.py speaks it) that creates a lobby and plays a Twitch channel in it,
so the lobby page has a video and the Twitch chat panel.

    python3 local_hub.py [--channel auto|<login>] [--url https://...]   (auto: the top live non-mature channel right now)

Prints `READY <base url> <lobby code>` once the lobby is up, then stays until Ctrl-C / SIGTERM (the game stays
connected, the hub runs; both stop together and the temp dir is removed). The hub's log is <tmp>/hub.log.
Nothing outside the temp dir is touched: the DB, the session and the lobby exist only there.
"""
import argparse
import asyncio
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time

HUB_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SID, NAME = "76561190000000042", "browsertest"


def live_channel(fallback="monstercat"):
    """The most watched live, non-mature Twitch channel right now (Twitch's public web GraphQL, read-only)."""
    import json
    import urllib.request
    q = '{ streams(first: 20) { edges { node { broadcaster { login broadcastSettings { isMature } } } } } }'
    req = urllib.request.Request("https://gql.twitch.tv/gql", data=json.dumps({"query": q}).encode(),
                                 headers={"Client-Id": "kimne78kx3ncx6brgo4mv6wki5h1ko", "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=8) as r:
            for e in json.load(r)["data"]["streams"]["edges"]:
                b = e["node"]["broadcaster"]
                if not (b.get("broadcastSettings") or {}).get("isMature"):
                    return b["login"]
    except Exception as e:
        print(f"(couldn't ask Twitch who is live: {e}; using {fallback})", file=sys.stderr)
    return fallback


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


async def wait_port(port, proc, timeout=15):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if proc.poll() is not None:
            raise SystemExit(f"hub.py exited with {proc.returncode}")
        try:
            r, w = await asyncio.open_connection("127.0.0.1", port)
            w.close()
            return
        except OSError:
            await asyncio.sleep(0.1)
    raise SystemExit("hub.py didn't start listening")


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--channel", default="auto", help="Twitch channel to play in the lobby (auto: the top live one)")
    ap.add_argument("--url", help="any other link to play instead (YouTube, ...)")
    ap.add_argument("--admin", action="store_true",
                    help="make the user a cstv.xyz admin, print a website session (ADMIN <cookie>) and add sample visits")
    args = ap.parse_args()
    link = args.url or f"https://www.twitch.tv/{live_channel() if args.channel == 'auto' else args.channel}"

    tmp = tempfile.mkdtemp(prefix="cstv-browsertest-")
    port = free_port()
    env = dict(os.environ,
               CSTV_HUB_PORT=str(port), CSTV_HUB_HOST="localhost",   # localhost: ::1 and 127.0.0.1 both answer
               CSTV_HUB_PUBLIC_URL=f"http://localhost:{port}",       # Twitch's embeds accept parent=localhost
               CSTV_HUB_DB=os.path.join(tmp, "hub.db"), CSTV_HUB_STREAM_HOST="", CSTV_HUB_AUTH_PORT="0")
    os.environ.update(env)

    # the user + a game session, written with the hub's own code before it opens the same DB
    sys.path.insert(0, HUB_DIR)
    import hub as m
    m.db = m.DB(env["CSTV_HUB_DB"])
    m.db.x("INSERT INTO users VALUES(?,?,?,?)", SID, NAME, None, time.time())
    token = m.new_session(SID, "game")
    web_token = None
    if args.admin:
        env["CSTV_HUB_ADMINS"] = os.environ["CSTV_HUB_ADMINS"] = SID
        web_token = m.new_session(SID, "web")
        now = time.time()
        ua = {"ff": "Mozilla/5.0 (X11; Linux x86_64; rv:140.0) Gecko/20100101 Firefox/140.0",
              "ch": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36",
              "ip": "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1"}
        for ago, dur, ip, sid, name, kind, u in (
                (600, 300, "203.0.113.7", SID, NAME, "web", ua["ff"]), (3000, 900, "203.0.113.7", SID, NAME, "game", None),
                (5400, 60, "198.51.100.23", "", "guest", "web", ua["ip"]), (7200, 1800, "192.0.2.44", "76561190000000077", "jumper", "web", ua["ch"]),
                (7300, 1700, "192.0.2.44", "76561190000000077", "jumper", "game", None), (90000, 400, "198.51.100.23", "", "guest", "web", ua["ip"]),
                (95000, 2000, "203.0.113.9", "", "bhopper", "game", None), (180000, 600, "192.0.2.44", "76561190000000077", "jumper", "web", ua["ch"])):
            m.db.x("INSERT INTO visits(at, until, ip, steamid, name, kind, ua) VALUES(?,?,?,?,?,?,?)",
                   now - ago, now - ago + dur, ip, sid, name, kind, u)
    m.db.c.close()

    log = open(os.path.join(tmp, "hub.log"), "w")
    proc = subprocess.Popen([sys.executable, os.path.join(HUB_DIR, "hub.py")], env=env, cwd=HUB_DIR,
                            stdout=log, stderr=subprocess.STDOUT)
    try:
        await wait_port(port, proc)
        from websockets.asyncio.client import connect
        async with connect(f"ws://127.0.0.1:{port}/game") as ws:
            lines = asyncio.Queue()

            async def pump():
                async for msg in ws:
                    for line in msg.split("\n"):
                        if line.strip():
                            await lines.put(line.strip())
            pumper = asyncio.create_task(pump())

            async def expect(*words, timeout=10):
                t0 = time.time()
                while True:
                    w = (await asyncio.wait_for(lines.get(), max(0.1, timeout - (time.time() - t0)))).split()
                    if w[:len(words)] == list(words):
                        return w

            await ws.send(f"auth {token}")
            await ws.send(f"hello {m.PROTO} {SID} {m.b64e(NAME)}")
            await expect("welcome")
            await ws.send("where 127.0.0.1:27015 bhop_browsertest")
            await ws.send("lobby create")
            code = (await expect("room", "lobby"))[2]   # (the "where" above gave a server room first)
            await ws.send("link " + m.b64e(link))
            await expect("url")
            if web_token:
                print(f"ADMIN {web_token}", flush=True)
            print(f"READY http://localhost:{port} {code}", flush=True)
            print(f"  lobby page: http://localhost:{port}/l/{code}   playing {link}   hub log: {tmp}/hub.log", flush=True)

            stop = asyncio.get_running_loop().create_future()
            for sig in (signal.SIGINT, signal.SIGTERM):
                asyncio.get_running_loop().add_signal_handler(sig, lambda: stop.done() or stop.set_result(None))

            async def drain():   # keep reading (the hub may ask the game for titles; nothing needs an answer)
                while True:
                    await lines.get()
            drainer = asyncio.create_task(drain())
            await asyncio.wait([stop, pumper], return_when=asyncio.FIRST_COMPLETED)
            drainer.cancel()
            pumper.cancel()
    finally:
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
        subprocess.run(["rm", "-rf", tmp])


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
