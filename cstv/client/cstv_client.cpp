// libcstv.so — CS:TV client preload: draws the video on screens in the world (the room's shared one
// and the player's own personal one) and on the HUD, turns chat commands (!tv, !link, ...) into lines
// for the helper (cstvd), and follows what cstvd forwards from the master server (screen, link, clock).
// No server plugin: it works on any server.
//
// Loads into cstrike_linux64 via LD_PRELOAD (cstv-launch), or is built into bhoptools' preload (make
// CSTV_DIR=..., -DCSTV_EMBEDDED, see cstv_embed.h: its Fixes tab / F9 turn CS:TV off and on); Windows: a DLL
// that bhoptools.exe loads (cstv_platform_win.cpp). CSTV_DISABLE=1 keeps it out. A thread waits for the
// game's modules, then swaps vtable slots (every hook calls the original):
//   IBaseClientDLL (VClient017, client.so)
//      6 LevelInitPostEntity  create the textures, find our materials
//      7 LevelShutdown        stop the video, forget the screen
//     20 IN_KeyEvent          +attack / +use pressed while aiming at the screen -> pause
//     35 FrameStageNotify     helper I/O (FRAME_START, FRAME_RENDER_START), uploads + volume (RENDER_START),
//                             stream capture (START; see cstv_capture.cpp)
//   IVRenderView (VEngineRenderView014, engine.so)
//      9 SceneEnd             draw the screens + chat panel and the place-mode preview: the world and
//                             models are drawn, the main 3D view is still pushed, the view model comes
//                             after (it stays in front)
//   IVEngineClient (VEngineClient015, engine.so)
//    106 ClientCmd_Unrestricted  the chat box's "say ..." line: CS:TV commands are taken here, before the
//                             engine's command system (checked in game on remote servers, 2026-10-17; the
//                             same slot on Windows)
// Other calls: IVEngineClient 6 ServerCmd, 8 GetPlayerInfo, 11 Con_IsVisible, 12 GetLocalPlayer,
// 26 IsInGame, 51 GetLevelName, 72 GetNetChannelInfo; INetChannelInfo 1 GetAddress; IEngineTrace 4
// TraceRay; ICvar (VEngineCvar004, libvstdlib.so) 5 AllocateDLLIdentifier, 6 RegisterConCommand,
// 7 UnregisterConCommand, 10 FindCommandBase; IMaterialSystem 71 FindMaterial; IMatRenderContext 9 Bind,
// 20 MatrixMode, 21 PushMatrix, 22 PopMatrix, 31 LoadIdentity, 56 GetDynamicMesh; IMesh 13 Draw;
// IBaseClientDLL 36 DispatchUserMessage (SayText = user message 3 in CS:S), 59 GetPlayerView.
// Slot numbers come from the SDK headers (tools/vtidx.cpp) and were checked against the Linux
// binaries (2026-10-18): vtable lengths (tools/vtdump.py), how often client.so calls each slot of the
// engine globals (tools/vtcalls.py), and the code in the slots (see tools/vtidx.cpp for what each
// check found). ConCommandBase/ConCommand field offsets and slots match client.so's own copies.
//
// Uploads run on the render thread (queued through the material system's call queue), so the
// game's main thread only spends ~1 µs per frame on them (measured 2026-10-15).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <cctype>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imaterialvar.h"
#include "materialsystem/itexture.h"
#include "materialsystem/imesh.h"
#include "vtf/vtf.h"
#include "cdll_int.h"
#include "view_shared.h"
#include "ivrenderview.h"
#include "engine/IEngineTrace.h"
#include "gametrace.h"
#include "inetchannelinfo.h"
#include "icvar.h"
#include "tier1/convar.h"
#include "tier1/bitbuf.h"
#include "tier1/callqueue.h"
#include "tier1/functors.h"

#include "cstv_proto.h"
#include "cstv_capture.h"
#include "cstv_gui.h"
#include "cstv_place.h"
#include "cstv_platform.h"
#include "cstv_embed.h"

#define CSTV_CLIENT_VERSION 3   // 3: cstv.xyz (sign-in, lobby chat), bhoptools embedding, Windows

// ---------------------------------------------------------------- utilities
typedef void (*ConMsg_t)(const char *fmt, ...);
static ConMsg_t g_ConMsg;

static void say(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_ConMsg) g_ConMsg("[cstv] %s\n", buf);
    else fprintf(stderr, "[cstv] %s\n", buf);
}

static double now_s() { return plat_now(); }

static std::string shorten(const std::string &s) { return s.size() > 80 ? s.substr(0, 77) + "..." : s; }

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static std::string b64encode(const std::string &in)
{
    std::string b;
    int acc = 0, bits = 0;
    for (unsigned char c : in) {
        acc = (acc << 8) | c;
        bits += 8;
        while (bits >= 6) { bits -= 6; b.push_back(B64[(acc >> bits) & 63]); }
    }
    if (bits) b.push_back(B64[(acc << (6 - bits)) & 63]);
    return b;
}

static bool b64decode(const std::string &in, std::string &out)
{
    out.clear();
    int acc = 0, bits = 0;
    for (char ch : in) {
        const char *p = ch ? strchr(B64, ch) : nullptr;
        if (ch == '=') break;
        if (!p) return false;
        acc = (acc << 6) | (int)(p - B64);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((char)((acc >> bits) & 0xff)); }
    }
    return true;
}

// Only the form of a link. cstvd does the real check before anything fetches it (public hosts only;
// rtmp:// only from the master's host, which only cstvd knows).
static bool url_ok(const std::string &u)
{
    if (u.size() < 8 || u.size() > 1500) return false;
    if (u.compare(0, 8, "https://") && u.compare(0, 7, "http://") && u.compare(0, 7, "rtmp://")) return false;
    for (unsigned char c : u)
        if (c <= ' ' || c == 0x7f) return false;
    return true;
}

static std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static std::string lower(std::string s)
{
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static std::vector<std::string> words(const std::string &s)
{
    std::vector<std::string> w;
    size_t i = 0;
    while ((i = s.find_first_not_of(" \t", i)) != std::string::npos) {
        size_t e = s.find_first_of(" \t", i);
        w.push_back(s.substr(i, e - i));
        i = e;
    }
    return w;
}

static bool is_number(const std::string &s)
{
    char *end;
    strtod(s.c_str(), &end);
    return !s.empty() && !*end && (isdigit((unsigned char)s[0]) || s[0] == '.');
}

// ---------------------------------------------------------------- math (mathlib's versions aren't header-inline)
static void angle_vectors(const QAngle &a, Vector *fwd, Vector *right, Vector *up)
{
    float p = a.x * (float)M_PI / 180, y = a.y * (float)M_PI / 180, r = a.z * (float)M_PI / 180;
    float sp = sinf(p), cp = cosf(p), sy = sinf(y), cy = cosf(y), sr = sinf(r), cr = cosf(r);
    if (fwd) *fwd = Vector(cp * cy, cp * sy, -sp);
    if (right) *right = Vector(-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp);
    if (up) *up = Vector(cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp);
}

static Vector unit(const Vector &v)
{
    float l = v.Length();
    return l > 1e-6f ? v * (1 / l) : Vector(0, 0, 0);
}

// On/off (bhoptools' Fixes tab, F9; cstv_set_enabled): off = no video, no screens, no window, no connection
// to cstvd (which then quits by itself). main_tick applies a change on the main thread.
static std::atomic<bool> g_on{true};

// ---------------------------------------------------------------- helper connection (main thread only)
static intptr_t g_sock = -1;
static bool g_playing_sent;   // the helper was told to load the current link
static int g_speed_pct = 100;
static int g_seek_id = -1;
static bool g_running = true;   // the master's clock is running (false while it waits for the video to start)
static double g_last_connect_try, g_last_ping, g_last_where;
static cstv_shm *g_shm;
static std::string g_ident_sent, g_where_sent;   // what cstvd was last told (resent after reconnecting)

static bool helper_send(const char *fmt, ...)
{
    if (g_sock < 0) return false;
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0 || n >= (int)sizeof buf) return false;
    if (plat_send(g_sock, buf, n) != n) {
        plat_close(g_sock);
        g_sock = -1;
        g_playing_sent = false;   // after reconnecting, send the link + aged position again
        capture_stop();
        say("lost the connection to cstvd");
        return false;
    }
    return true;
}

void cstv_helper_line(const char *line)
{
    helper_send("%s\n", line);
}

// Tried on connecting and then twice a second until it works: with socket activation our connect is what
// starts cstvd, so right after a reboot (/tmp emptied) the buffer doesn't exist yet when we first look
// (found 2026-10-19: sound played, the screen kept its placeholder until CS:TV was turned off and on).
static void map_shm()
{
    if (g_shm) return;
    void *p = plat_map(CSTV_SHM_PATH, sizeof(cstv_shm));
    if (!p) return;
    cstv_shm *s = (cstv_shm *)p;
    if (s->magic != CSTV_MAGIC || s->version != CSTV_VERSION || s->format != CSTV_FMT_DXT1 || s->w != CSTV_W || s->h != CSTV_H) {
        static bool warned;   // magic 0: cstvd is still filling in the header
        if (s->magic == CSTV_MAGIC && !warned) { warned = true; say("cstvd's frame buffer has an unexpected format; rebuild cstvd"); }
        plat_unmap(p, sizeof(cstv_shm));
        return;
    }
    g_shm = s;
}

static void update_where();

static bool g_warned_no_helper;
static double g_no_helper_since;   // first failed connect since the last good one (0 = none)
static void helper_connect(double t)
{
    if (g_sock >= 0 || t - g_last_connect_try < 2) return;
    g_last_connect_try = t;
    intptr_t s = plat_connect();
    if (s < 0) {
        if (g_no_helper_since == 0) g_no_helper_since = t;
        if (t - g_no_helper_since >= 5) plat_spawn_helper();
        if (!g_warned_no_helper) { g_warned_no_helper = true; say("cstvd is not reachable (systemctl --user enable --now cstvd.socket, or run cstv-launch); videos stay off"); }
        return;
    }
    g_sock = s;
    g_warned_no_helper = false;
    g_no_helper_since = 0;
    map_shm();
    say("connected to cstvd");
    g_ident_sent.clear();
    g_where_sent.clear();
    update_where();
    helper_send("speed %.2f\n", g_speed_pct / 100.0);
}

// ---------------------------------------------------------------- state from the master (via cstvd's "srv" lines)
static std::string g_from_helper;
static bool g_chat_active;   // cstvd is drawing a chat for the current video
static bool g_chat_panel;    // ... and the player wants it beside the screen (else it's for the window's Live tab)
static double g_last_bind_check;
static int g_user_vol = 100;
static bool g_universal;     // pref universal: full volume everywhere, like with the HUD screen on

