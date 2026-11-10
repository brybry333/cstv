# CS:TV

In-game video screens for Counter-Strike: Source (64-bit). Place a screen with `!tv`, play YouTube, Twitch or
most yt-dlp sites on it with `!link <url>`, and everyone in the same room who runs the mod watches in sync.
It works on any CS:S server, and the server doesn't need anything installed. Video is streamed, nothing is saved
to disk. Linux only for now.

Licensed under GPL-3.0, see `LICENSE`.

## Authors

The original plugin version and the cstv.xyz hub are by brybry333. The client-only rewrite is by daf
([ddyfad](https://github.com/ddyfad)).

## What's where

| Folder | What it is |
|---|---|
| `cstv/client/` | The in-game part (C++, Source SDK headers): screens, chat commands, the `!cstv` window, stream capture. |
| `cstv/helper/` | `cstvd` (C + libmpv) plays the video into shared memory. `cstv_overlay.py` draws chat, `cstv_ytdl.py` wraps yt-dlp, `cstv_link.py` connects to the hub. |
| `cstv/common/` | Shared-memory layouts and the line protocol used by the client and helper. |
| `cstv/assets/` | Materials and fonts that go in `cstrike/custom/cstv`. |
| `cstv/tools/` | Debug tools, plus `mcassets.py` for the Progress Log's book (see below). |
| `hub/` | The cstv.xyz server and website. `hub/deploy/` has the setup scripts. |
| `notes/PROTOCOL.md` | The wire protocols. |
| `server-plugin/` | The old SourceMod plugin, for reference only. The client doesn't use it. |
| `systemd/` | Socket-activated user units for the helper. |

## Building (Linux)

Make can't handle spaces in paths, so clone somewhere without any.

1. Source SDK headers (not included). From `cstv/`:
   ```
   git clone --filter=blob:none --sparse https://github.com/ValveSoftware/source-sdk-2013 third_party/source-sdk-2013
   cd third_party/source-sdk-2013 && git checkout be57e3d01ac7d7aa6a2cacbfef187c0b46616db0
   git sparse-checkout set src/common src/public src/tier1
   ```
2. Helper: `make` in `cstv/helper/`. It needs the libmpv dev files. At run time it also wants yt-dlp, streamlink,
   Python 3 with Pillow, and Noto Sans.
3. Client, on its own: `make` in `cstv/client/` builds `libcstv.so`, which is loaded with `LD_PRELOAD`.

## Running

Set Steam's launch options for CS:S to the line below. It starts the helper, loads the mod and copies the materials.

```
/path/to/repo/cstv/cstv-launch %command% -insecure
```

CS:TV hooks the game process, so only use it with `-insecure` and never on VAC-secured servers.

Everyone connects through the hub at cstv.xyz by default. To use your own hub, set `CSTV_HUB_URL=wss://your-host/game`.

## Progress Log

`cstv/client/cstv_prog.inc` is the Progress Log's Minecraft-book window, currently built as part of this client.
Its art, fonts and sounds are Mojang's, so they aren't in this repo. `cstv/tools/mcassets.py` makes them from
your own copy of Minecraft. Without them the window draws a plain book.
