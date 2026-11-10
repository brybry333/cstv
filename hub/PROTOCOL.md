# cstv-hub protocol

One WebSocket at `/ws`, JSON text messages `{"t": "<type>", ...}`. Browsers send an `Origin` header and are
signed in by the `cstv_session` cookie (Steam OpenID at `/login`). Anything without an `Origin` is a game
(the helper's hub link) and signs in with a token from the link-code flow. A foreign `Origin` is refused.

First message from the client: `{"t":"hello","kind":"web"}` or, from a game,
`{"t":"hello","kind":"game","token":"<saved token or omitted>","name":"<in-game name>"}`.

## Clock

The hub's clock is `now` = monotonic milliseconds of the hub process.
`{"t":"time","c":<client ms>}` -> `{"t":"time","c":<same>,"h":<hub ms>}` (not rate limited). Clients keep
the lowest-round-trip sample: `offset = h - (c + rtt/2)`, hub time = local time + offset.

Playback state (`{"t":"state", ...}`):

| key | meaning |
|---|---|
| `rev` | changes with every new link (and stop) |
| `url`, `title`, `by` | what plays (`url` null = nothing) |
| `paused`, `pos`, `at` | position `pos` ms at hub time `at` |
| `speed` | percent, 25..400 |
| `seek` | changes with every jump (exact seek instead of drift correction) |
| `running` | false while the hub waits for the first viewer's `started` (max 25 s), like the plugin |
| `loop` | restart instead of moving on |
| `now` | hub time when sent (a first clock estimate) |
| `tol` | drift tolerance in ms: 150 with 2+ viewers, 5000 alone (the plugin's `tol`) |
| `viewers` | connections with a running player |

Position now = `pos + (hubnow - at) * speed / 100` when `running && !paused`, else `pos`.

## Client -> hub

| message | needs | |
|---|---|---|
| `create` `{name?}` | signed in | new lobby, you own it -> `created {code}` then `lobby`; one lobby each (cstv.xyz admins any number): `error {code: haslobby, lobby}` if you have one. A lobby nobody has been in for an hour closes for good |
| `site {action: visits, tz?, day?}` | cstv.xyz admin | -> `site_visits {day, today, days: [{day, n}], items: [{ip, steamid, name, kind, ua, first, last, n, online}]}`: the visitor log (30 days), one day in time zone `tz` |
| `join {code}` / `leave` | | -> `lobby` snapshot or `error {code: nolobby\|banned}` |
| `mylobbies` | | -> `mylobbies {owned, recent}` |
| `delete {code}` | owner | |
| `watching {on}` | | a player runs here (counts as a viewer) |
| `link {url}` | LINK | play now |
| `queue {url}` | QUEUE | append (plays at once when nothing plays) |
| `queue_action {action: play\|next\|remove\|clear, uid}` | QUEUE (play: LINK) | |
| `pause {paused}` | PAUSE | |
| `seek {ms}` or `seek {rel}` | SEEK | |
| `speed {pct}`, `loop {on}` | PLAYBACK | |
| `skip` | SKIP | next in the queue, else stop |
| `stop` | LINK | |
| `started {rev}` | | first frame of `rev` is up (releases a held clock) |
| `ended {rev, next?}` | | the video ended; `next` = the playlist's next video (game helper). The first report wins for 3 s |
| `title {rev, title}` | game | a title the hub couldn't look up |
| `chat {text}` | signed in, not muted | 300 chars, 5 per 5 s |
| `admin {action, ...}` | ADMIN | `default_perms {perms}`, `settings {name?, sites?: any\|yt_twitch}`, `perms {steamid, perms\|null}`, `admin {steamid, on}` (owner only), `mute {steamid, minutes}` (0 = unmute), `kick`, `ban`, `unban {steamid}` |
| `link_game {code}` | signed in (browser) | gives the waiting game a token |
| `whep {offer}` | in a lobby | watch its live stream over WebRTC -> `whep {id, answer}` / `whep_fail {msg}` |
| `hls` | in a lobby | the same over low-latency HLS (browsers whose WebRTC can't show H.264, e.g. Vivaldi, or where WebRTC can't connect) -> `hls {id, url: "/hls/<token>/index.m3u8"}` / `hls_fail {msg}`; the token is this connection's, for that stream only |
| `vstats {mode, codec, ua, res, fps, fd, fdrop, fz, fzd, lost, jit, kbps, stalls}` | in a lobby | the stream player's numbers every 10 s: one line in the hub log, nothing kept |
| `xchat {on}` | in a lobby | the stream chat panel opened / closed: for a YouTube live video the hub reads its chat -> `ytchat` |

Everything except `time` is limited to 20 messages per 5 s. Refusals come back as `denied {msg}`.

## Hub -> client

`welcome {steamid, name, avatar, now, public_url, gifs, stats, admin}`, `lobby {code, name, owner, default_perms, sites, state, queue,
chat, members, guests, banned, you}`, `state`, `queue {items: [{uid, url, title, by}]}`, `members {list, guests,
banned}`, `chat {msg: {id, steamid, name, text, ts, from} | {id, sys: true, text, ts}}`, `you {steamid, name, perms}`,
`settings {name, default_perms, sites}`, `kicked {msg}`, `denied {msg}`, `error {msg, code?}`, `created`, `deleted`,
`mylobbies`, `linked {name}` (browser), and for games: `link_code {code, url}`, `token {token, steamid, name}`,
`upnext {url}` (prefetch the queue head).

YouTube live chat (the stream chat panel; YouTube has no open feed, the hub reads the chat its own page uses while
a browser has the panel open, one reader per video): `ytchat_status {v, ok, state: checking|ok|none, note}` to every
browser in a lobby playing video `v` (whether it has a live chat; asked of YouTube when the link changes or someone
comes in), `ytchat {v, items: [{id, n, k: o|m|s|"", p, r: [[text] | [alt, image url]], ch}], reset?}` (`reset`: the
backlog when the panel opens; `k` owner / moderator / member, `p` a paid message's amount), `ytchat_del {v, ids}`,
`ytchat_clear {v, ch}` (everything by one author).

HTTP besides the pages: `/hls/<token>/<file>` passes mediamtx's HLS (127.0.0.1:8888, `/live/<id>/...`) on to the
viewer the token was given to, while that stream is live (`no-store`; LL-HLS's `_HLS_msn/_HLS_part/_HLS_skip` and
mediamtx's `session` are the only query parameters passed).

Lobby chat notices (`sys` messages, also to games as `lchat ... sys` and printed in their chat): "<name> joined" /
"A guest joined" when someone comes in while others are there (one person = their SteamID across game + browser,
a guest = their IP; never for a silent site admin; not again for the same person within 2 min), "<name> left" /
"A guest left" 45 s after their last connection went if they didn't come back (at once for the game's `!leave`).

## Permissions

The plugin's bits: LINK 1, QUEUE 2, SKIP 4, PAUSE 8, SEEK 16, PLAYBACK 32 (speed + loop), SCREEN 64; plus ADMIN 128
(settings, permissions, kick, ban, mute) and OWNER 256. The owner and admins have everything; others get their own
value or the lobby default (all 7 for a new lobby). Signed-out viewers watch only.

## Game link codes

A game without a valid token gets `link_code {code, url}` and shows it on the TV. A signed-in browser sends
`link_game {code}`; the game receives `token` (keep it; send it in `hello` from then on). Codes live as long as
the game's connection (max 30 min).

Not yet: game server rooms (auto-join by who's on the same server), screen positions per server/map, playlists
(the helper's `plist`), lobby chat on the in-game panel.
