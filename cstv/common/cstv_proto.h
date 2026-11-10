// cstv_proto.h — shared between the host helper (cstvd) and the game-side preload (libcstv.so).
//
// Frames: /tmp/cstv/frames, an mmap'ed file = struct cstv_shm. Three frame buffers so the helper
// never writes the one the game is copying ("triple buffering"):
//   helper: write into a buffer that is neither `latest` nor `reading`, then publish it as `latest`
//           and bump `seq`.
//   game:   idx = latest; reading = idx; if latest still == idx copy buf[idx]; reading = NONE.
// Control: unix stream socket /tmp/cstv/ctl.sock, one line per command (game -> helper):
//   load <url base64url> <pos_ms> <paused 0|1> <running 0|1>   start playing (pos ignored for live streams)
//   pause <0|1> <pos_ms>                         pause/unpause, re-syncing to pos
//   sync <pos_ms> <running>                      periodic position from the master; helper seeks if off by >1.5 s
//   seek <pos_ms> / speed <x> / opt <name> <v> / info
//   vol <0-100>                                  volume (the game derives it from the player's distance)
//   stop                                         stop playback and audio
//   ping                                         keepalive; the helper stops if none arrives for 3 s
//   ident, m <master line>, search, pref, stream start|stop   (notes/PROTOCOL.md)
// helper -> game: srv <master line>, master 0|1, pref, result/results_end, stream_state, chat 0|1.
// The helper holds the connection to the master server (notes/PROTOCOL.md) and reports started/ended/title itself.
// /tmp is shared between the host and Steam's game container, so both sides see the same files.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
// Windows: the game reaches cstvd at 127.0.0.1:CSTV_TCP_PORT, and the buffers are named file mappings
// whose names carry the layout version (builds of different versions never share one). Runtime files
// (log, prefetch cache) go to %TEMP%\cstv.
#define CSTV_TCP_PORT   27961
#define CSTV_SHM_PATH   "Local\\cstv_frames_v2"
#else
#define CSTV_DIR        "/tmp/cstv"
#define CSTV_SHM_PATH   "/tmp/cstv/frames"
#define CSTV_SOCK_PATH  "/tmp/cstv/ctl.sock"
#endif
#define CSTV_MAGIC      0x56545343u   /* "CSTV" */
#define CSTV_VERSION    2u    /* 2: 1920x1080 (1: 1280x720) */

#define CSTV_W          1920
#define CSTV_H          1080
#define CSTV_FMT_DXT1   1u
#define CSTV_FRAME_BYTES ((CSTV_W / 4) * (CSTV_H / 4) * 8)   /* DXT1: 8 bytes per 4x4 block */
#define CSTV_NONE       0xFFFFFFFFu

enum cstv_state { CSTV_IDLE = 0, CSTV_LOADING = 1, CSTV_PLAYING = 2, CSTV_PAUSED = 3, CSTV_ERROR = 4 };

struct cstv_shm {
    uint32_t magic, version;
    uint32_t w, h, format, frame_bytes;
    volatile uint32_t latest;        /* newest complete buffer, CSTV_NONE if none yet */
    volatile uint32_t reading;       /* buffer the game is copying right now, CSTV_NONE otherwise */
    volatile uint64_t seq;           /* bumps once per published frame */
    volatile uint64_t heartbeat_ns;  /* CLOCK_MONOTONIC of the helper's last loop */
    volatile double pos_s;           /* playback position (s) */
    volatile uint32_t state;         /* enum cstv_state */
    volatile uint32_t live;          /* 1 = live stream (no seeking) */
    char status[96];                 /* short human-readable status for logs */
    uint8_t pad[4096 - 160];
    uint8_t buf[3][CSTV_FRAME_BYTES];
};

#ifdef __cplusplus
static_assert(offsetof(cstv_shm, buf) == 4096, "cstv_shm header must be exactly 4096 bytes");
#else
_Static_assert(offsetof(struct cstv_shm, buf) == 4096, "cstv_shm header must be exactly 4096 bytes");
#endif

// ---------------------------------------------------------------- overlay renderer (cstv_overlay.py)
// /tmp/cstv/chat: the Twitch chat panel, BGRX, triple-buffered exactly like the video frames.
#ifdef _WIN32
#define CSTV_CHAT_PATH   "Local\\cstv_chat_v1"
#else
#define CSTV_CHAT_PATH   "/tmp/cstv/chat"
#endif
#define CSTV_CHAT_MAGIC  0x54414843u   /* "CHAT" */
#define CSTV_CHAT_W      512
#define CSTV_CHAT_H      768
#define CSTV_CHAT_BYTES  (CSTV_CHAT_W * CSTV_CHAT_H * 4)

struct cstv_chat_shm {
    uint32_t magic, version, w, h;
    volatile uint32_t latest, reading;
    volatile uint64_t seq;
    volatile uint64_t heartbeat_ns;
    uint8_t pad[4096 - 40];
    uint8_t buf[3][CSTV_CHAT_BYTES];
};