void cstv_volume_preview(int vol) { g_user_vol = std::max(0, std::min(100, vol)); }

struct Screen {
    bool on, floating;
    float scale;
    Vector pos, normal, right, up;   // normal = the side viewers see; right/up = the picture's axes from there
    float w() const { return PLACE_W * scale; }
    float h() const { return PLACE_H * scale; }
};
static Screen g_scr;    // the room's shared screen for this server + map (from the master)
static Screen g_pscr;   // this player's personal one here (from cstvd's ~/.config/cstv/screens)
static std::string g_loc;   // "<server>/<map>", the key of both; empty when not on a server
static double g_place_wait; // a shared screen was sent to the master and no `screen` came back yet

// "<scale> <float> <pos> <normal> <right> <up>", as the master and cstvd send it
static Screen parse_screen(const char *a)
{
    Screen s = {};
    int fl = 0;
    if (sscanf(a, "%f %d %f %f %f %f %f %f %f %f %f %f %f %f", &s.scale, &fl, &s.pos.x, &s.pos.y, &s.pos.z, &s.normal.x, &s.normal.y,
               &s.normal.z, &s.right.x, &s.right.y, &s.right.z, &s.up.x, &s.up.y, &s.up.z) == 14 &&
        s.scale >= 0.05f && s.scale <= 4 && std::isfinite(s.pos.x) && std::isfinite(s.pos.y) && std::isfinite(s.pos.z)) {
        s.floating = fl != 0;
        s.normal = unit(s.normal);
        s.right = unit(s.right);
        s.up = unit(s.up);
        s.on = s.normal.LengthSqr() > 0.5f && s.right.LengthSqr() > 0.5f && s.up.LengthSqr() > 0.5f;
    }
    return s;
}

static std::string screen_line(const Screen &s)
{
    char b[300];
    snprintf(b, sizeof b, "%.3f %d %.1f %.1f %.1f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f", s.scale, s.floating ? 1 : 0, s.pos.x, s.pos.y,
             s.pos.z, s.normal.x, s.normal.y, s.normal.z, s.right.x, s.right.y, s.right.z, s.up.x, s.up.y, s.up.z);
    return b;
}

static int g_url_rev = -1;   // revision of the link we hold
static std::string g_url;
static bool g_have_state;    // a state line arrived for g_url_rev
static bool g_paused;
static long g_pos_ms;
static double g_state_time;  // when the state arrived (to age the position)

static void send_load()
{
    if (g_url.empty() || !g_have_state) return;
    bool counting = !g_paused && g_running;
    long pos = g_pos_ms + (counting ? (long)((now_s() - g_state_time) * 1000 * g_speed_pct / 100) : 0);
    if (helper_send("load %s %ld %d %d\n", b64encode(g_url).c_str(), pos, g_paused ? 1 : 0, g_running ? 1 : 0)) {
        g_playing_sent = true;
        say("playing %s", shorten(g_url).c_str());
    }
}

static void stop_video()
{
    if (g_playing_sent) helper_send("stop\n");
    g_playing_sent = false;
    g_have_state = false;
    g_url.clear();
    g_url_rev = -1;
}

static void on_master(const char *line)
{
    char verb[16] = {0};
    int off = 0;
    if (sscanf(line, "%15s %n", verb, &off) < 1) return;
    const char *a = line + off;
    if (!strcmp(verb, "screen")) {
        g_scr = parse_screen(a);   // "screen none" too
        g_place_wait = 0;
    } else if (!strcmp(verb, "url")) {
        int rev;
        char b[2100];
        std::string url;
        if (sscanf(a, "%d %2099s", &rev, b) != 2) return;
        if (!b64decode(b, url) || !url_ok(url)) { say("ignored an invalid link from the master"); return; }
        if (rev == g_url_rev && url == g_url) return;
        if (g_playing_sent) helper_send("stop\n");
        g_url = url;
        g_url_rev = rev;
        g_playing_sent = false;
        g_have_state = false;   // wait for the state line that follows
    } else if (!strcmp(verb, "state")) {
        int rev, paused, speed = 100, seek_id = -1, running = 1;
        long pos;
        if (sscanf(a, "%d %d %ld %d %d %d", &rev, &paused, &pos, &speed, &seek_id, &running) < 3 || rev != g_url_rev) return;
        bool seeked = g_have_state && seek_id != g_seek_id && g_playing_sent;
        g_seek_id = seek_id;
        g_running = running != 0;
        if (speed >= 25 && speed <= 400 && speed != g_speed_pct) {
            g_speed_pct = speed;
            helper_send("speed %.2f\n", speed / 100.0);
        }
        bool was_paused = g_paused;
        g_paused = paused != 0;
        g_pos_ms = pos;
        g_state_time = now_s();
        g_have_state = true;
        if (!g_playing_sent) send_load();
        else if (g_paused != was_paused) helper_send("pause %d %ld\n", g_paused ? 1 : 0, pos);
        else if (seeked) helper_send("seek %ld\n", pos);
        else helper_send("sync %ld %d\n", pos, g_running ? 1 : 0);   // running = 0 while the master holds its clock
    } else if (!strcmp(verb, "stop")) {
        stop_video();
    } else if (!strcmp(verb, "chat")) {   // chat <name b64> <text b64> <web|game>: a line of the lobby's chat
        char nb[300], tb[1300], from[16] = "";
        std::string name, text;
        if (sscanf(a, "%299s %1299s %15s", nb, tb, from) < 2 || !b64decode(nb, name) || !b64decode(tb, text)) return;
        std::string line = std::string("\x03") + name + (strcmp(from, "web") ? "" : " (cstv.xyz)") + "\x01: " + text;
        cstv_print(line.c_str());
    } else if (!strcmp(verb, "msg") || !strcmp(verb, "kicked")) {
        char b[1024];
        std::string text;
        if (sscanf(a, "%1023s", b) != 1 || !b64decode(b, text)) return;
        if (!strcmp(verb, "kicked")) text = "You were removed from the lobby" + (text.empty() ? std::string(".") : ": " + text);
        cstv_print(text.c_str());
    }
}

// Lines from cstvd (non-blocking).
static void helper_poll()
{
    if (g_sock < 0) return;
    char buf[4096];
    int r;
    while ((r = plat_recv(g_sock, buf, sizeof buf)) > 0) {
        g_from_helper.append(buf, (size_t)r);
        if (g_from_helper.size() > 262144) g_from_helper.clear();   // a whole queue fits in far less
    }
    if (r == 0) {
        plat_close(g_sock);
        g_sock = -1;
        g_playing_sent = false;
        g_from_helper.clear();
        g_chat_active = false;
        capture_stop();
        say("lost the connection to cstvd");
        return;
    }
    size_t nl;
    while ((nl = g_from_helper.find('\n')) != std::string::npos) {
        std::string line = g_from_helper.substr(0, nl);
        g_from_helper.erase(0, nl + 1);
        int v;
        if (!line.compare(0, 4, "srv ")) {
            on_master(line.c_str() + 4);
        } else if (!line.compare(0, 8, "pscreen ")) {   // pscreen <server>/<map> <geometry|none>
            size_t sp = line.find(' ', 8);
            if (sp != std::string::npos && !g_loc.empty() && line.compare(8, sp - 8, g_loc) == 0) g_pscr = parse_screen(line.c_str() + sp + 1);
        } else if (sscanf(line.c_str(), "pref vol %d", &v) == 1) {
            g_user_vol = std::max(0, std::min(100, v));
        } else if (sscanf(line.c_str(), "pref universal %d", &v) == 1) {
            g_universal = v != 0;
        } else if (sscanf(line.c_str(), "chat %d", &v) == 1) {
            g_chat_active = v != 0;
            g_chat_panel = v == 1;
            g_last_bind_check = 0;   // show / hide the panel right away
        } else if (sscanf(line.c_str(), "master %d", &v) == 1) {
            say(v ? "connected to cstv.xyz" : "no connection to cstv.xyz");
        } else if (sscanf(line.c_str(), "capture %d", &v) == 1) {
            if (v) capture_start();
            else capture_stop();
        }
        gui_on_line(line.c_str());
    }
}

// ---------------------------------------------------------------- game interfaces
static IBaseClientDLL *g_client;
static IMaterialSystem *g_matsys;
static IVEngineClient *g_engine;
static IVRenderView *g_render;
static IEngineTrace *g_trace;
static ICvar *g_cvar;

static bool get_view(Vector &org, QAngle &ang)
{
    CViewSetup v;
    if (!g_client->GetPlayerView(v)) return false;
    org = v.origin;
    ang = v.angles;
    return true;
}

static bool trace_world(const Vector &from, const Vector &to, trace_t &tr)
{
    Ray_t ray;
    ray.Init(from, to);
    CTraceFilterWorldOnly filter;
    g_trace->TraceRay(ray, MASK_SOLID, &filter, &tr);
    return tr.fraction < 1.f;
}

// Who we are and where (cstvd passes `where` on to the master, which puts us in that server's room).
// Player info only exists in game, so `ident` waits until then. A listen server's address is
// "loopback" for its host: that becomes a room of its own (local-<steamid>), so strangers playing
// offline don't end up sharing one room.
static void update_where()
{
    if (!g_engine) return;
    std::string where = "- -", loc;
    if (g_engine->IsInGame()) {
        player_info_t pi;
        unsigned long long id = 0;
        if (g_engine->GetPlayerInfo(g_engine->GetLocalPlayer(), &pi)) {
            pi.name[sizeof pi.name - 1] = 0;
            id = pi.friendsID ? 76561197960265728ULL + pi.friendsID : 0;
            char ident[128];
            snprintf(ident, sizeof ident, "ident %llu %s", id, pi.name[0] ? b64encode(pi.name).c_str() : "-");
            if (g_ident_sent != ident && helper_send("%s\n", ident)) g_ident_sent = ident;
        }
        INetChannelInfo *ch = g_engine->GetNetChannelInfo();
        const char *addr = ch ? ch->GetAddress() : nullptr;
        std::string map = g_engine->GetLevelName();   // "maps/<name>.bsp"
        if (!map.compare(0, 5, "maps/")) map.erase(0, 5);
        if (map.size() > 4 && !map.compare(map.size() - 4, 4, ".bsp")) map.resize(map.size() - 4);
        for (auto &c : map)
            if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') c = '_';
        std::string server = addr && !strncmp(addr, "loopback", 8) ? "local-" + std::to_string(id) : addr ? addr : "";
        bool server_ok = server.size() >= 3 && server.size() <= 64;
        for (char c : server)
            if (!isalnum((unsigned char)c) && !strchr(".:[]_-", c)) server_ok = false;
        if (server_ok && !map.empty() && map.size() <= 64) {
            where = server + " " + map;
            loc = server + "/" + map;
        }
    }
    if (loc != g_loc) {
        g_loc = loc;
        g_pscr = Screen();
    }
    if (where != g_where_sent && helper_send("m where %s\n", where.c_str())) {
        g_where_sent = where;
        if (!g_loc.empty()) helper_send("pscreen %s\n", g_loc.c_str());   // ask for the personal screen here
    }
}

