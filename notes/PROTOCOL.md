# CS:TV protocols (client-only version)

```
libcstv.so (game) ──unix socket /tmp/cstv/ctl.sock──▶ cstvd ──TCP 27970──▶ cstv-master (NA <master-host>)
                                                         └──RTMP 1935──▶ mediamtx on NA (streams only)
```

All three links: one line per message, words separated by single spaces, `\n` terminated.
Free text, names, titles and URLs are base64url without padding (`b64` below); `-` = empty.
Unknown verbs are ignored everywhere (forward compatibility).

## cstvd ↔ master

On (re)connect cstvd sends `hello`, then `where`, then `lobby join <code> <password b64|->` if it was in a lobby (cstvd remembers the password the player last gave).
Master answers `welcome`, `room`, `roster`, and everything the room has (`screen`, `url`+`state`, `queue`, ...).

### cstvd → master
| line | meaning |
|---|---|
| `hello <proto=2> <steamid64\|0> <name b64>` | identify |
| `where <server> <map>` | game server `ip:port` (or `-` when not on a server) and map. Not in a lobby → member of room `srv:<server>`. Screens are keyed by `<server>/<map>` inside the room. |
| `ping <t>` | → `pong <t>` |
| `lobby create [<name b64\|-> [<password b64>]]` | new lobby, sender = owner, leaves the server room; no name / `-` = "<player>'s lobby" |
| `lobby join <code> [<password b64>]` | join by code (6 chars `A-Z2-9`, case-insensitive); the password when it has one |
| `lobby name <name b64>` | owner only; at most 40 printable characters |
| `lobby password <b64\|->` | owner only; `-` removes it |
| `lobby leave` | back to the server room |
| `lobby kick <id>` / `lobby ban <id>` / `lobby unban <steamid>` | owner only; ban = steamid + IP |
| `lobby owner <id>` | owner only; hand ownership over |
| `lobby perm <all\|owner>` | owner only; who may control playback/screen (`all` default) |
| `place <scale 0.1..4> <float 0\|1> <pos xyz> <normal xyz> <right xyz> <up xyz>` | screen for the sender's `<server>/<map>`. normal = the side viewers see; right/up = picture axes. Size = 512*scale × 288*scale units. |
| `remove` | remove the screen at the sender's location |
| `link <url b64>` / `queue <url b64>` / `unqueue <uid>` / `playnow <uid>` / `clearqueue` | |
| `next` / `prev` / `pause` / `seek <+30\|-10\|1:23>` / `speed <pct 25..400>` / `loop <0\|1>` | |
| `started <rev>` | cstvd's first frame (or buffered seek) for revision rev |
| `ended <rev> <next url b64\|-> <title b64\|->` | video over; next = playlist's next (same list) |
| `title <uid> <title b64>` | answer to a title lookup |
| `stream start` / `stream live` / `stream stop` | see Streams |

### master → cstvd
| line | meaning |
|---|---|
| `welcome <proto> <your id>` | ids are small integers, per connection |
| `pong <t>` | |
| `room <srv\|lobby> <name> <owner 0\|1> <perm all\|owner> <label b64\|-> <password 0\|1> <password b64\|->` | name = server addr or lobby code; owner = you; label = the lobby's name; the password itself only goes to the owner (`-` for everyone else). Not in a room: `room srv - 0 all - 0 -` |
| `joinfail <code> <nolobby\|banned\|password\|wrong\|locked>` | a join didn't work: no such lobby, banned, it has a password and none was given, wrong password, or 5 wrong tries from that IP in 10 minutes |
| `roster <owner id\|-> [<id>,<steamid>,<name b64>,<where b64> ...]` | members |
| `screen <scale> <float> <pos> <normal> <right> <up>` / `screen none` | for your location |
| `url <rev> <url b64>` | new link (rev bumps on every link) |
| `state <rev> <paused> <pos_ms> <speed%> <seek_id> <running>` | clock, valid when sent (cstvd adds RTT/2 when running) |
| `stop` | nothing playing |
| `nowtitle <title b64\|->` | title of the current link once known |
| `queue [<uid>,<url b64>,<title b64\|->,<by b64> ...]` | whole queue on every change |
| `upnext <url b64>` | prefetch hint |
| `tol <ms>` | drift tolerance |
| `title <uid> <url b64>` | please look up this link's title (answer `title <uid> <b64>`) |
| `skip` | please report the playlist's next video (`ended` with skip) |
| `msg <text b64>` | chat line for the player |
| `kicked <reason b64>` | removed from the lobby (now back in the server room) |
| `stream_go <publish url b64>` | to the streamer: start pushing here |
| `stream_end` | to the streamer: stop pushing |