// /tmp/cstv/card: the info card, RGBA (straight alpha), blended into the video by cstvd while
// CLOCK_MONOTONIC < until_ns, with its top-left corner at (x, y) in the video frame.
#ifdef _WIN32
#define CSTV_CARD_PATH   "Local\\cstv_card_v1"
#else
#define CSTV_CARD_PATH   "/tmp/cstv/card"
#endif
#define CSTV_CARD_MAGIC  0x44524143u   /* "CARD" */
#define CSTV_CARD_W      1080
#define CSTV_CARD_H      315

struct cstv_card_shm {
    uint32_t magic, version, w, h, x, y;
    volatile uint64_t seq;
    volatile uint64_t until_ns;
    uint8_t pad[4096 - 40];
    uint8_t buf[CSTV_CARD_W * CSTV_CARD_H * 4];
};

#ifdef __cplusplus
static_assert(offsetof(cstv_chat_shm, buf) == 4096 && offsetof(cstv_card_shm, buf) == 4096, "overlay headers must be 4096 bytes");
#else
_Static_assert(offsetof(struct cstv_chat_shm, buf) == 4096 && offsetof(struct cstv_card_shm, buf) == 4096, "overlay headers must be 4096 bytes");
#endif

// /tmp/cstv/lchat: the lobby chat on the CS:TV window's Lobby page (and its emote / GIF pickers), drawn by
// cstv_overlay.py at the size the window asks for (w x h <= the max), BGRX rows of w*4 bytes, triple-buffered
// like the chat panel. The game makes its texture that size and draws it 1:1 with the VGUI surface.
#ifdef _WIN32
#define CSTV_LCHAT_PATH  "Local\\cstv_lchat_v1"
#else
#define CSTV_LCHAT_PATH  "/tmp/cstv/lchat"
#endif
#define CSTV_LCHAT_MAGIC 0x5448434Cu   /* "LCHT" */
#define CSTV_LCHAT_MAXW  800
#define CSTV_LCHAT_MAXH  1200

struct cstv_lchat_shm {
    uint32_t magic, version, w, h;
    volatile uint32_t latest, reading;
    volatile uint64_t seq;
    volatile uint64_t heartbeat_ns;
    uint8_t pad[4096 - 40];
    uint8_t buf[3][CSTV_LCHAT_MAXW * CSTV_LCHAT_MAXH * 4];
};

// ---------------------------------------------------------------- in-game capture (streaming)
// /tmp/cstv/capture: what the game draws and plays, for "Start stream". cstvd creates the file (size
// sizeof(struct cstv_cap_shm)) and sets `active` while it streams; the game client maps it when cstvd
// sends `capture 1` and stops writing when `active` is 0 or after `capture 0`.
// Video: BGRA 1920x1080 (60 a second; version 2, was 1280x720 at 30), top row first, triple-buffered like the video frames: the game writes into a
// buffer that is neither `latest` nor `reading`, sets frame_ns (CLOCK_MONOTONIC), publishes it as
// `latest` and bumps `seq`; cstvd sets `reading` while it copies.
// Audio: the game's own mix (SDL audio callback), converted to interleaved s16 at the device's rate.
// The game sets rate/channels before its first write; audio_written = total frames ever written;
// frame i lives at audio[(i % CSTV_CAP_AUDIO_FRAMES) * channels]. cstvd keeps its own read count and
// skips ahead if it falls more than the ring behind.
#ifdef _WIN32
#define CSTV_CAP_PATH   "Local\\cstv_capture_v1"
#else
#define CSTV_CAP_PATH   "/tmp/cstv/capture"
#endif
#define CSTV_CAP_MAGIC  0x54504143u   /* "CAPT" */
#define CSTV_CAP_W      1920
#define CSTV_CAP_H      1080
#define CSTV_CAP_FRAME_BYTES (CSTV_CAP_W * CSTV_CAP_H * 4)
#define CSTV_CAP_AUDIO_FRAMES 262144   /* ~5.5 s at 48 kHz; power of two */

struct cstv_cap_shm {
    uint32_t magic, version, w, h;
    volatile uint32_t active;          /* cstvd: 1 while streaming */
    volatile uint32_t latest, reading; /* video buffers, CSTV_NONE = none */
    volatile uint32_t audio_rate, audio_channels;   /* set by the game; channels 1 or 2 */
    uint32_t pad0;
    volatile uint64_t seq, frame_ns;
    volatile uint64_t game_heartbeat_ns;            /* game: CLOCK_MONOTONIC of its last write (video or audio) */
    volatile uint64_t audio_written;
    uint8_t pad[4096 - 72];
    uint8_t buf[3][CSTV_CAP_FRAME_BYTES];
    int16_t audio[CSTV_CAP_AUDIO_FRAMES * 2];
};

#ifdef __cplusplus
static_assert(offsetof(cstv_cap_shm, buf) == 4096, "cstv_cap_shm header must be exactly 4096 bytes");
#else
_Static_assert(offsetof(struct cstv_cap_shm, buf) == 4096, "cstv_cap_shm header must be exactly 4096 bytes");
#endif
