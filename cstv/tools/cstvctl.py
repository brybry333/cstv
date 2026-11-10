#!/usr/bin/env python3
"""cstvctl — talk to cstvd like the game does (for testing without CS:S).

  cstvctl.py play <url> [seconds] [--vol N] [--pos S] [--png out.png]
      load the URL, keep pinging, print state every second, optionally save the newest frame as PNG
  cstvctl.py status        print the shared-memory header once
  cstvctl.py snap <prefix> save the current video frame and chat panel as PNGs (read-only, no connection)
  extra: --cmd "opt chat 1" (repeatable, sent after load) --at N "info" (send at second N) --chatpng out.png
"""
import base64, mmap, os, socket, struct, subprocess, sys, time

SHM = "/tmp/cstv/frames"
SOCK = "/tmp/cstv/ctl.sock"
# frame size comes from the shared-memory header
HDR = struct.Struct("<6I I I Q Q d I I 96s")   # matches struct cstv_shm up to status
STATES = {0: "idle", 1: "loading", 2: "playing", 3: "paused", 4: "error"}


def header(mm):
    magic, ver, w, h, fmt, fb, latest, reading, seq, hb, pos, state, live, status = HDR.unpack_from(mm, 0)
    return dict(w=w, h=h, latest=latest, seq=seq, hb_age=(time.clock_gettime_ns(time.CLOCK_MONOTONIC) - hb) / 1e9,
                pos=pos, state=STATES.get(state, state), live=live, status=status.split(b"\0")[0].decode(errors="replace"), fb=fb)


def dxt1_to_png(block_bytes, w, h, out):
    rgb = bytearray(w * h * 3)
    bw = w // 4
    for bi in range(len(block_bytes) // 8):
        c0, c1, idx = struct.unpack_from("<HHI", block_bytes, bi * 8)
        def ex(c): return ((c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)
        a, b = ex(c0), ex(c1)
        if c0 > c1:
            pal = [a, b, tuple((2 * x + y) // 3 for x, y in zip(a, b)), tuple((x + 2 * y) // 3 for x, y in zip(a, b))]
        else:
            pal = [a, b, tuple((x + y) // 2 for x, y in zip(a, b)), (0, 0, 0)]
        bx, by = (bi % bw) * 4, (bi // bw) * 4
        for p in range(16):
            col = pal[(idx >> (2 * p)) & 3]
            o = ((by + p // 4) * w + bx + p % 4) * 3
            rgb[o:o + 3] = bytes(col)
    ppm = out + ".ppm"
    with open(ppm, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h) + bytes(rgb))
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", ppm, "-vf", "scale=640:-1", out], check=True)
    os.unlink(ppm)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    fd = os.open(SHM, os.O_RDONLY)
    mm = mmap.mmap(fd, 0, prot=mmap.PROT_READ)
    if sys.argv[1] == "status":
        print(header(mm))
        return
    if sys.argv[1] == "snap":
        prefix = sys.argv[2]
        h = header(mm)
        print(h)
        if h["latest"] <= 2:
            off = 4096 + h["latest"] * h["fb"]
            dxt1_to_png(mm[off:off + h["fb"]], h["w"], h["h"], prefix + "_video.png")
            print("saved", prefix + "_video.png")
        if os.path.exists("/tmp/cstv/chat"):
            cfd = os.open("/tmp/cstv/chat", os.O_RDONLY); cm = mmap.mmap(cfd, 0, prot=mmap.PROT_READ)
            latest = struct.unpack_from("<I", cm, 16)[0]
            if latest <= 2:
                from PIL import Image
                raw = cm[4096 + latest * 512 * 768 * 4: 4096 + (latest + 1) * 512 * 768 * 4]
                Image.frombytes("RGB", (512, 768), raw, "raw", "BGRX").save(prefix + "_chat.png"); print("saved", prefix + "_chat.png")
        return
    url = sys.argv[2]
    secs = float(sys.argv[3]) if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else 15
    args = sys.argv[3:]
    vol = int(args[args.index("--vol") + 1]) if "--vol" in args else 0
    pos = float(args[args.index("--pos") + 1]) if "--pos" in args else 0
    png = args[args.index("--png") + 1] if "--png" in args else None
    chatpng = args[args.index("--chatpng") + 1] if "--chatpng" in args else None
    extra = [args[i + 1] for i, a in enumerate(args) if a == "--cmd"]
    timed = [(float(args[i + 1]), args[i + 2]) for i, a in enumerate(args) if a == "--at"]
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    b64 = base64.urlsafe_b64encode(url.encode()).decode().rstrip("=")
    s.sendall(f"vol {vol}\nload {b64} {int(pos * 1000)} 0\n".encode())
    for c in extra:
        s.sendall((c + "\n").encode())
    t0 = time.time()
    first = None
    last_seq = None
    while time.time() - t0 < secs:
        for at, c in list(timed):
            if time.time() - t0 >= at:
                s.sendall((c + "\n").encode()); timed.remove((at, c)); print(f"  -> sent: {c}")
        s.sendall(b"ping\n")
        time.sleep(1)
        h = header(mm)
        if first is None and h["seq"] and h["latest"] != 0xFFFFFFFF:
            first = time.time() - t0
        fps = h["seq"] - last_seq if last_seq is not None else 0
        last_seq = h["seq"]
        print(f"{time.time() - t0:5.1f}s {h['state']:8} live={h['live']} pos={h['pos']:7.1f} frames/s={fps:3} | {h['status']}")
    if png and header(mm)["latest"] != 0xFFFFFFFF:
        h = header(mm)
        off = 4096 + h["latest"] * h["fb"]
        dxt1_to_png(mm[off:off + h["fb"]], h["w"], h["h"], png)
        print("saved", png)
    if chatpng and os.path.exists("/tmp/cstv/chat"):
        cfd = os.open("/tmp/cstv/chat", os.O_RDONLY); cm = mmap.mmap(cfd, 0, prot=mmap.PROT_READ)
        latest = struct.unpack_from("<I", cm, 16)[0]
        if latest <= 2:
            raw = cm[4096 + latest * 512 * 768 * 4: 4096 + (latest + 1) * 512 * 768 * 4]
            from PIL import Image
            Image.frombytes("RGB", (512, 768), raw, "raw", "BGRX").save(chatpng); print("saved", chatpng)
    s.sendall(b"stop\n")
    time.sleep(0.3)
    s.close()
    print(f"first frame after {first:.1f}s" if first else "no frames")


if __name__ == "__main__":
    main()