### Streams
`stream start` → master makes a stream id + secret, replies `stream_go rtmp://<master-host>:1935/live/<id>?key=<secret>`.
cstvd creates `/tmp/cstv/capture` (`struct cstv_cap_shm` in cstv_proto.h), sets `active` and sends the
client `capture 1`; the client fills it with the game's own picture (BGRA 1280x720) and its own sound mix
(s16 from the SDL audio callback). A writer thread in cstvd feeds that to ffmpeg at a fixed 30 fps (newest
frame, the previous one again if none; silence while the game's audio hasn't started or stalls; a tick is
dropped whole, picture and sound, when ffmpeg can't keep up) → h264 (h264_vaapi on /dev/dri/renderD128
if it works, else libx264 veryfast/zerolatency) ~3500k, keyframe every 2 s, aac 160k, flv to the url.
No desktop capture and no desktop audio: Discord, notifications and the CS:TV video itself stay out.
After ~2 s of pushing with game frames arriving, cstvd sends `stream live`. No game heartbeat within 5 s
→ stream_state error ("restart the game after updating CS:TV"); ffmpeg exiting → error with its last
stderr line. On stop cstvd clears `active`, sends `capture 0`, ends ffmpeg and unlinks the file.
`stream_cmd=<shell command>` in the prefs file (`{url}` = the publish url) replaces all of this with the
player's own command: a desktop capture, whose audio and windows are the player's responsibility.
Windows: not available yet.
Master sets the room's link to `rtmp://<master-host>:1935/live/<id>` (kind live: no seek/pause clock,
like a Twitch channel). `stream stop` (or the streamer leaving) ends it: master sends `stop` to the
room and `stream_end` to the streamer. mediamtx asks the master over HTTP (127.0.0.1:27971,
mediamtx `authMethod: http`) whether a publish is allowed; reads are open.
cstvd and the client accept `rtmp://` links only when the host is the master's host.

## libcstv.so ↔ cstvd (adds to the existing verbs in cstv_proto.h: load, pause, sync, seek, vol, speed, stop, ping, opt, info)

### client → cstvd
| line | meaning |
|---|---|
| `ident <steamid64> <name b64>` | sent on connect; cstvd uses it for `hello` |
| `m <master line>` | forward to the master (cstvd only passes the client verbs listed above, ≤ 3000 bytes) |
| `search <id> <query b64>` | YouTube search (yt-dlp `ytsearch15:`) |
| `pref <name> <value>` | set + save a personal setting (`vol 0-100`, `quality <auto\|source\|1080\|720\|480\|360>`, `subs 0\|1`, `chat 0\|1`, `upnext 0\|1`, `hud 0\|1`, `hud_x`/`hud_y`/`hud_w` 0..1) in `~/.config/cstv/prefs` |
| `pscreen <server>/<map>` | which personal screen is kept here? (sent with every new `where`) |
| `pscreen <server>/<map> <scale> <float> <pos> <normal> <right> <up>` / `pscreen <server>/<map> none` | keep / forget the personal screen here (geometry as in `place`) |
| `stream start` / `stream stop` | the streamer's own capture |

`started`, `ended`, `title`, `skip`, `upnext`, `tol` no longer go through the client: cstvd talks to the master itself.

### cstvd → client
| line | meaning |
|---|---|
| `srv <master line>` | every master line, verbatim (client uses screen, url, state, stop, msg, room, roster, queue, nowtitle, kicked; ignores the rest) |
| `master <0\|1>` | master connection up/down |
| `pref <name> <value>` | all prefs, sent on client connect and after each change |
| `pscreen <server>/<map> <geometry>\|none` | answer to every `pscreen` line: what is kept there now |
| `result <id> <i> <url b64> <title b64> <channel b64> <duration s\|->` / `results_end <id> <n\|error b64>` | search results |
| `stream_state <idle\|starting\|live\|error> <text b64\|->` | |
| `capture 1` / `capture 0` | start / stop writing the game's picture and sound into `/tmp/cstv/capture` (map it on `capture 1`: it's a new file each stream; also stop when `active` is 0) |
| `chat 0\|1` | as before |

## Personal settings
Volume, quality, subtitles, live chat panel, up-next notices, the HUD screen (on/off, top left and width as
fractions of the game's screen): per player, in cstvd's prefs file. cstvd applies quality/subs/chat itself;
the client reads `pref vol` for the distance volume and the `hud*` prefs for the HUD screen.

## Screens
* Shared: the room's (`place` / `remove` / `screen`); only players in the same room (lobby or server room) on
  the same server + map see it.
* Personal: only this player sees it. cstvd keeps one per `<server>/<map>` in `~/.config/cstv/screens`
  (`<key> <geometry>` lines, at most 1000) and never tells the master.
* HUD: the video on the player's HUD; prefs only. Volume is full while it's on, else by the nearest world screen.

## Master address
cstvd: `CSTV_MASTER` env, else `master=` in the prefs file, else `<master-host>:27970`.