// ---------------------------------------------------------------- chat output
// A chat line for this player only: a SayText user message (id 3 in CS:S: 4th registered in client.so's
// RegisterUserMessages, after Geiger, Train, HudText) handed to the client as if the server sent it.
// bWantsToChat = 0 prints it through the HUD chat's Printf (no Steam text filter on it).
enum { USERMSG_SAYTEXT = 3 };

void cstv_print(const char *text)
{
    std::string s = std::string(" \x04[CS:TV]\x01 ") + text;
    if (s.size() > 250) s.resize(250);
    for (auto &c : s)
        if (c == '\n' || c == '\r') c = ' ';
    if (!g_engine || !g_client || !g_engine->IsInGame()) { say("%s", text); return; }
    alignas(8) unsigned char data[272] = {0};   // bf_read reads whole words: padded
    size_t n = 0;
    data[n++] = 0;   // from: nobody
    memcpy(data + n, s.c_str(), s.size() + 1);
    n += s.size() + 1;
    data[n++] = 0;   // bWantsToChat
    // bf_read's constructors live in tier1's bitbuf.cpp; zeroed + its public fields = the same object
    alignas(bf_read) unsigned char raw[sizeof(bf_read)] = {0};
    bf_read *msg = reinterpret_cast<bf_read *>(raw);
    msg->m_pData = data;
    msg->m_nDataBytes = (int)n;
    msg->m_nDataBits = (int)n * 8;
    msg->m_iCurBit = 0;
    g_client->DispatchUserMessage(USERMSG_SAYTEXT, *msg);
}

// ---------------------------------------------------------------- textures
static ITexture *g_tex;
static IMaterial *g_screen_mat, *g_chat_mat;   // materials/cstv/{screen,chat}.vmt (in cstrike/custom/cstv)
static IMaterial *g_preview_mat, *g_outline_mat;   // place mode: see-through video, plain colour
static IMaterial *g_chat_see_mat;   // the chat panel see-through, for screens below 100 % opacity (preview.vmt is the video's)
static uint64_t g_uploaded_seq;

class Regen : public ITextureRegenerator {
public:
    void RegenerateTextureBits(ITexture *, IVTFTexture *vtf, Rect_t *) override
    {
        cstv_shm *s = g_shm;
        if (!s || vtf->Width() != CSTV_W || vtf->Height() != CSTV_H) return;
        if (s->version != CSTV_VERSION || s->w != CSTV_W || s->h != CSTV_H) return;   // a cstvd of another build took over the file
        uint32_t idx = __atomic_load_n(&s->latest, __ATOMIC_ACQUIRE);
        if (idx > 2) return;
        __atomic_store_n(&s->reading, idx, __ATOMIC_RELEASE);   // the helper won't write this buffer now
        memcpy(vtf->ImageData(), s->buf[idx], CSTV_FRAME_BYTES);
        __atomic_store_n(&s->reading, CSTV_NONE, __ATOMIC_RELEASE);
    }
    void Release() override {}
};
static Regen g_regen;

static void ensure_texture()
{
    if (g_tex) return;
    int flags = TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD | TEXTUREFLAGS_PROCEDURAL | TEXTUREFLAGS_SINGLECOPY |
                TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT;
    ITexture *t = g_matsys->CreateProceduralTexture("_rt_cstv_video", TEXTURE_GROUP_OTHER, CSTV_W, CSTV_H, IMAGE_FORMAT_DXT1, flags);
    if (!t || t->GetActualWidth() != CSTV_W || t->GetActualHeight() != CSTV_H || t->GetImageFormat() != IMAGE_FORMAT_DXT1) {
        say("could not create the video texture; CS:TV stays off");
        return;
    }
    t->SetTextureRegenerator(&g_regen);
    // Hold our own reference: the engine frees textures nobody references when a map loads
    // (CreateProceduralTexture hands it out with none), and attaching a freed one crashes.
    t->IncrementReferenceCount();
    g_tex = t;
}

static IMaterial *find_material(const char *name)
{
    IMaterial *m = g_matsys->FindMaterial(name, TEXTURE_GROUP_OTHER, false);
    if (!m || m->IsErrorMaterial()) {
        say("materials/%s.vmt is missing (make install GAME=... in cstv/client, or start the game with cstv-launch)", name);
        return nullptr;
    }
    m->IncrementReferenceCount();   // ours for good: never freed on map changes
    return m;
}

// ---- chat panel: drawn by cstv_overlay.py into /tmp/cstv/chat (BGRX, triple-buffered)
static cstv_chat_shm *g_chat;
static ITexture *g_chat_tex;
static uint64_t g_chat_uploaded_seq;
static bool g_chat_shown;   // the panel has our texture and is drawn

static void map_chat()
{
    if (g_chat) return;
    void *p = plat_map(CSTV_CHAT_PATH, sizeof(cstv_chat_shm));
    if (!p) return;
    cstv_chat_shm *c = (cstv_chat_shm *)p;
    if (c->magic != CSTV_CHAT_MAGIC || c->w != CSTV_CHAT_W || c->h != CSTV_CHAT_H) { plat_unmap(p, sizeof(cstv_chat_shm)); return; }
    g_chat = c;
}

class ChatRegen : public ITextureRegenerator {
public:
    void RegenerateTextureBits(ITexture *, IVTFTexture *vtf, Rect_t *) override
    {
        cstv_chat_shm *c = g_chat;
        if (!c || vtf->Width() != CSTV_CHAT_W || vtf->Height() != CSTV_CHAT_H) return;
        uint32_t idx = __atomic_load_n(&c->latest, __ATOMIC_ACQUIRE);
        if (idx > 2) return;
        __atomic_store_n(&c->reading, idx, __ATOMIC_RELEASE);
        unsigned char *dst = vtf->ImageData();
        int stride = vtf->RowSizeInBytes(0);
        for (int y = 0; y < CSTV_CHAT_H; y++) memcpy(dst + (size_t)y * stride, c->buf[idx] + (size_t)y * CSTV_CHAT_W * 4, CSTV_CHAT_W * 4);
        __atomic_store_n(&c->reading, CSTV_NONE, __ATOMIC_RELEASE);
    }
    void Release() override {}
};
static ChatRegen g_chat_regen;

static void ensure_chat_texture()
{
    if (g_chat_tex) return;
    int flags = TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD | TEXTUREFLAGS_PROCEDURAL | TEXTUREFLAGS_SINGLECOPY |
                TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT;
    ITexture *t = g_matsys->CreateProceduralTexture("_rt_cstv_chat", TEXTURE_GROUP_OTHER, CSTV_CHAT_W, CSTV_CHAT_H, IMAGE_FORMAT_BGRX8888, flags);
    if (!t || t->GetActualWidth() != CSTV_CHAT_W || t->GetActualHeight() != CSTV_CHAT_H) { say("could not create the chat texture"); return; }
    t->SetTextureRegenerator(&g_chat_regen);
    t->IncrementReferenceCount();   // see ensure_texture
    g_chat_tex = t;
}

// Remember a texture we may put back later, holding a reference so the engine can't free it meanwhile
// (while our video texture is attached, nothing else references the placeholder).
static void keep_ref(ITexture *&slot, ITexture *tex)
{
    tex->IncrementReferenceCount();
    if (slot) slot->DecrementReferenceCount();
    slot = tex;
}

// Point the material at `want` (our texture) or back at its own placeholder texture. Checked about once a
// second, not just once: a material reload (mat_reloadallmaterials, some config changes) puts the
// vmt's $basetexture back.
static void bind_texture(IMaterial *m, ITexture *want, ITexture *ours, ITexture *&placeholder)
{
    bool found = false;
    IMaterialVar *v = m ? m->FindVar("$basetexture", &found, false) : nullptr;
    if (!found || !v) return;
    ITexture *cur = v->GetTextureValue();
    if (cur && cur != ours && cur != placeholder) keep_ref(placeholder, cur);
    if (want && cur != want) v->SetTextureValue(want);
    else if (!want && cur == ours && placeholder) v->SetTextureValue(placeholder);
}

static void keep_bound(double t, bool force = false)
{
    if (!g_tex || (!force && t - g_last_bind_check < 1.0)) return;
    g_last_bind_check = t;
    static ITexture *placeholder, *preview_placeholder, *chat_placeholder, *chat_see_placeholder;
    // video, or cstvd's own "Loading..." / "Nothing playing"; no cstvd: the placeholder
    bool want_video = g_on.load() && g_sock >= 0 && g_shm && g_shm->latest <= 2;
    bind_texture(g_screen_mat, want_video ? g_tex : nullptr, g_tex, placeholder);
    bind_texture(g_preview_mat, want_video ? g_tex : nullptr, g_tex, preview_placeholder);
    if (!g_chat) map_chat();
    ensure_chat_texture();
    g_chat_shown = g_chat_tex && g_chat && g_chat->latest <= 2 && g_chat_active && g_chat_mat;
    bind_texture(g_chat_mat, g_chat_shown ? g_chat_tex : nullptr, g_chat_tex, chat_placeholder);
    bind_texture(g_chat_see_mat, g_chat_shown ? g_chat_tex : nullptr, g_chat_tex, chat_see_placeholder);
}

// Upload on the render thread: the main thread only queues this job; the render thread runs it.
class UploadJob : public CFunctor {
    std::atomic<int> m_refs{1};
    ITexture *m_tex;
public:
    explicit UploadJob(ITexture *t) : m_tex(t) {}
    int AddRef() override { return ++m_refs; }
    int Release() override { int r = --m_refs; if (!r) delete this; return r; }
    void operator()() override { m_tex->Download(); }
};

static void upload(ITexture *tex)
{
    CMatRenderContextPtr ctx(g_matsys);
    if (ICallQueue *q = ctx->GetCallQueue()) {
        UploadJob *job = new UploadJob(tex);
        q->QueueFunctor(job);   // takes its own reference
        job->Release();
    } else {
        tex->Download();        // mat_queue_mode 0: no render thread, upload directly (~0.1 ms)
    }
}

// ---------------------------------------------------------------- drawing
static const float CHAT_W = 192;   // chat panel width at scale 1 (its height is the screen's)

