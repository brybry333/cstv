#!/usr/bin/env python3
"""cstv_ytdl - what mpv runs instead of yt-dlp (cstvd sets script-opts ytdl_hook-ytdl_path to it).

mpv's ytdl_hook calls it with yt-dlp's arguments and the link last (after "--"). If cstvd already looked
that link up (the next video of the queue / playlist, fetched while the current one plays), the saved
answer is printed at once and the video starts ~2 s sooner. Otherwise it runs yt-dlp and passes its answer on.
YouTube is asked for an answer without an ad (no ad = no wait: see NO_ADS); should one come with an ad anyway,
the answer is held back until YouTube's links work (see wait_until_available).

  cstv_ytdl.py --prefetch <url>    (cstvd) look <url> up now, with the arguments mpv used last time

Answers are kept in /tmp/cstv/ytcache (RAM), used once, and dropped after 45 minutes (YouTube's stream
links stay valid for hours). Nothing about the video itself is saved, only yt-dlp's description of it.
"""
import hashlib
import json
import os
import subprocess
import sys
import time

WINDOWS = sys.platform == "win32"
# Windows: what we start must not open console windows (we run without one)
NO_WINDOW = {"creationflags": 0x08000000} if WINDOWS else {}   # CREATE_NO_WINDOW
# the Windows build carries yt-dlp.exe next to this script; Linux finds yt-dlp in PATH
_HERE_YTDLP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "yt-dlp.exe")
YTDLP = _HERE_YTDLP if WINDOWS and os.path.isfile(_HERE_YTDLP) else "yt-dlp"


# No ads: YouTube's servers refuse a player's video links (403) until its pre-roll ad could have been skipped
# (~5 s), so an ad means a wait even though we never show it. yt-dlp 2026.10.21 asks as a Samsung TV first,
# which gets an ad on nearly every video. player_params 2AMB (the player request's "params", where YouTube is
# told what kind of playback this is) gets an answer with no ad on every client tried: 15 of 15 videos on
# 2026-10-24, incl. a music video that had an ad on all 6 clients without it, on visionos and tv_samsung alike;
# links work at once. Asked as visionos (the Vision Pro app): no JS challenge (faster), up to 2160p, subtitles,
# dubbed audio, live. If that can't get a video, again with yt-dlp's own clients (2AMB too).
# YAHIAQE= did the same in the same test, should 2AMB stop working.
NO_ADS = "player_params=2AMB"
FIRST_TRY = ["--extractor-args", "youtube:player_client=visionos;" + NO_ADS]
FALLBACK = ["--extractor-args", "youtube:" + NO_ADS]


def lookup(args, errors):
    """yt-dlp's answer for these mpv arguments (no ads, visionos first): (exit status, stdout). errors: where the
    final attempt's messages go (mpv shows the first ERROR line); the first attempt's are dropped."""
    if not any(h in (args[-1] if args else "") for h in ("youtube.com/", "youtu.be/", "youtube-nocookie.com/")):
        r = subprocess.run([YTDLP] + args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=errors, **NO_WINDOW)
        return r.returncode, r.stdout
    at = args.index("--") if "--" in args else 0
    r = subprocess.run([YTDLP] + args[:at] + FIRST_TRY + args[at:], stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, **NO_WINDOW)
    if r.returncode == 0 and r.stdout.lstrip().startswith(b"{"):
        return 0, r.stdout
    r = subprocess.run([YTDLP] + args[:at] + FALLBACK + args[at:], stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=errors, **NO_WINDOW)
    return r.returncode, r.stdout


def wait_until_available(data):
    """Safety net, should YouTube attach an ad after all: its video links answer 403 until the ad could have been skipped.
    yt-dlp gives that time as each format's "available_at" and sleeps until then before it downloads;
    mpv doesn't know about it and asks at once (every load failed with 403, 2026-10-24). So sleep here,
    before mpv gets the answer: the latest time over all formats, so a quality switch right after the start
    works too. Without an ad available_at is the lookup time: no wait."""
    try:
        info = json.loads(data)
    except ValueError:
        return
    if not isinstance(info, dict):
        return
    at = max([f.get("available_at") or 0 for f in info.get("formats") or [] if isinstance(f, dict)]
             + [info.get("available_at") or 0])
    wait = at - time.time()
    if wait > 0:
        time.sleep(min(wait, 30))


def answer(args):
    """Look up for mpv: the answer, once the links work, on our stdout; errors straight to mpv."""
    sys.stdout.flush()
    code, out = lookup(args, sys.stderr)
    if code == 0:
        wait_until_available(out)
    sys.stdout.buffer.write(out)
    sys.stdout.flush()
    return code


CACHE = os.path.join(os.environ.get("CSTV_DIR", "/tmp/cstv"), "ytcache")
TEMPLATE = os.path.join(CACHE, "args.json")
TTL = 45 * 60
URL = "\x00url"


def key(args):
    # mpv builds the option list in no fixed order (Lua table), so compare the sorted words
    return hashlib.sha1("\x00".join(sorted(args)).encode()).hexdigest()


def write_atomic(path, data):
    tmp = f"{path}.{os.getpid()}.tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def prefetch(url):
    try:
        with open(TEMPLATE) as f:
            tmpl = json.load(f)
    except (OSError, ValueError):
        return 0   # mpv hasn't looked anything up yet: no arguments to copy
    args = [url if a == URL else a for a in tmpl]
    code, out = lookup(args, subprocess.DEVNULL)
    if code == 0 and out.lstrip().startswith(b"{"):
        write_atomic(os.path.join(CACHE, key(args) + ".json"), out)
    now = time.time()
    for name in os.listdir(CACHE):   # tidy up answers nobody used
        p = os.path.join(CACHE, name)
        try:
            if name.endswith(".json") and p != TEMPLATE and now - os.path.getmtime(p) > TTL:
                os.unlink(p)
        except OSError:
            pass
    return 0


def main():
    args = sys.argv[1:]
    try:
        os.makedirs(CACHE, mode=0o700, exist_ok=True)
    except OSError:
        return answer(args)
    if len(args) == 2 and args[0] == "--prefetch":
        return prefetch(args[1])
    if len(args) >= 2 and args[-2] == "--":   # remember how mpv asks, for the next prefetch
        try:
            write_atomic(TEMPLATE, json.dumps(args[:-1] + [URL]).encode())
        except OSError:
            pass
    path = os.path.join(CACHE, key(args) + ".json")
    try:
        if time.time() - os.path.getmtime(path) < TTL:
            with open(path, "rb") as f:
                data = f.read()
            os.unlink(path)   # once: if this link turns out bad, the retry does a fresh lookup
            wait_until_available(data)
            sys.stdout.buffer.write(data)
            return 0
    except OSError:
        pass
    return answer(args)


if __name__ == "__main__":
    sys.exit(main())