static Vector chat_center(const Screen &s, const Vector &right)
{
    return s.pos + right * (s.w() / 2 + CHAT_W * s.scale / 2 + 2);
}

// CMeshBuilder::End calls these (tier2's meshutils.cpp, not in the SDK); only quads are drawn here.
void GenerateQuadIndexBuffer(unsigned short *idx, int n, int first)
{
    for (int i = 0; i + 6 <= n; i += 6, first += 4) {
        const unsigned short q[6] = {(unsigned short)first, (unsigned short)(first + 1), (unsigned short)(first + 2),
                                     (unsigned short)first, (unsigned short)(first + 2), (unsigned short)(first + 3)};
        memcpy(idx + i, q, sizeof q);
    }
}
void GenerateSequentialIndexBuffer(unsigned short *idx, int n, int first)
{
    for (int i = 0; i < n; i++) idx[i] = (unsigned short)(first + i);
}
void GeneratePolygonIndexBuffer(unsigned short *idx, int n, int first)
{
    for (int i = 0, v = 1; i + 3 <= n; i += 3, v++) {
        idx[i] = (unsigned short)first;
        idx[i + 1] = (unsigned short)(first + v);
        idx[i + 2] = (unsigned short)(first + v + 1);
    }
}
void GenerateLineStripIndexBuffer(unsigned short *idx, int n, int first)
{
    for (int i = 0, v = 0; i + 2 <= n; i += 2, v++) {
        idx[i] = (unsigned short)(first + v);
        idx[i + 1] = (unsigned short)(first + v + 1);
    }
}
void GenerateLineLoopIndexBuffer(unsigned short *idx, int n, int first)
{
    GenerateLineStripIndexBuffer(idx, n - 2, first);
    if (n >= 2) {
        idx[n - 2] = (unsigned short)(first + n / 2 - 1);
        idx[n - 1] = (unsigned short)first;
    }
}

// The colour only shows with materials that have $vertexcolor / $vertexalpha (preview, outline).
static void draw_quad(IMatRenderContext *ctx, IMaterial *mat, const Vector &c, const Vector &r, const Vector &u, float w, float h,
                      int cr = 255, int cg = 255, int cb = 255, int ca = 255)
{
    ctx->Bind(mat);
    IMesh *mesh = ctx->GetDynamicMesh(true);
    CMeshBuilder mb;
    mb.Begin(mesh, MATERIAL_QUADS, 1);
    static const float corner[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};   // top left, top right, bottom right, bottom left
    for (auto &k : corner) {
        Vector p = c + r * (k[0] * w / 2) + u * (k[1] * h / 2);
        mb.Position3f(p.x, p.y, p.z);
        mb.Color4ub((unsigned char)cr, (unsigned char)cg, (unsigned char)cb, (unsigned char)ca);
        mb.TexCoord2f(0, (k[0] + 1) / 2, (1 - k[1]) / 2);
        mb.AdvanceVertex();
    }
    mb.End();
    mesh->Draw();
}

// The vmts have $nocull, so we pick the side ourselves: a wall screen only from the front; a floating
// screen from behind uses the mirrored right axis, so the picture reads the right way round there too.
// Below 100 % opacity (Screen tab) the see-through materials draw it, faded by the vertex alpha.
static void draw_world_screen(IMatRenderContext *ctx, const Screen &s, const Vector &eye)
{
    if (!s.on) return;
    Vector right = s.right;
    if ((eye - s.pos).Dot(s.normal) < 0) {
        if (!s.floating) return;
        right = -right;
    }
    int a = gui_screen_alpha();
    bool see = a < 255 && g_preview_mat;
    draw_quad(ctx, see ? g_preview_mat : g_screen_mat, s.pos, right, s.up, s.w(), s.h(), 255, 255, 255, a);
    if (g_chat_shown && g_chat_panel)
        draw_quad(ctx, see && g_chat_see_mat ? g_chat_see_mat : g_chat_mat, chat_center(s, right), right, s.up, CHAT_W * s.scale, s.h(),
                  255, 255, 255, a);
}

// ---------------------------------------------------------------- placing screens, clicking them
static const float FLOAT_DIST = 300, FLOAT_MIN = 48, FLOAT_MAX = 3000, CLICK_RANGE = 2048;

static V3 v3(const Vector &v) { return {v.x, v.y, v.z}; }
static Vector vec(const V3 &v) { return Vector(v.x, v.y, v.z); }

static Screen to_screen(const Placement &p)
{
    Screen s = {};
    s.on = true;
    s.floating = p.floating;
    s.scale = p.scale;
    s.pos = vec(p.pos);
    s.normal = vec(p.normal);
    s.right = vec(p.right);
    s.up = vec(p.up);
    return s;
}

static bool send_m(const char *fmt, ...)
{
    char buf[3000];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0 || n >= (int)sizeof buf) return false;
    if (helper_send("m %s\n", buf)) return true;
    cstv_print("cstvd isn't running, so CS:TV can't do that right now.");
    return false;
}

static float g_place_wait_scale;

static bool send_shared(const Screen &s)
{
    if (!send_m("place %s", screen_line(s).c_str())) return false;
    g_place_wait = now_s();
    g_place_wait_scale = s.scale;
    std::string m = std::string("Shared screen placed. Only ") + gui_shared_audience() + " can see it.";
    cstv_print(m.c_str());
    return true;
}

// The old plugin's PlaceScreen, straight away (!tv large, !tv float, !link with no screen): on the
// wall / floor / ceiling the view aims at (world only), or floating `dist` units along the aim (or just
// short of what it hits), facing the player.
static bool place_screen(float scale, bool floating, float dist)
{
    Vector eye, fwd;
    QAngle ang;
    if (!g_engine->IsInGame() || !get_view(eye, ang)) { cstv_print("Join a server first."); return false; }
    angle_vectors(ang, &fwd, nullptr, nullptr);
    trace_t tr;
    bool hit = trace_world(eye, eye + fwd * 32768, tr);
    Placement p;
    p.scale = scale;
    p.floating = floating;
    if (floating) {
        dist = std::max(FLOAT_MIN, std::min(FLOAT_MAX, dist));
        if (hit) dist = std::min(dist, (tr.endpos - eye).Length() - 8);
        if (dist < FLOAT_MIN) { cstv_print("Too close to a wall to float a screen: aim at open space."); return false; }
        float_axes(ang.x, ang.y, p.normal, p.right, p.up);
        p.pos = v3(eye + fwd * dist);
    } else {
        if (!hit || tr.startsolid) { cstv_print("Aim at a wall, floor or ceiling (or add \x04" "float\x01 to hang it in the air)."); return false; }
        p.normal = v3(tr.plane.normal);
        surface_axes(p.normal, ang.y, p.right, p.up);
        p.pos = v3(tr.endpos) + p.normal * 2;
    }
    return send_shared(to_screen(p));
}

// Place mode: a preview follows the aim every frame until the player places it (E / Enter) or cancels
// (Esc). The GUI's SDL filter turns the keys into cstv_place_event calls and draws the hint bar.
static struct {
    bool on, personal, want_float;
    bool confirm, cancel;                    // keys seen by the SDL filter, acted on in main_tick
    float scale = 0.5f, dist = FLOAT_DIST;   // what the player asked for; kept for next time
    bool valid, ok;                          // the preview was worked out this frame / it fits
    Screen cur;
} g_place;

static float trace_free(const V3 &a, const V3 &b)
{
    trace_t tr;
    trace_world(vec(a), vec(b), tr);
    return tr.startsolid ? 0.f : tr.fraction;
}

// Floating at the chosen distance, facing the player; flat on what the aim hits instead when that's no
// further than the floating screen would be (+ PLACE_SNAP), so a far wall never grabs it. Sky and
// invisible brushes only stop a floating screen short of them. Then fitted around the geometry nearby.
static const float PLACE_SNAP = 64;

static void update_preview(const Vector &eye, const QAngle &ang)
{
    Vector fwd;
    angle_vectors(ang, &fwd, nullptr, nullptr);
    trace_t tr;
    bool hit = trace_world(eye, eye + fwd * FLOAT_MAX, tr) && !tr.startsolid;
    float hit_dist = hit ? (tr.endpos - eye).Length() : 1e9f;
    bool surface = hit && !(tr.surface.flags & (SURF_SKY | SURF_SKY2D | SURF_NODRAW));
    Placement p;
    p.scale = g_place.scale;
    p.floating = g_place.want_float || !surface || hit_dist > g_place.dist + PLACE_SNAP;
    bool ok = true;
    if (p.floating) {
        float dist = std::min(g_place.dist, hit_dist - 8);
        ok = dist >= FLOAT_MIN;
        float_axes(ang.x, ang.y, p.normal, p.right, p.up);
        p.pos = v3(eye + fwd * std::max(dist, 1.f));
    } else {
        p.normal = v3(tr.plane.normal);
        surface_axes(p.normal, ang.y, p.right, p.up);
        p.pos = v3(tr.endpos) + p.normal * 2;
    }
    ok = fit_screen(p, PLACE_SCALE_MIN, trace_free) && ok;
    g_place.cur = to_screen(p);
    g_place.ok = ok;
    g_place.valid = true;
}

// See-through video (or the placeholder) with a green outline where it fits, red where it doesn't.
static void draw_preview(IMatRenderContext *ctx, const Vector &eye)
{
    if (!g_place.valid) return;
    const Screen &s = g_place.cur;
    Vector right = (eye - s.pos).Dot(s.normal) < 0 ? -s.right : s.right;
    int tint = g_place.ok ? 255 : 120;
    draw_quad(ctx, g_preview_mat ? g_preview_mat : g_screen_mat, s.pos, right, s.up, s.w(), s.h(), 255, tint, tint, 150);
    if (!g_outline_mat) return;
    float w = s.w(), h = s.h(), t = std::max(1.5f, w * 0.012f);
    int r = g_place.ok ? 70 : 235, g = g_place.ok ? 220 : 60, b = g_place.ok ? 110 : 50;
    draw_quad(ctx, g_outline_mat, s.pos + s.up * (h / 2 + t / 2), right, s.up, w + 2 * t, t, r, g, b);
    draw_quad(ctx, g_outline_mat, s.pos - s.up * (h / 2 + t / 2), right, s.up, w + 2 * t, t, r, g, b);
    draw_quad(ctx, g_outline_mat, s.pos + right * (w / 2 + t / 2), right, s.up, t, h, r, g, b);
    draw_quad(ctx, g_outline_mat, s.pos - right * (w / 2 + t / 2), right, s.up, t, h, r, g, b);
}

static void draw_screens()
{
    if (!g_screen_mat || !g_on.load() || (!g_scr.on && !g_pscr.on && !g_place.on)) return;
    Vector eye;
    QAngle ang;
    if (!get_view(eye, ang)) return;
    if (g_place.on) update_preview(eye, ang);
    CMatRenderContextPtr ctx(g_matsys);
    ctx->MatrixMode(MATERIAL_MODEL);   // whatever was drawn last may have left its own model matrix
    ctx->PushMatrix();
    ctx->LoadIdentity();
    draw_world_screen(ctx, g_scr, eye);
    draw_world_screen(ctx, g_pscr, eye);
    if (g_place.on) draw_preview(ctx, eye);
    ctx->MatrixMode(MATERIAL_MODEL);
    ctx->PopMatrix();
}

static const char NO_HELPER_PERSONAL[] = "cstvd isn't running, so CS:TV can't save a personal screen right now.";

void cstv_place_begin(bool personal)
{
    if (!g_engine->IsInGame() || g_loc.empty()) { cstv_print("Join a server first."); return; }
    if (gui_open()) gui_toggle();
    g_place.on = true;
    g_place.personal = personal;
    g_place.want_float = false;
    g_place.valid = g_place.confirm = g_place.cancel = false;
}

static void place_confirm()
{
    if (!g_place.valid) return;
    if (!g_place.ok) {
        cstv_print("It doesn't fit there (red outline): aim somewhere else, or make it smaller with the mouse wheel.");
        return;
    }
    const Screen &s = g_place.cur;
    if (g_place.personal) {
        if (!helper_send("pscreen %s %s\n", g_loc.c_str(), screen_line(s).c_str())) { cstv_print(NO_HELPER_PERSONAL); return; }
        g_pscr = s;
        cstv_print("Personal screen placed. Only you can see it; it's kept for this server and map.");
    } else if (!send_shared(s)) {
        return;
    }
    g_place.on = false;
}

void cstv_place_event(int ev, int n)
{
    if (!g_place.on) return;
    switch (ev) {
    case PLACE_SIZE: g_place.scale = std::max(PLACE_SCALE_MIN, std::min(PLACE_SCALE_MAX, g_place.scale * powf(1.1f, (float)n))); break;
    case PLACE_DIST: g_place.dist = std::max(FLOAT_MIN, std::min(FLOAT_MAX, g_place.dist * powf(1.25f, (float)n))); break;
    case PLACE_FLOAT: g_place.want_float = !g_place.want_float; break;
    case PLACE_KIND: g_place.personal = !g_place.personal; break;
    case PLACE_CONFIRM: g_place.confirm = true; break;
    case PLACE_CANCEL: g_place.cancel = true; break;
    }
}

bool cstv_place_hint(PlaceHint *h)
{
    if (!g_place.on) return false;
    if (h) {
        h->personal = g_place.personal;
        h->floating = g_place.valid ? g_place.cur.floating : g_place.want_float;
        h->want_float = g_place.want_float;
        h->ok = !g_place.valid || g_place.ok;
        h->scale = g_place.valid ? g_place.cur.scale : g_place.scale;
        h->want_scale = g_place.scale;
        h->dist = g_place.dist;
    }
    return true;
}

bool cstv_personal_screen(float *scale, bool *floating)
{
    if (!g_pscr.on) return false;
    *scale = g_pscr.scale;
    *floating = g_pscr.floating;
    return true;
}

void cstv_remove_personal()
{
    if (!g_pscr.on || g_loc.empty()) { cstv_print("You have no personal screen on this map."); return; }
    if (!helper_send("pscreen %s none\n", g_loc.c_str())) { cstv_print(NO_HELPER_PERSONAL); return; }
    g_pscr = Screen();
    cstv_print("Personal screen removed.");
}

// Port of AimsAtScreen: the aim crosses the screen's rectangle, with no wall in between.
static bool aims_at_screen(const Screen &s)
{
    Vector eye, fwd;
    QAngle ang;
    if (!s.on || !get_view(eye, ang)) return false;
    angle_vectors(ang, &fwd, nullptr, nullptr);
    Vector n = s.normal;
    float denom = fwd.Dot(n);
    if (denom > -0.01f) {   // looking away from the screen's front
        if (!s.floating || denom < 0.01f) return false;
        n = -n;             // a floating screen works from behind too
        denom = -denom;
    }
    float t = (s.pos - eye).Dot(n) / denom;
    if (t < 0 || t > CLICK_RANGE) return false;
    Vector hit = eye + fwd * t, rel = hit - s.pos;
    if (fabsf(rel.Dot(s.right)) > s.w() / 2 || fabsf(rel.Dot(s.up)) > s.h() / 2) return false;
    trace_t tr;
    return !trace_world(eye, hit + n * 4, tr);
}

// +attack / +use, or a bind that starts with one of them
static bool is_click_binding(const char *b)
{
    for (const char *k : {"+attack", "+use"}) {
        size_t n = strlen(k);
        if (!strncasecmp(b, k, n) && (!b[n] || b[n] == ';' || b[n] == ' ')) return true;
    }
    return false;
}

static void on_click()
{
    static double last;
    double t = now_s();
    if (!g_on.load() || g_url.empty() || !g_engine->IsInGame() || g_engine->Con_IsVisible() || gui_open() || g_place.on || t - last < 0.3) return;
    if (!aims_at_screen(g_scr) && !aims_at_screen(g_pscr)) return;
    last = t;
    send_m("pause");
}

// ---------------------------------------------------------------- commands
static void open_gui()
{
    if (!gui_open()) gui_toggle();
}

static void print_help()
{
    cstv_print("\x04!tv\x01 place the shared screen (only your room sees it), \x04!tv personal\x01 one only you see, \x04!tv hud\x01 video on your HUD");
    cstv_print("\x04!tv small|medium|large|huge|0.1-4\x01 [\x04" "float\x01 [distance]] shared screen right away, \x04!tv remove\x01 [\x04personal\x01]");
    cstv_print("\x04!link <url>\x01 play, \x04!queue <url>\x01, \x04!queuemenu\x01, \x04!clearqueue\x01, \x04!tvnext\x01, \x04!tvprev");
    cstv_print("\x04!tvpause\x01 (or shoot / use the screen), \x04!tv +30\x01 / \x04-10\x01 / \x04" "1:23\x01 seek, \x04!tvvol 0-100\x01 your volume");
    cstv_print("\x04!join <code>\x01 / \x04!leave\x01 / \x04!lobby create\x01 watch with friends anywhere (also at \x04" "cstv.xyz\x01), \x04!tvchat <text>\x01 lobby chat");
    cstv_print("\x04!tvlogin <code>\x01 sign in (code from \x04" "cstv.xyz/link\x01), \x04!tvlogout\x01, \x04!tvstatus\x01, \x04!cstv\x01 the CS:TV window");
}

static bool lobby_code_ok(const std::string &c)
{
    return c.size() == 6 && std::all_of(c.begin(), c.end(), [](char ch) { return isalpha((unsigned char)ch) || (ch >= '2' && ch <= '9'); });
}

// !tvlogin <code>: the code the website shows under "Connect game" (cstv.xyz/link)
static void login_cmd(const std::string &arg)
{
    std::string code = trim(arg);
    for (auto &c : code) c = (char)toupper((unsigned char)c);
    if (code.size() < 4 || code.size() > 8 || !std::all_of(code.begin(), code.end(), [](char c) { return isalnum((unsigned char)c); })) {
        cstv_print("Usage: \x04!tvlogin <code>\x01, with the code from \x04" "cstv.xyz/link\x01 (sign in with Steam there, then \"Connect game\").");
        return;
    }
#ifdef _WIN32
    send_m("login %s windows", code.c_str());
#else
    send_m("login %s linux", code.c_str());
#endif
}

static void tvchat_cmd(const std::string &arg)
{
    std::string text = trim(arg);
    if (text.empty()) { cstv_print("Usage: \x04!tvchat <text>\x01 (the lobby's chat, also on cstv.xyz)"); return; }
    if (text.size() > 300) text.resize(300);
    send_m("chat %s", b64encode(text).c_str());
}

static bool link_cmd(const char *verb, const std::string &arg)
{
    std::string url = trim(arg);
    if (url.empty()) { cstv_print(!strcmp(verb, "link") ? "Usage: \x04!link <url>" : "Usage: \x04!queue <url>"); return false; }
    if (!url_ok(url)) { cstv_print("That doesn't look like a link (http://, https://)."); return false; }
    // someone has to see it: a shared screen where the player aims, unless they watch on their own one
    if (!strcmp(verb, "link") && !g_scr.on && !g_pscr.on && !gui_hud_on() && !place_screen(0.5f, false, FLOAT_DIST)) return false;
    return send_m("%s %s", verb, b64encode(url).c_str());
}

static float size_word(const std::string &s)
{
    return s == "small" ? 0.25f : s == "medium" ? 0.5f : s == "large" ? 0.75f : s == "huge" ? 1.0f : 0;
}

static bool scale_word(const std::string &s, float &scale)
{
    float k = size_word(s);
    if (!k && is_number(s) && atof(s.c_str()) >= PLACE_SCALE_MIN && atof(s.c_str()) <= PLACE_SCALE_MAX) k = (float)atof(s.c_str());
    if (k) scale = k;
    return k != 0;
}

// !tv, !tv personal [size], cstv place [personal|shared] [size]: place mode, starting at that size
static void place_cmd(const std::vector<std::string> &w, bool personal)
{
    for (auto &word : w) {
        std::string s = lower(word);
        if (s == "personal" || s == "mine" || s == "private") personal = true;
        else if (s == "shared") personal = false;
        else if (s != "place" && !scale_word(s, g_place.scale)) {
            cstv_print("Usage: \x04!tv\x01 or \x04!tv personal\x01, optionally with a size (small|medium|large|huge|0.1-4)");
            return;
        }
    }
    cstv_place_begin(personal);
}

static void remove_cmd(const std::string &arg)
{
    if (lower(trim(arg)) == "personal") cstv_remove_personal();
    else send_m("remove");
}

static void tv_cmd(const std::string &rest)
{
    std::vector<std::string> w = words(rest);
    std::string first = w.empty() ? "" : lower(w[0]);
    if (w.empty() || first == "personal" || first == "place") { place_cmd(w, false); return; }
    if (first == "hud") { gui_hud_toggle(); return; }
    if (first == "remove" || first == "off" || first == "clear" || first == "delete") { remove_cmd(w.size() > 1 ? w[1] : ""); return; }
    if (first == "link" || first == "play") { link_cmd("link", rest.substr(rest.find(w[0]) + w[0].size())); return; }
    // !tv +30 / -10 / 1:23
    std::string seek;
    for (char c : rest)
        if (c != ' ') seek += c;
    if (!seek.empty() && (seek[0] == '+' || seek[0] == '-' || (isdigit((unsigned char)seek[0]) && seek.find(':') != std::string::npos))) {
        if (seek.size() > 12 || seek.find_first_not_of("+-0123456789:.") != std::string::npos) { cstv_print("Seek with \x04!tv +30\x01, \x04-10\x01 or \x04" "1:23"); return; }
        send_m("seek %s", seek.c_str());
        return;
    }
    // !tv [size] [float [distance]], words in any order
    float scale = 0.5f, dist = FLOAT_DIST;
    bool floating = false;
    for (auto &word : w) {
        std::string s = lower(word);
        if (s == "float" || s == "floating" || s == "air") floating = true;
        else if (is_number(s) && floating) dist = (float)atof(s.c_str());
        else if (!scale_word(s, scale)) {
            cstv_print("Usage: \x04!tv\x01 (place mode), \x04!tv personal\x01, \x04!tv hud\x01, \x04!tv small|medium|large|huge\x01 or a size 0.1-4 "
                       "(add \x04" "float\x01 to hang it in the air), \x04!tv link <url>\x01, \x04!tv remove\x01 [\x04personal\x01], "
                       "\x04!tv +30\x01 / \x04-10\x01 / \x04" "1:23\x01. All commands: \x04!tvhelp");
            return;
        }
    }
    place_screen(scale, floating, dist);
}

static bool all_digits(const std::string &s) { return !s.empty() && s.size() <= 20 && s.find_first_not_of("0123456789") == std::string::npos; }

static void lobby_cmd(const std::string &rest)
{
    std::vector<std::string> w = words(rest);
    std::string sub = w.empty() ? "" : lower(w[0]), arg = w.size() > 1 ? w[1] : "";
    auto code_ok = [](const std::string &c) {
        return c.size() == 6 && std::all_of(c.begin(), c.end(), [](char ch) { return isalpha((unsigned char)ch) || (ch >= '2' && ch <= '9'); });
    };
    if (sub == "create" || sub == "new") send_m("lobby create");
    else if (sub == "leave") send_m("lobby leave");
    else if (sub == "join" && code_ok(arg)) send_m("lobby join %s", arg.c_str());
    else if (w.size() == 1 && code_ok(w[0])) send_m("lobby join %s", w[0].c_str());
    else if ((sub == "kick" || sub == "ban" || sub == "owner" || sub == "unban") && all_digits(arg)) send_m("lobby %s %s", sub.c_str(), arg.c_str());
    else if (sub == "perm" && (arg == "all" || arg == "owner")) send_m("lobby perm %s", arg.c_str());
    else cstv_print("\x04!lobby create\x01, \x04!lobby <code>\x01, \x04!lobby leave\x01; owner: \x04kick|ban|owner <id>\x01, \x04unban <steamid>\x01, \x04perm all|owner\x01 (ids: \x04!cstv\x01)");
}

static bool is_command(const std::string &verb, const std::string &arg);

// One CS:TV command, without its ! or /. false = not ours.
static bool run_command(const std::string &verb, const std::string &rest)
{
    std::string arg = trim(rest);
    if (!is_command(verb, arg)) return false;
    if (!g_on.load() && verb != "tvhelp") {
        cstv_print("CS:TV is off: turn it on with F9 (or in bhoptools' Fixes tab).");
        return true;
    }
    if (verb == "tv") tv_cmd(arg);
    else if (verb == "link" || verb == "tvlink") link_cmd("link", arg);
    else if ((verb == "queue" || verb == "tvqueue") && !arg.empty()) link_cmd("queue", arg);
    else if (verb == "queue" || verb == "tvqueue" || verb == "queuemenu" || verb == "tvset" || verb == "tvsettings" || verb == "tvyt" || verb == "tvttv") open_gui();
    else if (verb == "clearqueue") send_m("clearqueue");
    else if (verb == "tvnext") send_m("next");
    else if (verb == "tvprev") send_m("prev");
    else if (verb == "tvpause") send_m("pause");
    else if (verb == "tvremove") remove_cmd(arg);
    else if (verb == "tvplace") place_cmd(words(arg), false);   // `cstv place [personal]`
    else if (verb == "tvhud") gui_hud_toggle();                   // `cstv hud`
    else if (verb == "tvhelp") print_help();
    else if (verb == "cstv") gui_toggle();
    else if (verb == "lobby") lobby_cmd(arg);
    else if (verb == "join" || verb == "tvjoin") send_m("lobby join %s", arg.c_str());
    else if (verb == "leave" || verb == "tvleave") send_m("lobby leave");
    else if (verb == "tvlogin") login_cmd(arg);
    else if (verb == "tvlogout") send_m("logout");
    else if (verb == "tvstatus") send_m("status");
    else if (verb == "tvchat") tvchat_cmd(arg);
    else if (verb == "tvvol") {
        if (!arg.empty() && all_digits(arg) && atoi(arg.c_str()) <= 100) {
            g_user_vol = atoi(arg.c_str());
            helper_send("pref vol %d\n", g_user_vol);
            char m[64];
            snprintf(m, sizeof m, "Your CS:TV volume: %d", g_user_vol);
            cstv_print(m);
        } else {
            char m[64];
            snprintf(m, sizeof m, "Your CS:TV volume is %d. Change it with \x04!tvvol 0-100", g_user_vol);
            cstv_print(m);
        }
    } else return false;
    return true;
}

// Whether a chat command is CS:TV's. !join and !leave are common words on game servers, so only !join
// with a lobby code, and !leave while in a lobby, are ours; everything else goes to the server.
static bool is_command(const std::string &verb, const std::string &arg)
{
    static const char *const ours[] = {"tv", "link", "tvlink", "queue", "tvqueue", "queuemenu", "tvset", "tvsettings", "tvyt",
                                       "tvttv", "clearqueue", "tvnext", "tvprev", "tvpause", "tvremove", "tvplace", "tvhud", "tvhelp",
                                       "cstv", "lobby", "tvvol", "tvjoin", "tvleave", "tvlogin", "tvlogout", "tvstatus", "tvchat"};
    for (const char *o : ours)
        if (verb == o) return true;
    if (verb == "join") {
        std::string c = trim(arg);
        for (auto &ch : c) ch = (char)toupper((unsigned char)ch);
        return lobby_code_ok(c);
    }
    if (verb == "leave") return gui_in_lobby() && trim(arg).empty();
    return false;
}

// A chat line: ours if it starts with ! or / and one of our commands.
static bool chat_command(std::string text)
{
    text = trim(text);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') text = trim(text.substr(1, text.size() - 2));
    if (text.size() < 2 || (text[0] != '!' && text[0] != '/')) return false;
    size_t e = text.find_first_of(" \t");
    std::string verb = lower(text.substr(1, e == std::string::npos ? std::string::npos : e - 1));
    return run_command(verb, e == std::string::npos ? "" : text.substr(e));
}

// The engine has no client-side say: server.so (loaded in every game process for listen servers)
// registers say / say_team with FCVAR_GAMEDLL, and the engine forwards those to the server before
// dispatching them unless this process hosts the game. So while we're not hosting, ours replace them
// (same flags minus FCVAR_GAMEDLL): CS:TV commands stay here, everything else goes to the server
// unchanged through IVEngineClient::ServerCmd (engine.so: snprintf "cmd %s" + Cmd_ForwardToServer,
// i.e. the raw command line, quotes included; nothing runs locally, so no recursion). While hosting a
// listen server the originals go back: other players' chat on it runs through them. That host can use
// `cstv <command>` in the console instead.
static void say_cb(const CCommand &args)
{
    if (!chat_command(args.ArgS())) g_engine->ServerCmd(args.GetCommandString(), true);
}

static void cstv_cb(const CCommand &args)
{
    std::string rest = trim(args.ArgS());
    if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') rest = trim(rest.substr(1, rest.size() - 2));
    if (rest.empty()) { gui_toggle(); return; }
    size_t e = rest.find_first_of(" \t");
    std::string verb = lower(rest.substr(0, e)), tail = e == std::string::npos ? "" : rest.substr(e);
    if (!verb.empty() && (verb[0] == '!' || verb[0] == '/')) verb.erase(0, 1);
    if (!run_command(verb, tail) && !run_command("tv" + verb, tail)) cstv_print("Unknown CS:TV command. \x04!tvhelp\x01 lists them.");
}

// ConCommand without tier1's convar.cpp: these are the parts of it ConCommand needs, so the class (and
// its vtable) is exactly the SDK's. Checked against client.so's copy: 14 ConCommand slots, name at
// +0x18, flags at +0x28, callback at +0x30, Dispatch = slot 13.
static CVarDLLIdentifier_t g_dll_id = -1;
ConCommandBase::ConCommandBase() : m_pNext(nullptr), m_bRegistered(false), m_pszName(nullptr), m_pszHelpString(nullptr), m_nFlags(0) {}
ConCommandBase::~ConCommandBase() {}
bool ConCommandBase::IsCommand() const { return true; }
bool ConCommandBase::IsFlagSet(int flag) const { return (flag & m_nFlags) != 0; }
void ConCommandBase::AddFlags(int flags) { m_nFlags |= flags; }
const char *ConCommandBase::GetName() const { return m_pszName; }
const char *ConCommandBase::GetHelpText() const { return m_pszHelpString; }
bool ConCommandBase::IsRegistered() const { return m_bRegistered; }
CVarDLLIdentifier_t ConCommandBase::GetDLLIdentifier() const { return g_dll_id; }
void ConCommandBase::CreateBase(const char *name, const char *help, int flags)
{
    m_bRegistered = false;
    m_pszName = name;
    m_pszHelpString = help ? help : "";
    m_nFlags = flags;
    m_pNext = nullptr;
}
void ConCommandBase::Init() {}
ConCommand::ConCommand(const char *name, FnCommandCallback_t cb, const char *help, int flags, FnCommandCompletionCallback)
{
    m_fnCommandCallback = cb;
    m_fnCompletionCallback = nullptr;
    m_bHasCompletionCallback = false;
    m_bUsingNewCommandCallback = true;
    m_bUsingCommandCallbackInterface = false;
    CreateBase(name, help, flags);
}
ConCommand::~ConCommand() {}
bool ConCommand::IsCommand() const { return true; }
int ConCommand::AutoCompleteSuggest(const char *, CUtlVector<CUtlString> &) { return 0; }
bool ConCommand::CanAutoComplete() { return false; }
void ConCommand::Dispatch(const CCommand &command) { m_fnCommandCallback(command); }

// The chat box sends "say <text>" / "say_team <text>" with IVEngineClient::ClientCmd_Unrestricted. Taking
// our commands there works whoever owns the say command (bhoptools patches server.so's say; on a remote
// server the engine forwards that one without running it), so it's the main path; say_cb above covers
// `say !tv` typed in the console.
typedef void (*ClientCmdU_t)(IVEngineClient *, const char *);
static ClientCmdU_t o_ClientCmdU;

static void h_ClientCmdU(IVEngineClient *self, const char *cmd)
{
    if (cmd && (!strncmp(cmd, "say ", 4) || !strncmp(cmd, "say_team ", 9)) && chat_command(cmd + (cmd[3] == ' ' ? 4 : 9))) return;
    o_ClientCmdU(self, cmd);
}

static ConCommand g_cmd_say("say", say_cb, "say (CS:TV commands like !tv stay on this client)");
static ConCommand g_cmd_say_team("say_team", say_cb, "say_team (CS:TV commands like !tv stay on this client)");
static ConCommand g_cmd_cstv("cstv", cstv_cb, "CS:TV: cstv (window), cstv place [personal], cstv hud, cstv tv small, cstv link <url>, cstv pause, cstv next, cstv vol 50, cstv help ...");
// the Progress Log window (cstv_prog.inc): the server's progresslog plugin sends `prog_ui ...`
static void prog_cb(const CCommand &args)
{
    std::vector<const char *> v;
    for (int i = 0; i < args.ArgC(); i++) v.push_back(args[i]);
    gui_prog_cmd((int)v.size(), v.data());
}
static ConCommand g_cmd_prog("prog_ui", prog_cb, "Progress Log window (the server's progresslog plugin drives it; !prog)", FCVAR_SERVER_CAN_EXECUTE);
static ConCommandBase *g_say_orig[2];   // server.so's say / say_team
static ConCommand *const g_say_ours[2] = {&g_cmd_say, &g_cmd_say_team};
static bool g_cmds_done, g_say_is_ours;

static void register_commands()
{
    g_cmds_done = true;
    g_dll_id = g_cvar->AllocateDLLIdentifier();
    g_cvar->RegisterConCommand(&g_cmd_cstv);
    g_cvar->RegisterConCommand(&g_cmd_prog);
    for (int i = 0; i < 2; i++) {
        ConCommandBase *c = g_cvar->FindCommandBase(g_say_ours[i]->GetName());
        if (c && !c->IsCommand()) continue;
        g_say_orig[i] = c;
        int flags = 0;
        for (int b = 0; b < 31 && c; b++)
            if (c->IsFlagSet(1 << b)) flags |= 1 << b;
        g_say_ours[i]->AddFlags(flags & ~FCVAR_GAMEDLL);
    }
}

static void set_say_owner(bool ours)
{
    if (ours == g_say_is_ours) return;
    g_say_is_ours = ours;
    for (int i = 0; i < 2; i++) {
        ConCommandBase *off = ours ? g_say_orig[i] : g_say_ours[i], *on = ours ? g_say_ours[i] : g_say_orig[i];
        if (off) g_cvar->UnregisterConCommand(off);
        if (on) g_cvar->RegisterConCommand(on);
    }
}

static bool hosting()
{
    INetChannelInfo *ch = g_engine->GetNetChannelInfo();
    const char *addr = ch ? ch->GetAddress() : nullptr;
    return addr && !strncmp(addr, "loopback", 8);
}

// ---------------------------------------------------------------- per frame
// Off: everything stops and cstvd is let go (it quits on its own); on again: reconnect and catch up.
static void go_dark()
{
    stop_video();
    if (g_place.on) g_place.on = false;
    if (gui_open()) gui_toggle();
    capture_stop();
    if (g_sock >= 0) {
        plat_close(g_sock);
        g_sock = -1;
    }
    g_from_helper.clear();
    g_chat_active = false;
    g_playing_sent = false;
    g_ident_sent.clear();
    g_where_sent.clear();
    keep_bound(now_s(), true);   // the placeholder again
}

static void main_tick()
{
    double t = now_s();
    if (!g_cmds_done) register_commands();
    static bool was_on = true;
    bool on = g_on.load();
    if (on != was_on) {
        was_on = on;
        if (on) g_last_connect_try = 0;   // connect right away
        else go_dark();
    }
    if (!on) {
        if (t - g_last_where >= 0.5) { g_last_where = t; set_say_owner(!hosting()); }   // !tvhelp still works
        return;
    }
    helper_connect(t);
    helper_poll();
    if (g_sock >= 0 && t - g_last_ping >= 1.0) { g_last_ping = t; helper_send("ping\n"); }
    if (t - g_last_where >= 0.5) {
        g_last_where = t;
        set_say_owner(!hosting());
        update_where();
        if (g_sock >= 0) map_shm();
    }
    if (g_place.on && g_place.cancel) {
        g_place.on = false;
        cstv_print("Placing cancelled.");
    } else if (g_place.on && g_place.confirm) {
        g_place.confirm = false;
        place_confirm();
    }
    if (g_place_wait && t - g_place_wait > 4) {
        g_place_wait = 0;
        cstv_print(g_place_wait_scale < 0.25f ? "The master didn't put the shared screen up (an older master only takes sizes 0.25-4)."
                                              : "The master didn't put the shared screen up.");
    }
}

static const float VIS_MAX_DIST = 3000, VOL_FULL = 300, VOL_ZERO = 1500;
static int g_last_vol = -1;
static double g_last_vol_time;

// can (a point of) the screen be on screen: near, or within 80 degrees of the view direction
static bool maybe_visible(const Vector &eye, const Vector &fwd, const Vector &p)
{
    Vector d = p - eye;
    float dist = d.Length();
    return dist < VIS_MAX_DIST && (dist < 256 || fwd.Dot(d) / dist > cosf(80.f * (float)M_PI / 180));
}

static void render_tick()
{
    if (!g_tex || !g_on.load()) return;
    double t = now_s();
    static bool bound_want;
    bool want = g_sock >= 0 && g_shm && g_shm->latest <= 2;
    if (want != bound_want) { bound_want = want; keep_bound(t, true); }   // switch right away, not on the 1 s check
    else keep_bound(t);
    if (!g_playing_sent && g_have_state && g_sock >= 0) send_load();   // e.g. cstvd started after the link was set

    Vector eye, fwd;
    QAngle ang;
    const Screen *const screens[] = {&g_scr, &g_pscr};
    bool have_view = (g_scr.on || g_pscr.on) && get_view(eye, ang);
    if (have_view) angle_vectors(ang, &fwd, nullptr, nullptr);
    bool hud = gui_hud_on() || gui_preview_on();

    // volume: full with the HUD screen on (or the Lobby page's preview, or universal audio), else by the nearest world screen:
    // full within VOL_FULL, silent beyond VOL_ZERO; scaled by the player's preference
    if (g_playing_sent && t - g_last_vol_time > 0.1) {
        g_last_vol_time = t;
        float dist = hud || g_universal ? 0 : 1e9f;
        for (const Screen *s : screens)
            if (have_view && s->on) dist = std::min(dist, (s->pos - eye).Length());
        float k = dist <= VOL_FULL ? 1.f : dist >= VOL_ZERO ? 0.f : 1.f - (dist - VOL_FULL) / (VOL_ZERO - VOL_FULL);
        int vol = (int)lroundf(k * g_user_vol);
        if (abs(vol - g_last_vol) >= 2 || (vol == 0) != (g_last_vol == 0)) { g_last_vol = vol; helper_send("vol %d\n", vol); }
    }

    // upload only a new frame, only when a screen can be on screen (or the Lobby page's preview is shown)
    bool seen = hud || g_place.on || gui_preview_on(), chat_seen = false;
    for (const Screen *s : screens) {
        if (!have_view || !s->on) continue;
        seen = seen || maybe_visible(eye, fwd, s->pos);
        chat_seen = chat_seen || maybe_visible(eye, fwd, chat_center(*s, s->right)) || maybe_visible(eye, fwd, chat_center(*s, -s->right));
    }
    if (g_shm && g_sock >= 0 && seen) {
        uint64_t seq = __atomic_load_n(&g_shm->seq, __ATOMIC_ACQUIRE);
        if (seq != g_uploaded_seq && g_shm->latest <= 2) {
            g_uploaded_seq = seq;
            upload(g_tex);
        }
    }
    if (g_chat_shown && (((chat_seen || gui_hud_on()) && g_chat_panel) || gui_xchat_on())) {
        uint64_t seq = __atomic_load_n(&g_chat->seq, __ATOMIC_ACQUIRE);
        if (seq != g_chat_uploaded_seq && g_chat->latest <= 2) {
            g_chat_uploaded_seq = seq;
            upload(g_chat_tex);
        }
    }
}

// ---------------------------------------------------------------- hooks
enum {
    SLOT_LevelInitPostEntity = 6, SLOT_LevelShutdown = 7, SLOT_IN_KeyEvent = 20, SLOT_FrameStageNotify = 35,
    SLOT_SceneEnd = 9,   // IVRenderView
};
typedef void (*LevelInitPostEntity_t)(IBaseClientDLL *);
typedef void (*LevelShutdown_t)(IBaseClientDLL *);
typedef int (*IN_KeyEvent_t)(IBaseClientDLL *, int, ButtonCode_t, const char *);
typedef void (*FrameStageNotify_t)(IBaseClientDLL *, ClientFrameStage_t);
typedef void (*SceneEnd_t)(IVRenderView *);
static LevelInitPostEntity_t o_LevelInitPostEntity;
static LevelShutdown_t o_LevelShutdown;
static IN_KeyEvent_t o_IN_KeyEvent;
static FrameStageNotify_t o_FrameStageNotify;
static SceneEnd_t o_SceneEnd;

static void h_LevelInitPostEntity(IBaseClientDLL *self)
{
    o_LevelInitPostEntity(self);
    ensure_texture();
    if (!g_screen_mat) g_screen_mat = find_material("cstv/screen");
    if (!g_chat_mat) g_chat_mat = find_material("cstv/chat");
    if (!g_preview_mat) g_preview_mat = find_material("cstv/preview");
    if (!g_outline_mat) g_outline_mat = find_material("cstv/outline");
    if (!g_chat_see_mat) g_chat_see_mat = find_material("cstv/chat_see");
    keep_bound(now_s(), true);
}

static void h_LevelShutdown(IBaseClientDLL *self)
{
    stop_video();
    g_scr.on = false;
    g_pscr.on = false;
    g_loc.clear();
    g_place.on = false;
    g_place_wait = 0;
    g_where_sent.clear();   // even back on the same server + map: the master then sends the screen and link again
    o_LevelShutdown(self);
}

static int h_IN_KeyEvent(IBaseClientDLL *self, int down, ButtonCode_t key, const char *binding)
{
    int r = o_IN_KeyEvent(self, down, key, binding);
    if (r && down && binding && is_click_binding(binding)) on_click();
    return r;
}

static void h_FrameStageNotify(IBaseClientDLL *self, ClientFrameStage_t stage)
{
    if (stage == FRAME_START || stage == FRAME_RENDER_START) main_tick();
    if (stage == FRAME_START) capture_frame();
    if (stage == FRAME_RENDER_START) {
        render_tick();
    }
    o_FrameStageNotify(self, stage);
}

static void h_SceneEnd(IVRenderView *self)
{
    o_SceneEnd(self);
    draw_screens();
}

// ---------------------------------------------------------------- init
// Swap one vtable slot. The original is stored before the swap, so a call that lands right after
// it already has somewhere to go; if someone else swapped the slot in between, theirs is what we call.
template <typename F>
static bool hook_slot(void *obj, int slot, F &orig, void *fn)
{
    void **vt = *(void ***)obj;
    orig = (F)vt[slot];
    void *was = plat_swap_slot(obj, slot, fn);
    if (!was) return false;
    orig = (F)was;
    return true;
}

static bool interfaces()
{
    g_client = (IBaseClientDLL *)plat_interface("client", CLIENT_DLL_INTERFACE_VERSION);
    g_matsys = (IMaterialSystem *)plat_interface("materialsystem", MATERIAL_SYSTEM_INTERFACE_VERSION);
    g_engine = (IVEngineClient *)plat_interface("engine", VENGINE_CLIENT_INTERFACE_VERSION);
    g_render = (IVRenderView *)plat_interface("engine", VENGINE_RENDERVIEW_INTERFACE_VERSION);
    g_trace = (IEngineTrace *)plat_interface("engine", INTERFACEVERSION_ENGINETRACE_CLIENT);
    g_cvar = (ICvar *)plat_interface(PLAT_VSTDLIB, CVAR_INTERFACE_VERSION);
    return g_client && g_matsys && g_engine && g_render && g_trace && g_cvar;
}

enum { SLOT_EC_ClientCmdUnrestricted = 106 };

// 0 = not yet (the game's modules aren't loaded), 1 = hooked, 2 = failed for good
static std::atomic<int> g_hooked{0};

static bool hook_once()
{
    int expect = 0;
    if (!g_hooked.compare_exchange_strong(expect, 3)) return expect == 1;   // 3: someone is at it right now
    if (!interfaces()) { g_hooked.store(0); return false; }
    g_ConMsg = (ConMsg_t)plat_conmsg();
    // the original pointers are set before each swap, and the hooks only run on the main thread
    if (!hook_slot(g_client, SLOT_LevelInitPostEntity, o_LevelInitPostEntity, (void *)h_LevelInitPostEntity)
        || !hook_slot(g_client, SLOT_LevelShutdown, o_LevelShutdown, (void *)h_LevelShutdown)
        || !hook_slot(g_client, SLOT_IN_KeyEvent, o_IN_KeyEvent, (void *)h_IN_KeyEvent)
        || !hook_slot(g_render, SLOT_SceneEnd, o_SceneEnd, (void *)h_SceneEnd)
        || !hook_slot(g_client, SLOT_FrameStageNotify, o_FrameStageNotify, (void *)h_FrameStageNotify)) {
        g_hooked.store(2);
        say("vtable hook failed; CS:TV stays off");
        return false;
    }
    if (!hook_slot(g_engine, SLOT_EC_ClientCmdUnrestricted, o_ClientCmdU, (void *)h_ClientCmdU)) {
        o_ClientCmdU = nullptr;
        say("can't watch the chat box: CS:TV commands work from the console (`cstv ...`) and say there only");
    }
    g_hooked.store(1);
    say("CS:TV client %d loaded", CSTV_CLIENT_VERSION);
    return true;
}

// the window needs vgui2, vguimatsurface and the input hook, which may still be loading
static void *gui_thread(void *)
{
    int i = 0;
    for (; i < 600 && !gui_install(); i++) plat_sleep_ms(100);
    if (i == 600) say("the CS:TV window could not be set up (!cstv won't open)");
    return nullptr;
}

static void *init_thread(void *)
{
    int i = 0;
    for (; i < 3000 && !hook_once() && g_hooked.load() != 2; i++) plat_sleep_ms(100);   // up to 300 s while the game loads
    if (g_hooked.load() != 1) {
        g_ConMsg = (ConMsg_t)plat_conmsg();
        if (i == 3000) say("game interfaces not found; CS:TV stays off");
        return nullptr;
    }
    gui_thread(nullptr);
    return nullptr;
}

static bool g_started;

bool cstv_start()
{
    if (!plat_in_game() || plat_disabled()) return false;   // only inside the game itself
    // Both the standalone libcstv.so and a bhoptools with CS:TV built in may be preloaded: the first one
    // to start runs CS:TV, the other stays out (two copies would fight over the hooks and cstvd).
    if (!plat_claim()) return false;
    if (!plat_thread(init_thread)) return false;
    g_started = true;
    return true;
}

bool cstv_start_now()
{
    if (!plat_in_game() || plat_disabled()) return false;
    if (!plat_claim()) {   // the standalone libcstv.so is preloaded too and started first
        g_ConMsg = (ConMsg_t)plat_conmsg();
        say("another CS:TV client is already loaded (libcstv.so in LD_PRELOAD as well?); this one stays out");
        return false;
    }
    g_started = true;
    if (hook_once()) return plat_thread(gui_thread) || true;
    if (g_hooked.load() == 2 || !plat_thread(init_thread)) { g_started = false; return false; }
    return true;   // the game's interfaces aren't there yet: a thread keeps trying
}

bool cstv_toggle()
{
    bool on = !g_on.load();
    g_on.store(on);
    return on;
}

bool cstv_running() { return g_started; }
void cstv_set_enabled(bool on) { g_on.store(on); }
bool cstv_enabled() { return g_on.load(); }

#ifndef _WIN32
#ifndef CSTV_EMBEDDED
__attribute__((constructor)) static void cstv_ctor() { cstv_start(); }
#endif

// ---------------------------------------------------------------- operating system (cstv_platform.h), Linux
double plat_now()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// full path of a loaded module whose path ends with `suffix`, from /proc/self/maps
static std::string module_path(const char *suffix)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return "";
    char line[1024];
    std::string found;
    size_t sl = strlen(suffix);
    while (fgets(line, sizeof line, f)) {
        char *p = strchr(line, '/');
        if (!p) continue;
        p[strcspn(p, "\n")] = 0;
        size_t n = strlen(p);
        if (n >= sl && !strcmp(p + n - sl, suffix)) { found = p; break; }
    }
    fclose(f);
    return found;
}

typedef void *(*CreateInterfaceFn_t)(const char *name, int *ret);

void *plat_interface(const char *module, const char *iface)
{
    std::string path = module_path((std::string("/") + module + ".so").c_str());
    if (path.empty()) return nullptr;
    void *h = dlopen(path.c_str(), RTLD_NOLOAD | RTLD_LAZY);   // the game loads these privately
    if (!h) return nullptr;
    auto ci = (CreateInterfaceFn_t)dlsym(h, "CreateInterface");
    void *r = ci ? ci(iface, nullptr) : nullptr;
    dlclose(h);
    return r;
}

// A vtable can share its page with writable data, so restore what the page had.
static int page_prot(uintptr_t addr)
{
    int prot = PROT_READ;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return prot;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        uintptr_t s, e;
        char perms[5] = {0};
        if (sscanf(line, "%lx-%lx %4s", &s, &e, perms) < 3 || addr < s || addr >= e) continue;
        prot = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) | (perms[2] == 'x' ? PROT_EXEC : 0);
        break;
    }
    fclose(f);
    return prot;
}

void *plat_swap_slot(void *obj, int slot, void *fn)
{
    void **vt = *(void ***)obj;
    uintptr_t a = (uintptr_t)&vt[slot];
    long pg = sysconf(_SC_PAGESIZE);
    uintptr_t start = a & ~(uintptr_t)(pg - 1);
    int prot = page_prot(a);
    if (mprotect((void *)start, pg, prot | PROT_WRITE)) return nullptr;
    void *orig = __atomic_exchange_n(&vt[slot], fn, __ATOMIC_SEQ_CST);
    mprotect((void *)start, pg, prot);
    return orig;
}

intptr_t plat_connect()
{
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un a = {};
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", CSTV_SOCK_PATH);
    if (connect(s, (sockaddr *)&a, sizeof a)) { close(s); return -1; }
    return s;
}

int plat_send(intptr_t s, const char *buf, int n) { return (int)send((int)s, buf, n, MSG_NOSIGNAL | MSG_DONTWAIT); }
int plat_recv(intptr_t s, char *buf, int n) { return (int)recv((int)s, buf, n, MSG_DONTWAIT); }
void plat_close(intptr_t s) { close((int)s); }

void *plat_map(const char *name, size_t size)
{
    int fd = open(name, O_RDWR);
    if (fd < 0) return nullptr;
    struct stat st;   // shorter (cstvd between creating and sizing it): reading past the end would be SIGBUS
    if (fstat(fd, &st) || st.st_size < (off_t)size) { close(fd); return nullptr; }
    void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return p == MAP_FAILED ? nullptr : p;
}

void plat_unmap(void *p, size_t size) { munmap(p, size); }

void *plat_conmsg()
{
    std::string tier0 = module_path("/libtier0.so");
    void *h = tier0.empty() ? nullptr : dlopen(tier0.c_str(), RTLD_NOLOAD | RTLD_LAZY);
    if (!h) return nullptr;
    void *f = dlsym(h, "_Z6ConMsgPKcz");
    dlclose(h);
    return f;
}

bool plat_in_game()
{
    char comm[64] = {0};
    if (FILE *f = fopen("/proc/self/comm", "r")) { if (!fgets(comm, sizeof comm, f)) comm[0] = 0; fclose(f); }
    return strstr(comm, "cstrike_linux64") != nullptr;
}

bool plat_disabled()
{
    const char *env = getenv("CSTV_DISABLE");
    return env && !strcmp(env, "1");
}

void plat_spawn_helper() {}   // systemd's cstvd.socket starts cstvd on connect (or cstv-launch did)

bool plat_thread(void *(*fn)(void *))
{
    // detached from the start (pthread_detach is a glibc 2.34 symbol; the Steam runtime may be older)
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    bool ok = pthread_create(&th, &at, fn, nullptr) == 0;
    pthread_attr_destroy(&at);
    return ok;
}

void plat_sleep_ms(int ms) { usleep(ms * 1000); }

bool plat_claim()
{
    if (getenv("CSTV_CLIENT_LOADED")) return false;
    setenv("CSTV_CLIENT_LOADED", "1", 1);
    return true;
}
#endif   // !_WIN32
