// The in-game CS:TV window, the HUD screen and the place-mode hint bar. Drawn with the VGUI surface
// after "MatSystemTopPanel" paints, so they work the same on togl and DXVK and sit above the HUD (the
// console and game menu popups still paint over them). While the window is open an SDL event filter
// eats mouse and keyboard input, so the game doesn't act on it; in place mode it eats only the keys
// and buttons place mode uses. The game keeps the mouse grabbed (launcher.so's relative mode and
// warping), so the window moves a cursor of its own by the motion events' deltas and draws it.
//
// The HUD screen is the cstv/screen material (the core swaps its $basetexture between the video and
// the placeholder) drawn as a VGUI texture: CreateNewTextureID + DrawSetTextureFile look the material up
// by name (FindMaterial returns the core's own IMaterial) and DrawTexturedRect draws it with the
// surface's 2D setup, which is the same on both renderers.
//
// Slots (CS:S 64-bit Linux, checked 2026-10-18 by disassembling the vtables of the real binaries,
// see the report in the session notes; the SDK 2013 headers give the same numbers for these):
//   IPanel (VPanelWrapper, vgui2.so)        37 GetName, 42 PaintTraverse (hooked)
//   ISurface (CMatSystemSurface)            10 DrawSetColor, 12 DrawFilledRect, 14 DrawOutlinedRect,
//                                           17 DrawSetTextFont, 18 DrawSetTextColor, 20 DrawSetTextPos,
//                                           22 DrawPrintText, 30 DrawSetTextureFile, 32 DrawSetTexture,
//                                           34 DrawTexturedRect, 37 CreateNewTextureID, 38 GetScreenSize,
//                                           66 CreateFont, 67 SetFontGlyphSet, 69 GetFontTall,
//                                           75 GetTextSize
//   IVEngineClient (engine.so)              22 Key_LookupBinding, 26 IsInGame, 106 ClientCmd_Unrestricted
// The binary vtables are longer than the headers (CMatSystemSurface 182 vs 178, VPanelWrapper 64 vs 62),
// but the extra entries come after every slot used here. The texture slots (added 2026-10-18; numbers
// from the SDK header by PMF arithmetic as in tools/vtidx.cpp, vtable 0x29f750 in vguimatsurface.so via
// tools/vtdump.py + slots.py, code read with objdump): 30 calls the texture dictionary's
// BindTextureToFile (dictionary slot 5) and then this->slot 32; 32 compares the id with the bound one at
// +0x39c, flushes and stores it; 34 returns unless drawing (+0x2b), gets the bound texture's coordinates
// from the dictionary (slot 10), adds the panel offset and draws; 37 forwards to the dictionary's
// CreateTexture (slot 0) with the bool.
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#ifdef _WIN32
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <dlfcn.h>
#include <strings.h>
#endif

#include "vgui/ISurface.h"
#include "vgui/IPanel.h"
#include "cdll_int.h"

#include "cstv_capture.h"
#include "cstv_gui.h"
#include "cstv_platform.h"
#include "cstv_proto.h"

using vgui::VPANEL;
using vgui::HFont;

static vgui::ISurface *g_surf;
static vgui::IPanel *g_panel;
static IVEngineClient *g_engine;
static bool g_installed;
static bool g_open;

// ---------------------------------------------------------------- SDL (only the parts we touch)
enum {
    EV_KEYDOWN = 0x300, EV_KEYUP, EV_TEXTEDITING, EV_TEXTINPUT, EV_TEXTEDITING_EXT = 0x305,
    EV_MOUSEMOTION = 0x400, EV_MOUSEDOWN, EV_MOUSEUP, EV_MOUSEWHEEL,
};
enum { SC_A = 4, SC_C = 6, SC_E = 8, SC_F = 9, SC_V = 25, SC_RETURN = 40, SC_ESCAPE = 41, SC_BACKSPACE = 42, SC_TAB = 43, SC_KP_ENTER = 88,
       SC_PAGEUP = 75, SC_DELETE = 76, SC_PAGEDOWN = 78, SC_RIGHT = 79, SC_LEFT = 80, SC_DOWN = 81, SC_UP = 82 };
enum { KMOD_CTRL = 0x00C0 };

struct SdlKey { uint32_t type, timestamp, window; uint8_t state, repeat, pad2, pad3; int32_t scancode, sym; uint16_t mod; uint32_t unused; };
struct SdlText { uint32_t type, timestamp, window; char text[32]; };
struct SdlButton { uint32_t type, timestamp, window, which; uint8_t button, state, clicks, pad; int32_t x, y; };
struct SdlMotion { uint32_t type, timestamp, window, which, state; int32_t x, y, xrel, yrel; };
struct SdlWheel { uint32_t type, timestamp, window, which; int32_t x, y; uint32_t direction; };
union SdlEvent { uint32_t type; SdlKey key; SdlText text; SdlButton button; SdlMotion motion; SdlWheel wheel; uint8_t pad[56]; };

typedef int (*SdlFilter)(void *, SdlEvent *);
static int (*SDL_GetEventFilter_)(SdlFilter *, void **);
static void (*SDL_SetEventFilter_)(SdlFilter, void *);
static char *(*SDL_GetClipboardText_)();
static int (*SDL_SetClipboardText_)(const char *);
static void (*SDL_free_)(void *);
static void (*SDL_StartTextInput_)();
static int (*SDL_GetScancodeFromName_)(const char *);

static SdlFilter g_prev_filter;
static void *g_prev_data;

static bool load_sdl()
{
#ifdef _WIN32
    // no SDL for the keyboard and mouse on Windows: plat_input_install; these do the same jobs
    SDL_GetClipboardText_ = plat_clipboard_get;
    SDL_SetClipboardText_ = plat_clipboard_set;
    SDL_free_ = free;
    SDL_GetScancodeFromName_ = plat_scancode_from_name;
    return true;
#else
    // The Steam runtime ships it as libSDL2-2.0.so.0; the game loads it privately, so find it by path.
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[1024];
    std::string path;
    while (fgets(line, sizeof line, f)) {
        char *p = strchr(line, '/');
        if (!p || !strstr(p, "/libSDL2")) continue;
        p[strcspn(p, "\n")] = 0;
        path = p;
        break;
    }
    fclose(f);
    if (path.empty()) return false;
    void *h = dlopen(path.c_str(), RTLD_NOLOAD | RTLD_LAZY);
    if (!h) return false;
    *(void **)&SDL_GetEventFilter_ = dlsym(h, "SDL_GetEventFilter");
    *(void **)&SDL_SetEventFilter_ = dlsym(h, "SDL_SetEventFilter");
    *(void **)&SDL_GetClipboardText_ = dlsym(h, "SDL_GetClipboardText");
    *(void **)&SDL_SetClipboardText_ = dlsym(h, "SDL_SetClipboardText");
    *(void **)&SDL_free_ = dlsym(h, "SDL_free");
    *(void **)&SDL_StartTextInput_ = dlsym(h, "SDL_StartTextInput");
    *(void **)&SDL_GetScancodeFromName_ = dlsym(h, "SDL_GetScancodeFromName");
    dlclose(h);   // RTLD_NOLOAD only took a reference; the game keeps it loaded
    return SDL_GetEventFilter_ && SDL_SetEventFilter_;
#endif
}

// ---------------------------------------------------------------- text helpers
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static std::string b64enc(const std::string &in)
{
    std::string out;
    unsigned acc = 0;
    int bits = 0;
    for (unsigned char c : in) {
        acc = (acc << 8) | c;
        bits += 8;
        while (bits >= 6) { bits -= 6; out += B64[(acc >> bits) & 63]; }
    }
    if (bits) out += B64[(acc << (6 - bits)) & 63];
    return out;
}

// "-" is the protocol's empty value. False on anything that isn't base64url.
static bool b64dec(const std::string &in, std::string &out)
{
    out.clear();
    if (in == "-") return true;
    unsigned acc = 0;
    int bits = 0;
    for (char c : in) {
        const char *p = strchr(B64, c);
        if (!c || !p) return false;
        acc = (acc << 6) | (unsigned)(p - B64);
        bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((acc >> bits) & 0xff); }
    }
    return true;
}

static std::string dec(const std::string &in)
{
    std::string s;
    return b64dec(in, s) ? s : std::string();
}

static void split(const std::string &s, char sep, std::vector<std::string> &out)
{
    out.clear();
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(sep, i);
        if (j == std::string::npos) j = s.size();
        if (j > i || sep == ',') out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
}

// UTF-8 to wchar_t for DrawPrintText; control characters become spaces, bad bytes U+FFFD.
static std::wstring widen(const std::string &s)
{
    std::wstring w;
    w.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        unsigned cp;
        int n;
        if (c < 0x80) { cp = c; n = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 3; }
        else { w += 0xFFFD; i++; continue; }
        if (i + n >= s.size() + (n ? 0 : 1)) { w += 0xFFFD; break; }
        bool ok = true;
        for (int k = 1; k <= n; k++) {
            unsigned char d = s[i + k];
            if ((d & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (d & 0x3F);
        }
        if (!ok) { w += 0xFFFD; i++; continue; }
        i += n + 1;
        if (cp < 0x20 || cp == 0x7F) cp = ' ';
        if (sizeof(wchar_t) == 2 && cp > 0xFFFF) cp = 0xFFFD;
        w += (wchar_t)cp;
    }
    return w;
}

static std::string fmt_time(long s)
{
    char b[32];
    if (s < 0) s = 0;
    if (s >= 3600) snprintf(b, sizeof b, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
    else snprintf(b, sizeof b, "%ld:%02ld", s / 60, s % 60);
    return b;
}

static void send(const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0 && n < (int)sizeof buf) cstv_helper_line(buf);
}

// ---------------------------------------------------------------- state from cstvd
struct QItem { std::string uid, url, title, by; };
struct HItem { std::string url, title, by; long at; };
struct Member { std::string id, name, where; };
struct Result { std::string url, title, channel; long dur; };

static struct {
    bool master;
    std::string my_id;
    std::string url, title;
    bool paused, running, have_state;
    long pos_ms;
    int speed = 100;
    double state_at;
    bool loop;   // the master doesn't report it, so this is what we last asked for
    bool screen, screen_float;
    float screen_scale;
    std::vector<QItem> queue;
    std::string room_kind, room_name;
    std::string room_label, room_pw;   // lobby name; its password (the owner only gets it)
    bool room_locked;                  // the lobby has a password
    bool owner, perm_owner;
    std::string roster_owner;
    std::vector<Member> roster;
    std::string notice;
    int search_id;
    bool searching;
    std::string search_status;
    std::vector<Result> results;
    std::string join_code;   // the lobby a joinfail was about
    bool join_need_pw;
    int vol = 100;
    std::string quality = "auto";
    bool subs, chat, upnext = true;
    bool hud;
    bool universal;   // universal audio: the video's sound at full volume wherever you are
    float hud_x = 0.015f, hud_y = 0.32f, hud_w = 0.28f;   // top left and width, fractions of the screen
    int opacity_screen = 100, opacity_hud = 100;           // percent: placed screens, the HUD screen
    std::string stream = "idle", stream_text;
    std::string you_sid, you_name;   // signed in to cstv.xyz as (empty = not)
    std::string warning;             // a cstv.xyz admin's warning, shown until acknowledged
    std::vector<HItem> history;      // what played in this room, newest first (the Lobby page's History)
    struct Lobby { std::string code, name, playing; bool locked; int people; };
    std::vector<Lobby> lobbies;      // the Lobby tab's list: one page of every lobby on cstv.xyz
    int lobby_page = 1, lobby_pages = 1, lobby_total;
    double lobbies_at = -100;        // when that page was asked for
    std::string l_sites = "any", l_emotes;   // this lobby's settings (lsettings): allowed sites, emotes channel,
    int l_slow, l_perms;                     // slow mode seconds, what everyone may do (PERM_* bits),
    bool l_gifs = true;                      // GIFs in chat
    std::string chat_text = "normal";        // the Lobby page chat's text size (pref chat_text)
    bool chat_avail;                         // cstvd: the current video has a live chat (Twitch / YouTube live)
    int chat_state;                          // cstvd: 0 no live chat drawn, 1 beside the screen, 2 for the Live tab
} st;

static void open_window();
static void chat_insert(const std::string &word);
static void set_chat_mode(int m);
static bool gui_lchat_on();

static bool is_live() { return st.url.compare(0, 7, "rtmp://") == 0; }

static void on_srv(const std::vector<std::string> &w)
{
    const std::string &v = w[1];
    std::vector<std::string> f;
    if (v == "url" && w.size() >= 4) {
        std::string u;
        if (!b64dec(w[3], u)) return;
        st.url = u;
        st.title.clear();
        st.have_state = false;
    } else if (v == "state" && w.size() >= 8) {
        st.paused = w[3] == "1";
        st.pos_ms = atol(w[4].c_str());
        st.speed = atoi(w[5].c_str());
        if (st.speed <= 0) st.speed = 100;
        st.running = w[7] == "1";
        st.state_at = plat_now();
        st.have_state = true;
    } else if (v == "stop") {
        st.url.clear();
        st.title.clear();
        st.have_state = false;
    } else if (v == "nowtitle" && w.size() >= 3) {
        st.title = dec(w[2]);
    } else if (v == "queue") {
        st.queue.clear();
        for (size_t i = 2; i < w.size(); i++) {
            split(w[i], ',', f);
            if (f.size() < 4) continue;
            st.queue.push_back({f[0], dec(f[1]), dec(f[2]), dec(f[3])});
        }
    } else if (v == "room" && w.size() >= 6) {
        st.room_kind = w[2];
        st.room_name = w[3];
        st.owner = w[4] == "1";
        st.perm_owner = w[5] == "owner";
        st.room_label = w.size() >= 7 ? dec(w[6]) : "";
        st.room_locked = w.size() >= 8 && w[7] == "1";
        st.room_pw = w.size() >= 9 ? dec(w[8]) : "";
        if (st.room_kind == "lobby") {
            st.join_code.clear();   // in: no password prompt any more
            st.join_need_pw = false;
        }
    } else if (v == "joinfail" && w.size() >= 4) {
        const std::string &why = w[3];
        st.join_code = w[2];
        st.join_need_pw = why == "password" || why == "wrong";
        st.notice = why == "password" ? "Lobby " + w[2] + " has a password."
                  : why == "wrong" ? "Wrong password."
                  : why == "locked" ? "Too many wrong passwords for that lobby. Try again in a few minutes."
                  : why == "banned" ? "You're banned from that lobby."
                  : "There's no lobby with that code.";
    } else if (v == "roster" && w.size() >= 3) {
        st.roster_owner = w[2];
        st.roster.clear();
        for (size_t i = 3; i < w.size(); i++) {
            split(w[i], ',', f);
            if (f.size() < 4) continue;
            st.roster.push_back({f[0], dec(f[2]), dec(f[3])});
        }
    } else if (v == "screen" && w.size() >= 3) {
        st.screen = w[2] != "none" && w.size() >= 4;
        if (st.screen) {
            st.screen_scale = (float)atof(w[2].c_str());
            st.screen_float = w[3] == "1";
        }
    } else if (v == "welcome" && w.size() >= 4) {
        st.my_id = w[3];
    } else if (v == "you" && w.size() >= 4) {
        st.you_sid = w[2] == "-" ? "" : w[2];
        st.you_name = st.you_sid.empty() ? "" : dec(w[3]);
    } else if (v == "history") {   // history url,title,by,when ... (newest first)
        st.history.clear();
        for (size_t i = 2; i < w.size(); i++) {
            split(w[i], ',', f);
            std::string u;
            if (f.size() < 4 || !b64dec(f[0], u) || u.empty()) continue;
            st.history.push_back({u, dec(f[1]), dec(f[2]), atol(f[3].c_str())});
        }
    } else if (v == "lobbies" && w.size() >= 5) {   // lobbies <page> <pages> <total> code,locked,people,name,playing...
        st.lobby_page = atoi(w[2].c_str());
        st.lobby_pages = std::max(1, atoi(w[3].c_str()));
        st.lobby_total = atoi(w[4].c_str());
        st.lobbies.clear();
        for (size_t i = 5; i < w.size(); i++) {
            split(w[i], ',', f);
            if (f.size() < 5) continue;
            st.lobbies.push_back({f[0], dec(f[3]), dec(f[4]), f[1] == "1", atoi(f[2].c_str())});
        }
    } else if (v == "lsettings" && w.size() >= 7) {   // lsettings <sites> <slow> <gifs> <emotes|-> <perms>
        st.l_sites = w[2];
        st.l_slow = atoi(w[3].c_str());
        st.l_gifs = w[4] == "1";
        st.l_emotes = w[5] == "-" ? "" : w[5];
        st.l_perms = atoi(w[6].c_str());
    } else if (v == "warn" && w.size() >= 3) {
        st.warning = dec(w[2]);
        if (!st.warning.empty()) open_window();
    } else if (v == "kicked") {
        st.notice = "You were removed from the lobby";
        std::string why = w.size() >= 3 ? dec(w[2]) : "";
        if (!why.empty()) st.notice += ": " + why;
    }
}

void gui_on_line(const char *line)
{
    if (!line) return;
    static std::vector<std::string> w;
    split(line, ' ', w);
    if (w.empty()) return;
    const std::string &v = w[0];
    if (v == "srv" && w.size() >= 2) {
        on_srv(w);
    } else if (v == "master" && w.size() >= 2) {
        st.master = w[1] == "1";
        if (!st.master) st.you_sid.clear(), st.you_name.clear();
    } else if (v == "pref" && w.size() >= 3) {
        if (w[1] == "vol") st.vol = atoi(w[2].c_str());
        else if (w[1] == "quality") st.quality = w[2];
        else if (w[1] == "subs") st.subs = w[2] == "1";
        else if (w[1] == "chat") st.chat = w[2] == "1";
        else if (w[1] == "upnext") st.upnext = w[2] == "1";
        else if (w[1] == "hud") st.hud = w[2] == "1";
        else if (w[1] == "hud_x") st.hud_x = (float)atof(w[2].c_str());
        else if (w[1] == "hud_y") st.hud_y = (float)atof(w[2].c_str());
        else if (w[1] == "hud_w") st.hud_w = (float)atof(w[2].c_str());
        else if (w[1] == "opacity_screen") st.opacity_screen = std::min(std::max(atoi(w[2].c_str()), 10), 100);
        else if (w[1] == "opacity_hud") st.opacity_hud = std::min(std::max(atoi(w[2].c_str()), 10), 100);
        else if (w[1] == "chat_text") st.chat_text = w[2];
        else if (w[1] == "universal") st.universal = w[2] == "1";
    } else if (v == "chatavail" && w.size() >= 2) {
        st.chat_avail = w[1] == "1";
    } else if (v == "chat" && w.size() >= 2) {
        st.chat_state = atoi(w[1].c_str());
    } else if (v == "result" && w.size() >= 7) {
        if (atoi(w[1].c_str()) != st.search_id || st.results.size() >= 50) return;
        Result r;
        if (!b64dec(w[3], r.url) || r.url.empty()) return;
        r.title = dec(w[4]);
        r.channel = dec(w[5]);
        r.dur = w[6] == "-" ? -1 : atol(w[6].c_str());
        st.results.push_back(r);
    } else if (v == "results_end" && w.size() >= 3) {
        if (atoi(w[1].c_str()) != st.search_id) return;
        st.searching = false;
        if (w[2].find_first_not_of("0123456789") == std::string::npos) {
            st.search_status = st.results.empty() ? "No results" : std::to_string(st.results.size()) + " results";
        } else {
            std::string err = dec(w[2]);
            st.search_status = "Search failed" + (err.empty() ? std::string() : ": " + err);
        }
    } else if (v == "lchat_pick" && w.size() >= 2) {   // an emote picked in the Lobby page's picker
        std::string name = dec(w[1]);
        if (!name.empty()) chat_insert(name);
    } else if (v == "lchat_gifpick" && w.size() >= 2) {   // a GIF picked: post it, back to the chat
        bool ok = !w[1].empty();
        for (char c : w[1]) ok = ok && isalnum((unsigned char)c);
        if (ok) send("m chatgif %s", w[1].c_str());
        set_chat_mode(0);
    } else if (v == "stream_state" && w.size() >= 2) {
        st.stream = w[1];
        st.stream_text = w.size() >= 3 ? dec(w[2]) : "";
    }
}

// ---------------------------------------------------------------- input (filled by the SDL filter)
static struct {
    bool down, click;
    int wheel;
    std::string typed;
    int backspaces, tabs;
    bool enter, paste;
    int nav_x, nav_y, flip;   // the Progress Log's arrow keys and PgUp / PgDn
    bool del;                 // its Delete
} in;

enum { MODE_CSTV, MODE_PROG };
static int g_mode;            // what the window shows: CS:TV, or the Progress Log (cstv_prog.inc)
static void prog_on_close();
static bool prog_esc();       // the Progress Log takes Esc (closes its Yes / No question) instead of closing
static bool g_hud_edit;       // the window shows only the HUD screen, to be dragged and resized
static int g_toggle_scan;     // SDL scancode of the key bound to `cstv`, or 0
static int g_toggle_button;   // SDL mouse button bound to it, or 0
static int g_book_scan, g_book_button;   // the same for `cstv_book`
static int g_focus, g_drag;
static int g_mx, g_my;
static float g_vx = -1, g_vy;   // our cursor; -1 = put it in the middle on the next draw

static void save_hud()
{
    send("pref hud_x %.4f", st.hud_x);
    send("pref hud_y %.4f", st.hud_y);
    send("pref hud_w %.4f", st.hud_w);
}

static void end_hud_edit()
{
    if (!g_hud_edit) return;
    g_hud_edit = false;
    g_drag = 0;
    save_hud();
}

static void close_window()
{
    if (!g_open) return;
    if (g_mode == MODE_PROG) prog_on_close();
    end_hud_edit();
    g_open = false;
    g_focus = g_drag = 0;
    in = {};
#ifdef _WIN32
    plat_input_window(false);
    if (g_engine) g_engine->ClientCmd_Unrestricted("cl_mouseenable 1");
#endif
}

// Runs inside SDL_PumpEvents on the main thread, before the game sees the event. Key and button
// releases always go through so a key held when the window opened doesn't stay pressed in the game.
static int handle_event(SdlEvent *e)
{
    switch (e->type) {
    case EV_KEYDOWN: {
        int sc = e->key.scancode;
        if (sc == SC_ESCAPE && g_mode == MODE_PROG && !e->key.repeat && prog_esc()) return 0;
        if (sc == SC_ESCAPE || (!e->key.repeat && ((g_toggle_scan && sc == g_toggle_scan) || (g_book_scan && sc == g_book_scan)))) {
            if (e->key.repeat || !st.warning.empty()) return 0;
            if (g_hud_edit && sc == SC_ESCAPE) end_hud_edit();   // back to the window
            else close_window();
            return 0;
        }
        bool ctrl = e->key.mod & KMOD_CTRL;
        if (sc == SC_BACKSPACE) in.backspaces++;
        else if (sc == SC_TAB && g_focus) in.tabs++;
        else if (sc == SC_RETURN || sc == SC_KP_ENTER) in.enter = true;
        else if (ctrl && sc == SC_V) in.paste = true;
        else if (g_mode == MODE_PROG) {
            if (sc == SC_RIGHT) in.nav_x++;
            else if (sc == SC_LEFT) in.nav_x--;
            else if (sc == SC_DOWN) in.nav_y++;
            else if (sc == SC_UP) in.nav_y--;
            else if (sc == SC_PAGEDOWN) in.flip++;
            else if (sc == SC_PAGEUP) in.flip--;
            else if (sc == SC_DELETE) in.del = true;
        }
        return 0;
    }
    case EV_TEXTINPUT:
        if (g_focus && in.typed.size() < 256) in.typed += std::string(e->text.text, strnlen(e->text.text, sizeof e->text.text));
        return 0;
    case EV_TEXTEDITING:
    case EV_TEXTEDITING_EXT:
        return 0;
    case EV_MOUSEMOTION:
        g_vx += e->motion.xrel;
        g_vy += e->motion.yrel;
        return 0;
    case EV_MOUSEDOWN:
        if ((g_toggle_button && e->button.button == g_toggle_button) || (g_book_button && e->button.button == g_book_button)) { if (st.warning.empty()) close_window(); return 0; }
        if (e->button.button == 1) { in.down = true; in.click = true; }
        return 0;
    case EV_MOUSEUP:
        if (e->button.button == 1) in.down = false;
        return 1;
    case EV_MOUSEWHEEL:
        in.wheel += e->wheel.direction == 1 ? -e->wheel.y : e->wheel.y;
        return 0;
    case EV_KEYUP:
    default:
        return 1;
    }
}

// Place mode: only its own keys and buttons; aiming and moving still reach the game. Releases go
// through (a release without its press does nothing in the game).
static int place_event(SdlEvent *e)
{
    int ev, n = 0;
    switch (e->type) {
    case EV_KEYDOWN:
        switch (e->key.scancode) {
        case SC_ESCAPE: ev = PLACE_CANCEL; break;
        case SC_E: case SC_RETURN: case SC_KP_ENTER: ev = PLACE_CONFIRM; break;
        case SC_F: ev = PLACE_FLOAT; break;
        case SC_TAB: ev = PLACE_KIND; break;
        default: return 1;
        }
        if (e->key.repeat) return 0;
        break;
    case EV_MOUSEDOWN:
        if (e->button.button != 1 && e->button.button != 3) return 1;
        ev = PLACE_DIST;
        n = e->button.button == 1 ? 1 : -1;
        break;
    case EV_MOUSEWHEEL:
        ev = PLACE_SIZE;
        n = e->wheel.direction == 1 ? -e->wheel.y : e->wheel.y;
        break;
    default:
        return 1;
    }
    cstv_place_event(ev, n);
    return 0;
}

static int gui_filter(void *, SdlEvent *e)
{
    // the console sits on top of the window: while it's up it gets the keys, so it can be closed
    if (!(g_engine && g_engine->Con_IsVisible())) {
        if (g_open && !handle_event(e)) return 0;
        if (!g_open && cstv_place_hint(nullptr) && !place_event(e)) return 0;
    }
    return g_prev_filter ? g_prev_filter(g_prev_data, e) : 1;
}

#ifdef _WIN32
// The game window's messages (cstv_platform_win.cpp) as the SDL events the code above works with.
static int win_event(const plat_event *p)
{
    SdlEvent e = {};
    switch (p->type) {
    case PLAT_KEYDOWN:
        e.type = EV_KEYDOWN;
        e.key.scancode = p->scancode;
        e.key.repeat = (uint8_t)p->repeat;
        e.key.mod = p->ctrl ? KMOD_CTRL : 0;
        break;
    case PLAT_TEXT:
        e.type = p->text[0] ? EV_TEXTINPUT : EV_TEXTEDITING;   // control characters: kept from the game, typed nowhere
        memcpy(e.text.text, p->text, sizeof p->text);
        break;
    case PLAT_MOTION:
        e.type = EV_MOUSEMOTION;
        e.motion.xrel = p->dx;
        e.motion.yrel = p->dy;
        break;
    case PLAT_BUTTONDOWN:
    case PLAT_BUTTONUP:
        e.type = p->type == PLAT_BUTTONDOWN ? EV_MOUSEDOWN : EV_MOUSEUP;
        e.button.button = (uint8_t)p->button;
        break;
    case PLAT_WHEEL:
        e.type = EV_MOUSEWHEEL;
        e.wheel.y = p->wheel;
        break;
    }
    return gui_filter(nullptr, &e);
}
#endif

// Source key names (inputsystem's ButtonCodeToString) that SDL spells differently.
static void find_key(const char *cmd, int &scan, int &button)
{
    scan = button = 0;
    const char *name = g_engine ? g_engine->Key_LookupBinding(cmd) : nullptr;
    if (!name || !*name) return;
    if (!strcasecmp(name, "MOUSE3")) { button = 2; return; }
    if (!strcasecmp(name, "MOUSE4")) { button = 4; return; }
    if (!strcasecmp(name, "MOUSE5")) { button = 5; return; }
    static const char *const map[][2] = {
        {"INS", "Insert"}, {"DEL", "Delete"}, {"PGUP", "PageUp"}, {"PGDN", "PageDown"},
        {"UPARROW", "Up"}, {"DOWNARROW", "Down"}, {"LEFTARROW", "Left"}, {"RIGHTARROW", "Right"},
        {"ENTER", "Return"}, {"SEMICOLON", ";"}, {"CAPSLOCK", "CapsLock"},
    };
    for (auto &m : map)
        if (!strcasecmp(name, m[0])) name = m[1];
    if (SDL_GetScancodeFromName_) scan = SDL_GetScancodeFromName_(name);
}

static void find_toggle_key()
{
    find_key("cstv", g_toggle_scan, g_toggle_button);
    find_key("cstv_book", g_book_scan, g_book_button);
}

// ---------------------------------------------------------------- drawing primitives
struct Col { int r, g, b, a; };

// cstv.xyz's colours (app.css): near-black greys, the blue as solid blocks with black text on them
static const Col C_DIM = {0, 0, 0, 150}, C_BG = {10, 10, 10, 252}, C_BAR = {10, 10, 10, 255},
                 C_SIDE = {16, 16, 16, 255}, C_TAB_HOVER = {29, 29, 29, 255}, C_PANEL2 = {23, 23, 23, 255},
                 C_ACCENT = {74, 143, 240, 255}, C_TEXT = {228, 228, 228, 255}, C_SUB = {138, 138, 138, 255},
                 C_BTN = {16, 16, 16, 255}, C_BTN_HOVER = {29, 29, 29, 255}, C_BORDER = {54, 54, 54, 255},
                 C_BORDER_HOVER = {74, 74, 74, 255}, C_TEXT_OFF = {90, 90, 90, 255}, C_DANGER = {255, 107, 107, 255},
                 C_FIELD = {13, 13, 13, 255}, C_LINE = {36, 36, 36, 255}, C_GOOD = {79, 209, 139, 255},
                 C_BAD = {255, 107, 107, 255}, C_ROW = {16, 16, 16, 255}, C_WHITE = {255, 255, 255, 255}, C_BLACK = {0, 0, 0, 255};

// a disabled control: its colours 65 % of the way to the background (the website's opacity .35)
static Col fade(Col c) { return {c.r + (10 - c.r) * 65 / 100, c.g + (10 - c.g) * 65 / 100, c.b + (10 - c.b) * 65 / 100, c.a}; }

// The window is laid out for 1080p and drawn this much bigger (fonts are rasterized at the final size)
static const float MENU_SCALE = 1.15f;

enum { F_SMALL, F_BODY, F_BOLD, F_TITLE, F_BIG, F_LABEL, F_MC, F_MCB, F_MC2, F_MCB2, F_COUNT };   // F_MC*: the Progress Log's
static HFont g_font[F_COUNT];
static int g_font_tall[F_COUNT];
static int g_font_for_h;   // screen height the fonts were rasterized for
static float g_s = 1;      // UI scale, 1 = 1080p

static bool g_click;
static int g_wheel;

static int S(float v) { return (int)(v * g_s + 0.5f); }

// The Progress Log's Minecraft book (cstv_prog.inc): a pixel of the book is g_book_f screen px, of its text and
// widgets g_book_t (finer: at the book's size a page would hold 14 lines, as in the game). 1080p: 5 and 2.
static int g_book_f = 5, g_book_t = 2;

static void book_scales(int sw, int sh)
{
    int f = std::min(sh * 17 / 20 / 180, sw * 9 / 10 / 332);   // the page 85 % of the screen high, a spread + tabs 90 % wide
    g_book_f = std::max(2, std::min(f, 12));
    g_book_t = std::max(1, 2 * g_book_f / 5);   // rounded down: a page's text never takes more of it than at 1080p
}

// Roboto Mono like the website: its TTFs ship in cstrike/custom/cstv/resource/cstv and are added with
// ISurface::AddCustomFontFile (slot 68, between the checked 67 SetFontGlyphSet and 69 GetFontTall; on Linux
// it hands the file to fontconfig, FcConfigAppFontAddFile). Without them: "Verdana" (remapped by vguimatsurface on
// Linux to the fonts the game ships, with a CJK fallback).
static void setup_fonts(int sh)
{
    static int have_mono = -1;
    if (have_mono < 0)
        have_mono = g_surf->AddCustomFontFile("Roboto Mono", "resource/cstv/RobotoMono-Regular.ttf") &
                    g_surf->AddCustomFontFile("Roboto Mono", "resource/cstv/RobotoMono-Bold.ttf");
    static const struct { int px, weight; } def[F_COUNT] = {{11, 400}, {13, 400}, {13, 700}, {17, 700}, {28, 700}, {10, 700}, {0, 400}, {0, 700}, {0, 400}, {0, 700}};
    for (int i = 0; i < F_COUNT; i++) {
        if (!g_font[i]) g_font[i] = g_surf->CreateFont();
        if (!g_font[i]) continue;
        if (i >= F_MC)   // the Progress Log's, when Minecraft's glyph pictures are missing (cstv_prog.inc draws those)
            g_surf->SetFontGlyphSet(g_font[i], have_mono ? "Roboto Mono" : "Verdana", 7 * g_book_t * (i >= F_MC2 ? 2 : 1), def[i].weight, 0, 0, vgui::ISurface::FONTFLAG_ANTIALIAS);
        else if (!have_mono || !g_surf->SetFontGlyphSet(g_font[i], "Roboto Mono", S(def[i].px), def[i].weight, 0, 0, vgui::ISurface::FONTFLAG_ANTIALIAS))
            g_surf->SetFontGlyphSet(g_font[i], "Verdana", S(def[i].px), def[i].weight, 0, 0, vgui::ISurface::FONTFLAG_ANTIALIAS);
        g_font_tall[i] = g_surf->GetFontTall(g_font[i]);
    }
    g_font_for_h = sh;
}

static void rect(int x, int y, int w, int h, Col c)
{
    g_surf->DrawSetColor(c.r, c.g, c.b, c.a);
    g_surf->DrawFilledRect(x, y, x + w, y + h);
}

static void outline(int x, int y, int w, int h, Col c)
{
    g_surf->DrawSetColor(c.r, c.g, c.b, c.a);
    g_surf->DrawOutlinedRect(x, y, x + w, y + h);
}

// a text box's or list's well
static void field_box(int x, int y, int w, int h, bool focused)
{
    rect(x, y, w, h, C_FIELD);
    outline(x, y, w, h, focused ? C_ACCENT : C_BORDER);
}

static int wtext_w(int f, const std::wstring &t)
{
    int w = 0, h = 0;
    if (g_font[f] && !t.empty()) g_surf->GetTextSize(g_font[f], t.c_str(), w, h);
    return w;
}

static int text_w(int f, const std::string &s) { return wtext_w(f, widen(s)); }

static void wtext(int f, int x, int y, Col c, const std::wstring &t)
{
    if (!g_font[f] || t.empty()) return;
    g_surf->DrawSetTextFont(g_font[f]);
    g_surf->DrawSetTextColor(c.r, c.g, c.b, c.a);
    g_surf->DrawSetTextPos(x, y);
    g_surf->DrawPrintText(t.c_str(), (int)t.size());
}

// Draws at most maxw wide (0 = no limit), cutting with an ellipsis. Returns the width drawn.
static int text(int f, int x, int y, Col c, const std::string &s, int maxw = 0)
{
    std::wstring t = widen(s);
    if (maxw > 0 && wtext_w(f, t) > maxw) {
        size_t lo = 0, hi = t.size();
        while (lo < hi) {
            size_t mid = (lo + hi + 1) / 2;
            if (wtext_w(f, t.substr(0, mid) + L"…") <= maxw) lo = mid;
            else hi = mid - 1;
        }
        t = t.substr(0, lo) + L"…";
    }
    wtext(f, x, y, c, t);
    return wtext_w(f, t);
}

static int vcenter(int f, int y, int h) { return y + (h - g_font_tall[f]) / 2; }

static bool hover(int x, int y, int w, int h) { return g_mx >= x && g_mx < x + w && g_my >= y && g_my < y + h; }

static bool take_click(int x, int y, int w, int h)
{
    if (!g_click || !hover(x, y, w, h)) return false;
    g_click = false;
    return true;
}

enum { B_NORMAL, B_ACTIVE, B_DANGER };

// The website's buttons: a 1 px border; B_ACTIVE (the main action, the picked choice) a solid blue block with
// black bold text; B_DANGER red text. A label that doesn't fit takes the small font, then an ellipsis.
static std::wstring fit(int &f, const std::wstring &t, int room)
{
    if (wtext_w(f, t) <= room) return t;
    if (f != F_SMALL && wtext_w(F_SMALL, t) <= room) { f = F_SMALL; return t; }
    f = F_SMALL;
    size_t lo = 0, hi = t.size();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (wtext_w(f, t.substr(0, mid) + L"\u2026") <= room) lo = mid;
        else hi = mid - 1;
    }
    return t.substr(0, lo) + L"\u2026";
}

static bool button(int x, int y, int w, int h, const char *label, int kind = B_NORMAL, bool enabled = true)
{
    bool hv = enabled && hover(x, y, w, h);
    Col bg = kind == B_ACTIVE ? C_ACCENT : hv ? C_BTN_HOVER : C_BTN;
    Col border = kind == B_ACTIVE ? C_ACCENT : hv ? C_ACCENT : C_BORDER;
    Col fg = kind == B_ACTIVE ? C_BLACK : kind == B_DANGER ? C_DANGER : C_TEXT;
    if (!enabled) { bg = fade(bg); border = fade(border); fg = fade(fg); }
    if (kind == B_ACTIVE && hv) bg = {106, 163, 243, 255};
    rect(x, y, w, h, bg);
    outline(x, y, w, h, border);
    int f = kind == B_ACTIVE ? F_BOLD : F_BODY;
    std::wstring t = fit(f, widen(label), w - 2 * S(6));
    wtext(f, x + (w - wtext_w(f, t)) / 2, vcenter(f, y, h), fg, t);
    return enabled && take_click(x, y, w, h);
}

// A square box with the label beside it; returns true when clicked (the caller flips its value).
static bool checkbox(int x, int y, const char *label, bool on, bool enabled = true, int maxw = 0)
{
    int b = S(14), lh = std::max(b, g_font_tall[F_BODY]);
    int lw = text_w(F_BODY, label), w = b + S(8) + (maxw ? std::min(lw, maxw - b - S(8)) : lw);
    bool hv = enabled && hover(x, y, w, lh);
    int by = y + (lh - b) / 2;
    rect(x, by, b, b, on ? C_ACCENT : C_FIELD);
    outline(x, by, b, b, on || hv ? C_ACCENT : C_BORDER);
    if (on) rect(x + b / 4, by + b / 4, b - b / 2, b - b / 2, C_BLACK);
    text(F_BODY, x + b + S(8), vcenter(F_BODY, y, lh), enabled ? C_TEXT : C_TEXT_OFF, label, maxw ? maxw - b - S(8) : 0);
    return enabled && take_click(x, y, w, lh);
}

// One-line text box with append-only editing (typing, backspace, Ctrl+V). True when Enter is pressed in it.
static bool textbox(int id, std::string &s, size_t maxlen, int x, int y, int w, int h, const char *placeholder, bool mask = false)
{
    if (take_click(x, y, w, h)) g_focus = id;
    bool focused = g_focus == id;
    bool enter = false;
    if (focused) {
        for (; in.backspaces > 0 && !s.empty(); in.backspaces--) {
            size_t i = s.size() - 1;
            while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
            s.erase(i);
        }
        std::string add = in.typed;
        if (in.paste && SDL_GetClipboardText_) {
            if (char *clip = SDL_GetClipboardText_()) {
                add += clip;
                if (SDL_free_) SDL_free_(clip);
            }
        }
        for (char c : add)
            if ((unsigned char)c >= 0x20 && s.size() < maxlen) s += c;
        enter = in.enter;
        in.typed.clear();
        in.backspaces = 0;
        in.paste = in.enter = false;
    }
    field_box(x, y, w, h, focused);
    int pad = S(10), room = w - 2 * pad;
    if (s.empty() && !focused) {
        text(F_BODY, x + pad, vcenter(F_BODY, y, h), C_TEXT_OFF, placeholder, room);
        return false;
    }
    // show the end of long text so the caret stays visible
    std::wstring t = widen(s);
    if (mask) t.assign(t.size(), L'\u2022');
    if (wtext_w(F_BODY, t) > room - S(4)) {
        size_t lo = 0, hi = t.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (wtext_w(F_BODY, t.substr(mid)) <= room - S(4)) hi = mid;
            else lo = mid + 1;
        }
        t = t.substr(lo);
    }
    wtext(F_BODY, x + pad, vcenter(F_BODY, y, h), C_TEXT, t);
    if (focused && (long)(plat_now() * 2) % 2 == 0) {
        int cx = x + pad + wtext_w(F_BODY, t) + 1;
        rect(cx, y + S(8), std::max(1, S(2)), h - S(16), C_TEXT);
    }
    return enter;
}

// True on the frame the drag ends (that's when the value is sent).
static bool slider(int id, int x, int y, int w, int h, int &val, int lo, int hi)
{
    if (take_click(x - S(8), y, w + S(16), h)) g_drag = id;
    bool done = false;
    if (g_drag == id) {
        int v = lo + (int)((long)(g_mx - x) * (hi - lo) / std::max(1, w));
        val = v < lo ? lo : v > hi ? hi : v;
        if (!in.down) { g_drag = 0; done = true; }
    }
    int ty = y + h / 2 - S(2);
    rect(x, ty, w, S(4), C_LINE);
    int fx = (int)((long)(val - lo) * w / std::max(1, hi - lo));
    rect(x, ty, fx, S(4), C_ACCENT);
    int k = S(12);
    rect(x + fx - k / 2, y + (h - k) / 2, k, k, g_drag == id || hover(x, y, w, h) ? C_WHITE : C_TEXT);
    return done;
}

// Wheel scrolling for a list of `count` rows of which `visible` fit; returns the first row to draw.
static int scroll_list(int &first, int count, int visible, int x, int y, int w, int h)
{
    if (g_wheel && hover(x, y, w, h)) { first -= g_wheel * 2; g_wheel = 0; }
    int maxf = count > visible ? count - visible : 0;
    if (first > maxf) first = maxf;
    if (first < 0) first = 0;
    if (count > visible) {
        int th = std::max(S(20), h * visible / count);
        int ty = y + (h - th) * first / std::max(1, maxf);
        rect(x + w - S(4), ty, S(4), th, C_BORDER);
    }
    return first;
}

// ---------------------------------------------------------------- the window
// (daf's Search tab, YouTube only, is left out: links from any site play)
// (Now playing, Link and Queue are on the Lobby tab's room page now, as on the website's lobby page)
enum { T_LOBBY, T_SCREEN, T_STREAM, T_SETTINGS, T_COUNT };
static int g_tab;
static std::string g_link, g_code, g_new_name, g_new_pw, g_join_pw, g_rename, g_set_pw;
static bool g_show_pw;
static int g_scroll_queue, g_scroll_roster;
static std::string g_toast;
static double g_toast_at;

static void toast(const std::string &s) { g_toast = s; g_toast_at = plat_now(); }

static void play(const std::string &url, bool queue)
{
    if (url.empty()) return;
    send("m %s %s", queue ? "queue" : "link", b64enc(url).c_str());
    toast(queue ? "Added to the queue" : "Playing");
}

// a section's name: blue capitals beside a blue bar (not a block: blocks are buttons)
static void section(int x, int &y, const char *label)
{
    rect(x, y + S(1), S(3), g_font_tall[F_LABEL] - S(2), C_ACCENT);
    text(F_LABEL, x + S(9), y, C_ACCENT, label);
    y += g_font_tall[F_LABEL] + S(9);
}

// Two or more side-by-side choices; returns the clicked index or -1.
static int choice(int x, int y, const char *const *labels, int n, int active, int bw, bool enabled = true, int bh = 0)
{
    int hit = -1;
    for (int i = 0; i < n; i++)
        if (button(x + i * (bw + S(6)), y, bw, bh ? bh : S(30), labels[i], i == active ? B_ACTIVE : B_NORMAL, enabled)) hit = i;
    return hit;
}

// ---- dropdown: a button showing the choice and a small arrow; its list opens over everything else (drawn
// last by dropdown_draw, and gets the click first in dropdown_input, so nothing under it is clicked)
static struct { int id, x, y, w, ih, n, sel, picked = -1; bool seen; const char *const *labels; int by; } g_dd;   // by: its button's top

static int dropdown(int id, int x, int y, int w, int h, const char *const *labels, int n, int sel, bool enabled = true)
{
    int ret = -1;
    if (g_dd.id == id) {
        g_dd.seen = true;
        if (g_dd.picked >= 0) {
            ret = g_dd.picked;
            g_dd.id = 0;
            g_dd.picked = -1;
        }
    }
    std::string label = std::string(labels[sel >= 0 && sel < n ? sel : 0]);
    bool hv = enabled && hover(x, y, w, h), open = g_dd.id == id;
    Col fg = enabled ? C_TEXT : fade(C_TEXT);
    rect(x, y, w, h, hv || open ? C_BTN_HOVER : C_FIELD);
    outline(x, y, w, h, open || hv ? C_ACCENT : enabled ? C_BORDER : fade(C_BORDER));
    int f = F_BODY;
    std::wstring t = fit(f, widen(label), w - S(30));
    wtext(f, x + S(10), vcenter(f, y, h), fg, t);
    int a = S(4), cx = x + w - S(14), cy = y + h / 2 - a / 2;
    for (int i = 0; i < a; i++) rect(cx - (a - i), cy + i, 2 * (a - i), 1, enabled ? C_SUB : fade(C_SUB));
    if (enabled && take_click(x, y, w, h)) {
        if (open) g_dd.id = 0;
        else {   // opens downward, or upward when the screen ends first
            int ly = y + h + n * h <= g_font_for_h ? y + h : y - n * h;
            g_dd = {id, x, ly, w, h, n, sel, -1, true, labels, y};
        }
    }
    return ret;
}

static void dropdown_input()
{
    g_dd.seen = false;
    if (!g_dd.id || !g_click) return;
    if (hover(g_dd.x, g_dd.y, g_dd.w, g_dd.ih * g_dd.n)) {
        g_dd.picked = (g_my - g_dd.y) / g_dd.ih;
        g_click = false;
    } else if (!hover(g_dd.x, g_dd.by, g_dd.w, g_dd.ih)) {   // outside (its own button toggles it)
        g_dd.id = 0;
        g_click = false;
    }
}

static void dropdown_draw()
{
    if (!g_dd.id) return;
    if (!g_dd.seen) { g_dd.id = 0; return; }   // its page isn't shown any more
    rect(g_dd.x, g_dd.y, g_dd.w, g_dd.ih * g_dd.n, C_SIDE);
    for (int i = 0; i < g_dd.n; i++) {
        int y = g_dd.y + i * g_dd.ih;
        bool on = i == g_dd.sel, hv = hover(g_dd.x, y, g_dd.w, g_dd.ih);
        if (on || hv) rect(g_dd.x, y, g_dd.w, g_dd.ih, on ? C_ACCENT : C_TAB_HOVER);
        int f = on ? F_BOLD : F_BODY;
        wtext(f, g_dd.x + S(10), vcenter(f, y, g_dd.ih), on ? C_BLACK : C_TEXT, widen(g_dd.labels[i]));
    }
    outline(g_dd.x, g_dd.y, g_dd.w, g_dd.ih * g_dd.n, C_BORDER);
}

static void roster_list(int x, int y, int w, int lh)
{
    int rh = S(50), fit = std::max(1, lh / rh);
    int first = scroll_list(g_scroll_roster, (int)st.roster.size(), fit, x, y, w, lh);
    bool can_manage = st.owner && st.room_kind == "lobby";
    int bx = x + w - S(10) - 3 * S(96);
    for (int i = first; i < (int)st.roster.size() && i < first + fit; i++) {
        const Member &m = st.roster[i];
        int ry = y + (i - first) * rh;
        rect(x, ry, w - S(10), rh - S(6), C_ROW);
        bool me = m.id == st.my_id;
        int tx = x + S(12), tw = (can_manage ? bx : x + w) - tx - S(110);
        int nw = text(F_BOLD, tx, ry + S(5), C_TEXT, (m.name.empty() ? "player " + m.id : m.name) + (me ? " (you)" : ""), tw);
        if (m.id == st.roster_owner) {
            int bxo = tx + nw + S(10), bwo = text_w(F_SMALL, "OWNER") + S(12);
            rect(bxo, ry + S(6), bwo, g_font_tall[F_SMALL] + S(4), C_ACCENT);
            text(F_SMALL, bxo + S(6), ry + S(8), C_WHITE, "OWNER");
        }
        if (!m.where.empty()) text(F_SMALL, tx, ry + S(9) + g_font_tall[F_BOLD], C_SUB, m.where, tw + S(100));
        if (can_manage && !me) {
            int by = ry + (rh - S(6) - S(32)) / 2;
            if (button(bx, by, S(90), S(32), "Make owner")) send("m lobby owner %s", m.id.c_str());
            if (button(bx + S(96), by, S(90), S(32), "Kick")) send("m lobby kick %s", m.id.c_str());
            if (button(bx + 2 * S(96), by, S(90), S(32), "Ban", B_DANGER)) send("m lobby ban %s", m.id.c_str());
        }
    }
}

static std::string b64_or_dash(const std::string &v) { return v.empty() ? "-" : b64enc(v); }

static std::string my_name()
{
    for (auto &m : st.roster)
        if (m.id == st.my_id && !m.name.empty()) return m.name;
    return "Your";
}

static void join_lobby(const std::string &code, const std::string &pw)
{
    std::string c = code;
    for (char &ch : c) ch = (char)toupper((unsigned char)ch);
    if (pw.empty()) send("m lobby join %s", c.c_str());
    else send("m lobby join %s %s", c.c_str(), b64enc(pw).c_str());
    st.notice.clear();
}

static std::string g_login_code;

// ---- every lobby on cstv.xyz, a page at a time (most people first, then the oldest)
static int g_lobby_view = -1;           // 0 = this lobby / server, 1 = all lobbies, 2 = join or create; -1 = pick
static std::string g_join_target;       // joined from the list: show that lobby once we're in
static std::string g_pw_code, g_pw_text; // the locked lobby whose password row is open

static void ask_lobbies(int page)
{
    st.lobbies_at = plat_now();
    send("m lobbies %d", std::max(1, page));
}

static void lobby_browser(int x, int y, int w, int h)
{
    int end = y + h, bh = S(34), gap = S(8);
    if (!st.master) {
        text(F_TITLE, x, y, C_TEXT, "Not connected to cstv.xyz", w);
        return;
    }
    if (plat_now() - st.lobbies_at > 10) ask_lobbies(st.lobby_page);   // fresh every 10 s while it's shown
    char head[64];
    snprintf(head, sizeof head, "LOBBIES  \xC2\xB7  %d", st.lobby_total);
    int hy = vcenter(F_LABEL, y, bh);
    section(x, hy, head);
    int rx = x + w - S(10);
    if (button(rx - S(100), y, S(100), bh, "Refresh")) ask_lobbies(st.lobby_page);
    rx -= S(100) + S(20);
    if (button(rx - S(80), y, S(80), bh, "Next", B_NORMAL, st.lobby_page < st.lobby_pages)) ask_lobbies(st.lobby_page + 1);
    rx -= S(80) + gap;
    char pg[32];
    snprintf(pg, sizeof pg, "%d / %d", st.lobby_page, st.lobby_pages);
    int pw = text_w(F_BODY, pg);
    text(F_BODY, rx - pw, vcenter(F_BODY, y, bh), C_TEXT, pg);
    rx -= pw + gap;
    if (button(rx - S(80), y, S(80), bh, "Prev", B_NORMAL, st.lobby_page > 1)) ask_lobbies(st.lobby_page - 1);
    y += bh + S(10);
    if (!st.notice.empty()) {
        text(F_BODY, x, y, C_BAD, st.notice, w);
        y += g_font_tall[F_BODY] + S(8);
    }
    if (st.lobbies.empty()) {
        text(F_BODY, x, y, C_SUB, plat_now() - st.lobbies_at < 2 ? "Loading\xE2\x80\xA6" : "No lobbies yet. Make one in Join or create.", w);
        return;
    }
    int rh = S(52);   // 8 rows (a page) fit under the header at 1080p, with a notice line
    for (const auto &L : st.lobbies) {
        if (y + rh - S(6) > end) break;
        int ih = rh - S(6), rw = w - S(10);
        rect(x, y, rw, ih, hover(x, y, rw, ih) ? C_TAB_HOVER : C_SIDE);
        outline(x, y, rw, ih, C_LINE);
        bool here = st.room_kind == "lobby" && st.room_name == L.code, asking = g_pw_code == L.code;
        int bx = x + rw - S(10) - S(100);
        // the password row, right-aligned: Password: [______] [Join] [Cancel]
        int cbx = x + rw - S(10) - S(90), jbx = cbx - S(8) - S(80), tbx = jbx - S(8) - S(200);
        int lbx = tbx - S(8) - text_w(F_BODY, "Password:");
        int left = asking ? lbx - S(10) : bx - S(90);   // where the name's room ends
        int tx = x + S(14), tw = left - tx - S(10);
        bool tag = L.locked && !asking;
        int nw = text(F_BOLD, tx, y + S(5), C_TEXT, L.name.empty() ? L.code : L.name, tag ? tw - S(90) : tw);
        if (tag) {
            int lw = text_w(F_LABEL, "PASSWORD") + S(10);
            outline(tx + nw + S(10), y + S(7), lw, g_font_tall[F_LABEL] + S(2), C_BORDER);
            text(F_LABEL, tx + nw + S(15), y + S(8), C_SUB, "PASSWORD");
        }
        std::string sub = L.code + (L.playing.empty() ? "" : "  \xC2\xB7  " + L.playing);
        text(F_SMALL, tx, y + S(7) + g_font_tall[F_BOLD], C_SUB, sub, tw);
        int by = y + (ih - bh) / 2;
        if (asking) {
            text(F_BODY, lbx, vcenter(F_BODY, by, bh), C_TEXT, "Password:");
            bool go = textbox(10, g_pw_text, 64, tbx, by, S(200), bh, "", true);
            if (button(jbx, by, S(80), bh, "Join", B_ACTIVE) || go) {
                join_lobby(L.code, g_pw_text);   // empty: those who got in before don't need it again
                g_pw_text.clear();
                g_join_target = L.code;
            }
            if (button(cbx, by, S(90), bh, "Cancel")) g_pw_code.clear();
        } else {
            char ppl[16];
            snprintf(ppl, sizeof ppl, "%d", L.people);
            int cw = text_w(F_BODY, ppl), cx = bx - S(30) - cw;
            rect(cx - S(14), y + (ih - S(8)) / 2, S(8), S(8), L.people ? C_GOOD : C_TEXT_OFF);
            text(F_BODY, cx, vcenter(F_BODY, y, ih), C_TEXT, ppl);
        }
        if (here) text(F_BOLD, bx + (S(100) - text_w(F_BOLD, "Here")) / 2, vcenter(F_BOLD, by, bh), C_GOOD, "Here");
        else if (!asking && button(bx, by, S(100), bh, "Join", B_ACTIVE)) {
            st.notice.clear();
            if (L.locked) {
                g_pw_code = L.code;
                g_pw_text.clear();
                g_focus = 10;
            } else {
                join_lobby(L.code, "");
                g_join_target = L.code;
            }
        }
        y += rh;
    }
}

// ---- the room page (Lobby tab > This lobby / This server): the website's lobby page without leaving the game.
// Left: the video (the screen's own picture), what plays, the controls, a link box. Right: Chat (a picture
// cstv_overlay.py draws: emotes, GIFs, its emote / GIF pickers) / Queue / History / People / Settings.
enum { LT_CHAT, LT_QUEUE, LT_HISTORY, LT_PEOPLE, LT_SETTINGS, LT_LIVE };
static int g_ltab = -1, g_set_view;
static int g_chat_mode;                 // 0 chat, 1 emote picker, 2 GIF picker
static std::string g_chat_text, g_pick_search, g_search_sent, g_side_link, g_emote_ch;
static double g_search_at;              // the picker's search box last changed
static int g_scroll_side;
static double g_preview_at, g_lchat_at; // when the page last drew the video / the chat
static int g_lchat_w, g_lchat_h, g_lchat_s;
static bool g_lchat_showing;            // the renderer was told "show 1"
static int g_preview_vtex = -1, g_lchat_vtex = -1;
static double g_delete_armed;           // Delete lobby: clicked once, confirm within 4 s

static long play_pos_ms()
{
    long ms = st.pos_ms;
    if (st.have_state && st.running && !st.paused) ms += (long)((plat_now() - st.state_at) * 10.0 * st.speed);
    return ms;
}

static void set_chat_mode(int m)
{
    g_chat_mode = m;
    g_pick_search.clear();
    g_search_sent.clear();
    send("lchat mode %s", m == 0 ? "chat" : m == 1 ? "emotes" : "gifs");
    if (m == 2) send("m gifs - 0");   // trending first
    g_focus = m ? 12 : 11;
}

// a word picked in the emote picker goes into the message
static void chat_insert(const std::string &word)
{
    if (!g_chat_text.empty() && g_chat_text.back() != ' ') g_chat_text += ' ';
    if (g_chat_text.size() + word.size() < 300) g_chat_text += word + " ";
}

static void sign_in_row(int x, int y, int w, int bh)
{
    int bw = S(90);
    bool go = textbox(9, g_login_code, 8, x, y, w - bw - S(6), bh, "Sign-in code (cstv.xyz/link)");
    if ((button(x + w - bw, y, bw, bh, "Sign in", B_ACTIVE, g_login_code.size() >= 4) || go) && g_login_code.size() >= 4) {
        std::string c = g_login_code;
        for (char &ch : c) ch = (char)toupper((unsigned char)ch);
#ifdef _WIN32
        send("m login %s windows", c.c_str());
#else
        send("m login %s linux", c.c_str());
#endif
        g_login_code.clear();
    }
}

// The chat picture: /tmp/cstv/lchat (cstv_overlay.py, RGBA at exactly the size asked for) into a VGUI texture
// with ISurface::DrawSetTextureRGBA (slot 31: VGUI makes and keeps the texture itself, like Steam avatars), at
// most 30 times a second, drawn 1:1 with DrawTexturedSubRect (slot 101).
// VGUI makes the texture at the size of the FIRST upload and never again: a later upload of another size is
// copied row by row into the top-left of it with no bounds check (vguimatsurface 0x114c60 -> 0x1144b0), so a
// bigger one overruns the heap (crashed 2026-10-18) and any other size is drawn with the wrong proportions. So the
// texture is made once at LC_TW x LC_TH, bigger than any picture, and each picture goes into its top-left.
static const int LC_TW = 1024, LC_TH = 2048;
static_assert(CSTV_LCHAT_MAXW <= LC_TW && CSTV_LCHAT_MAXH <= LC_TH, "the chat texture must hold any picture");
static cstv_lchat_shm *g_lc;
static std::vector<unsigned char> g_lc_px;
static uint64_t g_lc_seq;
static int g_lc_w, g_lc_h;
static double g_lc_at;

static bool lchat_picture(int w, int h)
{
    if (!g_lc) {
        static double tried;
        if (plat_now() - tried < 1) return false;
        tried = plat_now();
        void *p = plat_map(CSTV_LCHAT_PATH, sizeof(cstv_lchat_shm));
        if (!p) return false;
        if (((cstv_lchat_shm *)p)->magic != CSTV_LCHAT_MAGIC) { plat_unmap(p, sizeof(cstv_lchat_shm)); return false; }
        g_lc = (cstv_lchat_shm *)p;
    }
    uint64_t seq = __atomic_load_n(&g_lc->seq, __ATOMIC_ACQUIRE);
    if (seq != g_lc_seq && plat_now() - g_lc_at > 1.0 / 30) {
        uint32_t idx = __atomic_load_n(&g_lc->latest, __ATOMIC_ACQUIRE);
        int pw = (int)g_lc->w, ph = (int)g_lc->h;
        // only a picture of the size asked for (the file can still hold the last game's, at its old size)
        if (idx <= 2 && pw == w && ph == h && pw >= 16 && ph >= 16 && pw <= CSTV_LCHAT_MAXW && ph <= CSTV_LCHAT_MAXH) {
            if (g_lchat_vtex < 0) {   // made once, at its full size
                g_lchat_vtex = g_surf->CreateNewTextureID(true);
                g_lc_px.assign((size_t)LC_TW * LC_TH * 4, 0);
                g_surf->DrawSetTextureRGBA(g_lchat_vtex, g_lc_px.data(), LC_TW, LC_TH, 0, true);
            }
            g_lc_px.resize((size_t)pw * ph * 4);
            __atomic_store_n(&g_lc->reading, idx, __ATOMIC_RELEASE);
            memcpy(g_lc_px.data(), g_lc->buf[idx], g_lc_px.size());
            __atomic_store_n(&g_lc->reading, CSTV_NONE, __ATOMIC_RELEASE);
            g_surf->DrawSetTextureRGBA(g_lchat_vtex, g_lc_px.data(), pw, ph, 0, true);   // into the top-left
            g_lc_w = pw;
            g_lc_h = ph;
            g_lc_seq = seq;
            g_lc_at = plat_now();
        }
    }
    return g_lchat_vtex >= 0 && g_lc_w == w && g_lc_h == h;
}

// the chat text size (Settings > Lobby chat): the renderer's scale on top of the window's
static int chat_text_pct()
{
    const std::string &t = st.chat_text;   // ("small" was the first Normal)
    return t == "smaller" ? 72 : t == "large" ? 100 : t == "larger" ? 115 : t == "largest" ? 130 : 85;
}

// @name completion with Tab in the message box: the last word, if it starts with @, becomes the next lobby
// member whose name starts with what's typed (Tab again: the next one)
static void complete_mention(std::string &msg)
{
    static std::string stem, last;   // what was typed before the first Tab; what we put in
    static size_t pick;
    size_t at = msg.rfind('@');
    if (at == std::string::npos || (at > 0 && msg[at - 1] != ' ')) return;
    std::string word = msg.substr(at + 1);
    if (msg != last) { stem = word; pick = 0; }   // typed since: start over from what's there
    auto lower = [](std::string v) { for (char &c : v) c = (char)tolower((unsigned char)c); return v; };
    std::vector<std::string> names;
    for (const Member &m : st.roster)
        if (!m.name.empty() && m.id != st.my_id && lower(m.name).compare(0, stem.size(), lower(stem)) == 0) names.push_back(m.name);
    if (names.empty()) return;
    msg = msg.substr(0, at + 1) + names[pick++ % names.size()] + " ";
    last = msg;
}

// The @ list (like Chatterino): lobby members whose name starts with the word being typed after an @
static bool mention_list(const std::string &msg, std::vector<std::string> &names, size_t &at)
{
    at = msg.rfind('@');
    if (at == std::string::npos || (at > 0 && msg[at - 1] != ' ')) return false;
    std::string word = msg.substr(at + 1);
    if (word.find(' ') != std::string::npos) return false;
    auto lower = [](std::string v) { for (char &c : v) c = (char)tolower((unsigned char)c); return v; };
    std::string lw = lower(word);
    for (const Member &m : st.roster)
        if (!m.name.empty() && m.id != st.my_id && lower(m.name).compare(0, lw.size(), lw) == 0 &&
            std::find(names.begin(), names.end(), m.name) == names.end())
            names.push_back(m.name);
    return !names.empty();
}

static void side_chat(int x, int y, int w, int h)
{
    int ih = S(28), gap = S(4);
    bool picking = g_chat_mode != 0 && !st.you_sid.empty();
    // the picture; under it the message row, and in a picker also its [Back] [search] row
    int ph = h - ih - S(6) - (picking ? ih + gap : 0);
    int sc = (int)(g_s * chat_text_pct() + 0.5f);
    static double synced;
    static std::string draft_sent = "\x01";
    static double draft_at;
    // the renderer draws it at exactly this size; told again every few seconds (cstvd or the renderer may
    // have started again meanwhile and know nothing)
    if (w != g_lchat_w || ph != g_lchat_h || sc != g_lchat_s || !g_lchat_showing || plat_now() - synced > 3) {
        g_lchat_w = w;
        g_lchat_h = ph;
        g_lchat_s = sc;
        g_lchat_showing = true;
        synced = plat_now();
        send("lchat size %d %d %d", w, ph, sc);
        send("lchat show 1");
        send("lchat mode %s", g_chat_mode == 0 ? "chat" : g_chat_mode == 1 ? "emotes" : "gifs");
        if (!st.you_name.empty()) send("lchat me %s", b64enc(st.you_name).c_str());
        draft_sent = "\x01";   // and the draft again
    }
    // the message being written, so the renderer shows its emotes before it's sent
    if (g_chat_text != draft_sent && plat_now() - draft_at > 0.12) {
        draft_sent = g_chat_text;
        draft_at = plat_now();
        send("lchat draft %s", b64_or_dash(g_chat_text).c_str());
    }
    g_lchat_at = plat_now();
    if (lchat_picture(w, ph)) {
        g_surf->DrawSetColor(255, 255, 255, 255);
        g_surf->DrawSetTexture(g_lchat_vtex);
        g_surf->DrawTexturedSubRect(x, y, x + w, y + ph, 0, 0, (float)w / LC_TW, (float)ph / LC_TH);
    } else if (g_lchat_vtex >= 0 && g_lc_w == w && g_lc_h > 0) {   // resized: the last picture until the new one
        int dh = std::min(ph, g_lc_h);
        rect(x, y, w, ph, C_FIELD);
        g_surf->DrawSetColor(255, 255, 255, 255);
        g_surf->DrawSetTexture(g_lchat_vtex);
        g_surf->DrawTexturedSubRect(x, y + ph - dh, x + w, y + ph, 0, (float)(g_lc_h - dh) / LC_TH, (float)w / LC_TW, (float)g_lc_h / LC_TH);
    } else {
        rect(x, y, w, ph, C_FIELD);
        text(F_SMALL, x + S(10), y + S(10), C_TEXT_OFF, "Loading chat\xE2\x80\xA6", w - S(20));
    }
    outline(x, y, w, ph, C_LINE);
    if (g_wheel && hover(x, y, w, ph)) {
        send("lchat scroll %d", g_wheel);
        g_wheel = 0;
    }
    if (g_chat_mode && take_click(x, y, w, ph)) send("lchat click %d %d", g_mx - x, g_my - y);

    int ry = y + ph + S(6);
    if (st.you_sid.empty()) {   // guests watch; chatting needs the cstv.xyz account
        sign_in_row(x, ry, w, ih);
        return;
    }
    if (picking) {   // [Back] [search], the message row under it
        int bw = S(56);
        if (button(x, ry, bw, ih, "Back")) set_chat_mode(0);
        std::string before = g_pick_search;
        textbox(12, g_pick_search, 50, x + bw + gap, ry, w - bw - gap, ih, g_chat_mode == 1 ? "Search emotes" : "Search GIFs");
        if (g_pick_search != before) g_search_at = plat_now();
        if (g_pick_search != g_search_sent && plat_now() - g_search_at > 0.35) {
            g_search_sent = g_pick_search;
            send("lchat search %s", b64_or_dash(g_pick_search).c_str());
            if (g_chat_mode == 2) send("m gifs %s 0", b64_or_dash(g_pick_search).c_str());
        }
        ry += ih + gap;
    }
    int ew = S(32), gw = S(40), sw = S(56);
    if (button(x, ry, ew, ih, ":)", g_chat_mode == 1 ? B_ACTIVE : B_NORMAL)) set_chat_mode(g_chat_mode == 1 ? 0 : 1);
    if (button(x + ew + gap, ry, gw, ih, "GIF", g_chat_mode == 2 ? B_ACTIVE : B_NORMAL, st.l_gifs)) set_chat_mode(g_chat_mode == 2 ? 0 : 2);
    int tx = x + ew + gw + 2 * gap, tw = w - (tx - x) - sw - gap;
    if (g_focus == 11 && in.tabs > 0) {
        for (; in.tabs > 0; in.tabs--) complete_mention(g_chat_text);
    }
    bool go = textbox(11, g_chat_text, 300, tx, ry, tw, ih, "Message");
    if ((button(x + w - sw, ry, sw, ih, "Send", B_ACTIVE, !g_chat_text.empty()) || go) && !g_chat_text.empty()) {
        send("m chat %s", b64enc(g_chat_text).c_str());
        g_chat_text.clear();
        g_focus = 11;
    }
    // the @ list over the bottom of the chat: click a name, or Tab for the first (Tab again: the next)
    std::vector<std::string> names;
    size_t at;
    if (g_focus == 11 && mention_list(g_chat_text, names, at)) {
        int n = std::min((int)names.size(), 6), rh = std::max(S(22), g_font_tall[F_BODY] + S(6));
        int lw = std::min(w, S(240)), ly = (picking ? ry - ih - gap : ry) - n * rh - S(4);
        rect(x, ly, lw, n * rh, C_SIDE);
        for (int i = 0; i < n; i++) {
            int iy = ly + i * rh;
            bool hv = hover(x, iy, lw, rh);
            if (hv || i == 0) rect(x, iy, lw, rh, hv ? C_TAB_HOVER : C_PANEL2);
            int nw = text(F_BOLD, x + S(10), vcenter(F_BOLD, iy, rh), C_ACCENT, "@", lw);
            text(F_BODY, x + S(10) + nw, vcenter(F_BODY, iy, rh), C_TEXT, names[i], lw - S(20) - nw);
            if (take_click(x, iy, lw, rh)) {
                g_chat_text = g_chat_text.substr(0, at + 1) + names[i] + " ";
                g_focus = 11;
            }
        }
        outline(x, ly, lw, n * rh, C_ACCENT);
    }
}

// The Live tab: the stream's own chat (Twitch, YouTube live), the picture cstvd's renderer draws for the panel
// beside the screen (cstv/chat, 512x768), fitted to the tab. Asked for while the tab is open (xchat 1).
static int g_xchat_vtex = -1;
static double g_xchat_at;

static void side_live(int x, int y, int w, int h)
{
    static double asked;
    if (plat_now() - asked > 3) {   // again now and then: cstvd may have started again
        asked = plat_now();
        send("xchat 1");
    }
    g_xchat_at = plat_now();
    if (st.chat_state == 0) {
        rect(x, y, w, h, C_FIELD);
        outline(x, y, w, h, C_LINE);
        text(F_SMALL, x + S(10), y + S(10), C_TEXT_OFF, "Loading chat\xE2\x80\xA6", w - S(20));
        return;
    }
    if (g_xchat_vtex < 0) {
        g_xchat_vtex = g_surf->CreateNewTextureID(false);
        g_surf->DrawSetTextureFile(g_xchat_vtex, "cstv/chat", true, false);
    }
    int dw = w, dh = w * 768 / 512;
    if (dh > h) { dh = h; dw = h * 512 / 768; }
    int dx = x + (w - dw) / 2;
    rect(x, y, w, h, C_FIELD);
    g_surf->DrawSetColor(255, 255, 255, 255);
    g_surf->DrawSetTexture(g_xchat_vtex);
    g_surf->DrawTexturedRect(dx, y, dx + dw, y + dh);
    outline(x, y, w, h, C_LINE);
}

// a row of a side list: the website's tables (a line under each row)
static void list_row(int x, int y, int w, int h)
{
    bool hv = hover(x, y, w, h);
    if (hv) rect(x, y, w, h, C_SIDE);
    rect(x, y + h - 1, w, 1, C_LINE);
}

static void side_queue(int x, int y, int w, int h)
{
    int rh = S(44), bh = S(24), end = y + h;
    if (st.queue.empty()) {
        text(F_SMALL, x, y + S(4), C_TEXT_OFF, "Empty", w);
        return;
    }
    text(F_SMALL, x, vcenter(F_SMALL, y, bh), C_SUB, std::to_string(st.queue.size()) + " queued");
    if (button(x + w - S(70), y, S(70), bh, "Clear", B_DANGER)) send("m clearqueue");
    y += bh + S(8);
    int fit = std::max(1, (end - y) / rh);
    int first = scroll_list(g_scroll_side, (int)st.queue.size(), fit, x, y, w, end - y);
    for (int i = first; i < (int)st.queue.size() && i < first + fit; i++) {
        const QItem &q = st.queue[i];
        int ry = y + (i - first) * rh, bx = x + w - S(10) - 2 * S(46) - S(4);
        list_row(x, ry, w - S(8), rh);
        text(F_BODY, x + S(4), ry + S(6), C_TEXT, q.title.empty() ? q.url : q.title, bx - x - S(12));
        if (!q.by.empty()) text(F_SMALL, x + S(4), ry + S(8) + g_font_tall[F_BODY], C_SUB, q.by, bx - x - S(12));
        int by = ry + (rh - bh) / 2;
        if (button(bx, by, S(46), bh, "Play")) send("m playnow %s", q.uid.c_str());
        if (button(bx + S(50), by, S(46), bh, "\xC3\x97", B_DANGER)) send("m unqueue %s", q.uid.c_str());
    }
}

static std::string ago(long at)
{
    long s = std::max(0L, (long)time(nullptr) - at);
    return s < 60 ? "now" : s < 3600 ? std::to_string(s / 60) + "m" : s < 86400 ? std::to_string(s / 3600) + "h" : std::to_string(s / 86400) + "d";
}

static void side_history(int x, int y, int w, int h)
{
    int rh = S(44), bh = S(24), end = y + h;
    if (st.history.empty()) {
        text(F_SMALL, x, y + S(4), C_TEXT_OFF, "Empty", w);
        return;
    }
    int fit = std::max(1, (end - y) / rh);
    int first = scroll_list(g_scroll_side, (int)st.history.size(), fit, x, y, w, end - y);
    for (int i = first; i < (int)st.history.size() && i < first + fit; i++) {
        const auto &e = st.history[i];
        int ry = y + (i - first) * rh, bx = x + w - S(10) - 2 * S(56) - S(4);
        list_row(x, ry, w - S(8), rh);
        text(F_BODY, x + S(4), ry + S(6), C_TEXT, e.title.empty() ? e.url : e.title, bx - x - S(12));
        std::string sub = e.by + (e.at ? (e.by.empty() ? "" : "  ") + ago(e.at) : "");
        text(F_SMALL, x + S(4), ry + S(8) + g_font_tall[F_BODY], C_SUB, sub, bx - x - S(12));
        int by = ry + (rh - bh) / 2;
        if (button(bx, by, S(56), bh, "Play")) send("m link %s", b64enc(e.url).c_str());
        if (button(bx + S(60), by, S(56), bh, "Queue")) send("m queue %s", b64enc(e.url).c_str());
    }
}

static void side_people(int x, int y, int w, int h)
{
    int rh = S(42), bh = S(24), end = y + h;
    bool manage = st.owner && st.room_kind == "lobby";
    int fit = std::max(1, (end - y) / rh);
    int first = scroll_list(g_scroll_side, (int)st.roster.size(), fit, x, y, w, end - y);
    for (int i = first; i < (int)st.roster.size() && i < first + fit; i++) {
        const Member &m = st.roster[i];
        int ry = y + (i - first) * rh;
        bool me = m.id == st.my_id;
        int bx = x + w - S(10) - 2 * S(46) - S(4);
        list_row(x, ry, w - S(8), rh);
        int tw = (manage && !me ? bx : x + w - S(10)) - x - S(12);
        int nw = text(F_BODY, x + S(4), ry + S(5), C_TEXT, (m.name.empty() ? "player " + m.id : m.name) + (me ? " (you)" : ""), tw);
        if (m.id == st.roster_owner && nw + S(64) < tw) {
            int ox = x + S(4) + nw + S(8), ow = text_w(F_LABEL, "OWNER") + S(10);
            rect(ox, ry + S(6), ow, g_font_tall[F_LABEL] + S(2), C_ACCENT);
            text(F_LABEL, ox + S(5), ry + S(7), C_BLACK, "OWNER");
        }
        if (!m.where.empty()) text(F_SMALL, x + S(4), ry + S(7) + g_font_tall[F_BODY], C_SUB, m.where, tw);
        if (manage && !me) {
            int by = ry + (rh - bh) / 2;
            if (button(bx, by, S(46), bh, "Kick")) send("m lobby kick %s", m.id.c_str());
            if (button(bx + S(50), by, S(46), bh, "Ban", B_DANGER)) send("m lobby ban %s", m.id.c_str());
        }
    }
}

// The lobby's settings, as the website's Settings tab: General (name, password, allowed sites, delete), Chat (slow
// mode, GIFs, emotes channel), Access (what everyone may do).
static void side_settings(int x, int y, int w, int h)
{
    int bh = S(28), gap = S(6), bw = S(64);
    static const char *const views[] = {"General", "Chat", "Access"};
    int pick = choice(x, y, views, 3, g_set_view, (w - 2 * S(6)) / 3, true, S(26));
    if (pick >= 0) g_set_view = pick;
    y += S(26) + S(14);
    if (g_set_view == 0) {
        section(x, y, "NAME");
        bool go = textbox(7, g_rename, 40, x, y, w - bw - gap, bh, st.room_label.c_str());
        if ((button(x + w - bw, y, bw, bh, "Save", B_NORMAL, !g_rename.empty()) || go) && !g_rename.empty()) {
            send("m lobby name %s", b64enc(g_rename).c_str());
            g_rename.clear();
        }
        y += bh + S(14);
        std::string pwl = st.room_pw.empty() ? "PASSWORD  \xC2\xB7  NONE" : "PASSWORD  \xC2\xB7  " + (g_show_pw ? st.room_pw : std::string("\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2"));
        section(x, y, pwl.c_str());
        go = textbox(8, g_set_pw, 64, x, y, w - bw - gap, bh, "New password", true);
        if ((button(x + w - bw, y, bw, bh, "Set", B_NORMAL, !g_set_pw.empty()) || go) && !g_set_pw.empty()) {
            send("m lobby password %s", b64enc(g_set_pw).c_str());
            g_set_pw.clear();
        }
        y += bh + gap;
        if (button(x, y, S(76), bh, g_show_pw ? "Hide" : "Show", B_NORMAL, !st.room_pw.empty())) g_show_pw = !g_show_pw;
        if (button(x + S(82), y, w - S(82), bh, "Remove password", B_DANGER, st.room_locked)) send("m lobby password -");
        y += bh + S(14);
        section(x, y, "ALLOWED SITES");
        static const char *const sites[] = {"Any site", "YouTube + Twitch"};
        int hit = choice(x, y, sites, 2, st.l_sites == "yt_twitch" ? 1 : 0, (w - S(6)) / 2, true, bh);
        if (hit >= 0) send("m lobby set sites %s", hit ? "yt_twitch" : "any");
        y += bh + S(14);
        if (!st.roster_owner.empty() && st.roster_owner == st.my_id) {   // only the owner may delete it
            section(x, y, "LOBBY");
            bool armed = plat_now() - g_delete_armed < 4;
            if (button(x, y, w, bh, armed ? "Click again to delete" : "Delete lobby", B_DANGER)) {
                if (armed) send("m lobby delete");
                g_delete_armed = armed ? 0 : plat_now();
            }
        }
    } else if (g_set_view == 1) {
        section(x, y, "SLOW MODE");
        static const char *const slows[] = {"Off", "3s", "5s", "10s", "30s", "1m", "2m"};
        static const int slowv[] = {0, 3, 5, 10, 30, 60, 120};
        int cur = 0;
        for (int i = 0; i < 7; i++)
            if (st.l_slow == slowv[i]) cur = i;
        int hit = dropdown(3, x, y, S(140), bh, slows, 7, cur);
        if (hit >= 0) send("m lobby set slow %d", slowv[hit]);
        y += bh + S(14);
        section(x, y, "GIFS");
        static const char *const onoff[] = {"On", "Off"};
        hit = choice(x, y, onoff, 2, st.l_gifs ? 0 : 1, S(70), true, bh);
        if (hit >= 0) send("m lobby set gifs %d", hit == 0 ? 1 : 0);
        y += bh + S(14);
        section(x, y, "EMOTES CHANNEL");
        bool go = textbox(14, g_emote_ch, 25, x, y, w - bw - gap, bh, st.l_emotes.empty() ? "Twitch channel" : st.l_emotes.c_str());
        if (button(x + w - bw, y, bw, bh, "Save") || go) {
            send("m lobby set emotes %s", b64_or_dash(g_emote_ch).c_str());
            g_emote_ch.clear();
        }
    } else {
        section(x, y, "EVERYONE CAN");
        static const char *const names[] = {"Play links", "Add to queue", "Skip", "Pause", "Seek", "Speed & loop", "Place the TV"};
        int colw = (w - S(10)) / 2, rh = std::max(S(16), g_font_tall[F_BODY]) + S(10);
        for (int i = 0; i < 7; i++) {
            int bit = 1 << i, cx = x + (i % 2) * (colw + S(10)), cy = y + (i / 2) * rh;
            if (checkbox(cx, cy, names[i], (st.l_perms & bit) != 0, true, colw)) send("m lobby set perms %d", st.l_perms ^ bit);
        }
    }
}

static void room_page(int x, int y, int w, int h, int top_x)
{
    int end = y + h, gap = S(6), bh = S(28);
    bool lobby = st.room_kind == "lobby";
    // top right: the lobby's name and code under it (a server room: its address); invite / leave beside them
    std::string name = lobby ? (st.room_label.empty() ? st.room_name : st.room_label) : "Server";
    std::string sub = lobby ? "CODE " + st.room_name + (st.room_locked ? "  \xC2\xB7  PASSWORD" : "") : st.room_name;
    int nw = std::min(std::max(text_w(F_BOLD, name), text_w(F_SMALL, sub)), S(240));
    int ty = y - S(30) - S(14);   // on the row of the view switch
    text(F_BOLD, x + w - nw, ty, C_TEXT, name, nw);
    text(F_SMALL, x + w - nw, ty + g_font_tall[F_BOLD] + S(2), C_SUB, sub, nw);
    int bx = x + w - nw - S(14) - S(70) - gap - S(76);
    if (lobby && bx > top_x + S(8)) {
        if (button(bx, ty + S(2), S(76), bh, "Invite") && SDL_SetClipboardText_) {
            std::string invite = "CS:TV lobby " + st.room_name + " (in game: !join " + st.room_name + ", or cstv.xyz/l/" + st.room_name + ")";
            if (!st.room_pw.empty()) invite += ", password " + st.room_pw;
            SDL_SetClipboardText_(invite.c_str());
            toast(st.room_locked && st.room_pw.empty() ? "Code copied (only the owner knows the password)" : "Invite copied");
        }
        if (button(bx + S(76) + gap, ty + S(2), S(70), bh, "Leave", B_DANGER)) send("m lobby leave");
    }

    int rw = S(330), lw = w - rw - S(18), lx = x, rx = x + lw + S(18);
    // left: the video
    int pw = lw, ph = pw * 9 / 16;
    rect(lx, y, pw, ph, C_BLACK);
    if (!st.url.empty()) {
        if (g_preview_vtex < 0) {
            g_preview_vtex = g_surf->CreateNewTextureID(false);
            g_surf->DrawSetTextureFile(g_preview_vtex, "cstv/screen", true, false);
        }
        g_surf->DrawSetColor(255, 255, 255, 255);
        g_surf->DrawSetTexture(g_preview_vtex);
        g_surf->DrawTexturedRect(lx, y, lx + pw, y + ph);
        g_preview_at = plat_now();
    } else {
        std::wstring t = widen("Nothing playing");
        wtext(F_BODY, lx + (pw - wtext_w(F_BODY, t)) / 2, vcenter(F_BODY, y, ph), C_TEXT_OFF, t);
    }
    outline(lx, y, pw, ph, C_LINE);
    int yy = y + ph + S(10);
    text(F_BOLD, lx, yy, C_TEXT, st.url.empty() ? "Nothing playing" : st.title.empty() ? st.url : st.title, lw);
    yy += g_font_tall[F_BOLD] + S(3);
    std::string line;
    if (st.url.empty()) line = "";
    else if (is_live()) line = "LIVE";
    else if (!st.have_state) line = "Loading\xE2\x80\xA6";
    else if (!st.running) line = "Starting\xE2\x80\xA6";
    else line = fmt_time(play_pos_ms() / 1000) + (st.paused ? "  Paused" : "");
    text(F_SMALL, lx, yy, is_live() ? C_BAD : C_SUB, line, lw);
    yy += g_font_tall[F_SMALL] + S(12);
    // the website's controls: -10 s, pause, +10 s, prev, skip / loop, speed, quality, volume
    bool have = !st.url.empty(), clock = have && !is_live();
    int cbw = (lw - 4 * gap) / 5;
    if (button(lx, yy, cbw, bh, "-10s", B_NORMAL, clock)) send("m seek -10");
    if (button(lx + (cbw + gap), yy, cbw, bh, st.paused ? "Resume" : "Pause", B_NORMAL, clock)) send("m pause");
    if (button(lx + 2 * (cbw + gap), yy, cbw, bh, "+10s", B_NORMAL, clock)) send("m seek +10");
    if (button(lx + 3 * (cbw + gap), yy, cbw, bh, "Prev")) send("m prev");
    if (button(lx + 4 * (cbw + gap), yy, lw - 4 * (cbw + gap), bh, "Skip")) send("m next");
    yy += bh + gap;
    int loopw = S(64), spw = S(92), qw = S(96);
    if (button(lx, yy, loopw, bh, "Loop", st.loop ? B_ACTIVE : B_NORMAL, clock)) {
        st.loop = !st.loop;
        send("m loop %d", st.loop ? 1 : 0);
    }
    static const char *const speeds[] = {"0.25x", "0.5x", "0.75x", "Normal", "1.25x", "1.5x", "1.75x", "2x"};
    static const int pct[] = {25, 50, 75, 100, 125, 150, 175, 200};
    int sp = 3;
    for (int i = 0; i < 8; i++)
        if (st.speed == pct[i]) sp = i;
    int hit = dropdown(1, lx + loopw + gap, yy, spw, bh, speeds, 8, sp, clock);
    if (hit >= 0) send("m speed %d", pct[hit]);
    static const char *const qn[] = {"Auto", "Source", "1440p", "1080p", "720p", "480p", "360p"};
    static const char *const qv[] = {"auto", "source", "1440", "1080", "720", "480", "360"};
    int qa = 0;
    for (int i = 0; i < 7; i++)
        if (st.quality == qv[i]) qa = i;
    hit = dropdown(2, lx + loopw + spw + 2 * gap, yy, qw, bh, qn, 7, qa);
    if (hit >= 0) send("pref quality %s", qv[hit]);
    int vx = lx + loopw + spw + qw + 3 * gap + S(8);
    int vlw = text(F_SMALL, vx, vcenter(F_SMALL, yy, bh), C_SUB, "Vol");
    int sx = vx + vlw + S(10), sw = lx + lw - sx - S(6);
    if (sw > S(40)) {
        if (slider(20, sx, yy, sw, bh, st.vol, 0, 100)) send("pref vol %d", st.vol);
        else if (g_drag == 20) cstv_volume_preview(st.vol);   // heard while sliding; saved when let go
    }
    yy += bh + S(10);
    int lbw = S(80);
    bool go = textbox(13, g_side_link, 2000, lx, yy, lw - 2 * (lbw + gap), bh, "Link");
    if ((button(lx + lw - 2 * lbw - gap, yy, lbw, bh, "Play now", B_ACTIVE, !g_side_link.empty()) || go) && !g_side_link.empty()) {
        play(g_side_link, false);
        g_side_link.clear();
    }
    if (button(lx + lw - lbw, yy, lbw, bh, "Queue", B_NORMAL, !g_side_link.empty())) {
        play(g_side_link, true);
        g_side_link.clear();
    }
    yy += bh + S(12);
    if (!st.queue.empty() && yy + g_font_tall[F_SMALL] < end)
        text(F_SMALL, lx, yy, C_SUB, "Up next  " + (st.queue[0].title.empty() ? st.queue[0].url : st.queue[0].title), lw);

    // right: Chat / Queue / History / People / Settings (a server room: Queue / History / People)
    int ids[6], nt = 0;
    if (lobby) ids[nt++] = LT_CHAT;
    if (st.chat_avail) ids[nt++] = LT_LIVE;   // the stream's own chat (Twitch, YouTube live)
    ids[nt++] = LT_QUEUE;
    ids[nt++] = LT_HISTORY;
    ids[nt++] = LT_PEOPLE;
    if (lobby && st.owner) ids[nt++] = LT_SETTINGS;
    static const char *const names[] = {"Chat", "Queue", "History", "People", "Settings", "Live"};
    bool valid = false;
    for (int i = 0; i < nt; i++) valid = valid || ids[i] == g_ltab;
    if (!valid) g_ltab = ids[0];
    int tbh = S(28), tw = rw / nt;
    for (int i = 0; i < nt; i++) {
        int tx = rx + i * tw, w2 = i == nt - 1 ? rw - i * tw : tw;
        bool on = g_ltab == ids[i], hv = hover(tx, y, w2, tbh);
        rect(tx, y, w2, tbh, on ? C_ACCENT : hv ? C_TAB_HOVER : C_SIDE);
        std::string label = names[ids[i]];
        if (ids[i] == LT_QUEUE && !st.queue.empty()) label += " " + std::to_string(st.queue.size());
        int f = on ? F_BOLD : F_SMALL;
        std::wstring t = fit(f, widen(label), w2 - S(6));
        wtext(f, tx + (w2 - wtext_w(f, t)) / 2, vcenter(f, y, tbh), on ? C_BLACK : C_SUB, t);
        if (take_click(tx, y, w2, tbh) && g_ltab != ids[i]) {
            g_ltab = ids[i];
            g_scroll_side = 0;
        }
    }
    outline(rx, y, rw, tbh, C_LINE);
    int sy = y + tbh + S(10), shh = end - sy;
    switch (g_ltab) {
    case LT_CHAT: side_chat(rx, sy, rw, shh); break;
    case LT_QUEUE: side_queue(rx, sy, rw, shh); break;
    case LT_HISTORY: side_history(rx, sy, rw, shh); break;
    case LT_PEOPLE: side_people(rx, sy, rw, shh); break;
    case LT_SETTINGS: side_settings(rx, sy, rw, shh); break;
    case LT_LIVE: side_live(rx, sy, rw, shh); break;
    }
}

static void tab_lobby(int x, int y, int w, int h)
{
    int end = y + h;
    int bh = S(30), gap = S(10);
    bool lobby = st.room_kind == "lobby";
    bool room = st.master && (lobby || !(st.room_name.empty() || st.room_name == "-"));
    if (!g_join_target.empty() && lobby && st.room_name == g_join_target) {   // in: show it
        g_join_target.clear();
        g_pw_code.clear();
        g_lobby_view = 0;
    }
    // views: the room (this lobby / this server), every lobby, join or create
    const char *labels[3];
    int ids[3], n = 0;
    if (room) { labels[n] = lobby ? "This lobby" : "This server"; ids[n++] = 0; }
    labels[n] = "All lobbies"; ids[n++] = 1;
    labels[n] = "Join or create"; ids[n++] = 2;
    if (g_lobby_view < 0 || (g_lobby_view == 0 && !room)) g_lobby_view = room ? 0 : 1;
    int active = 0;
    for (int i = 0; i < n; i++)
        if (ids[i] == g_lobby_view) active = i;
    int vw = S(130);
    int pick = choice(x, y, labels, n, active, vw);
    if (pick >= 0 && ids[pick] != g_lobby_view) {
        g_lobby_view = ids[pick];
        if (g_lobby_view == 1) ask_lobbies(st.lobby_page);
    }
    int top_x = x + n * (vw + S(6));
    y += S(30) + S(14);
    if (g_lobby_view == 0) {
        room_page(x, y, w, end - y, top_x);
        return;
    }
    if (g_lobby_view == 1) {
        lobby_browser(x, y, w, end - y);
        return;
    }
    // join or create: sign in, make one, join one by its code
    if (st.master) {   // cstv.xyz account: lobbies are made and controlled signed in
        if (!st.you_sid.empty()) {
            int nw = text(F_BODY, x, vcenter(F_BODY, y, bh), C_SUB, "Signed in as ");
            nw += text(F_BOLD, x + nw, vcenter(F_BOLD, y, bh), C_TEXT, st.you_name, w / 2);
            if (button(x + nw + S(16), y, S(100), bh, "Sign out")) send("m logout");
        } else {
            section(x, y, "SIGN IN");
            sign_in_row(x, y, S(360), bh);
        }
        y += bh + S(20);
    } else {
        text(F_TITLE, x, y, C_TEXT, "Not connected to cstv.xyz", w);
        y += g_font_tall[F_TITLE] + S(20);
    }

    if (st.master && !st.you_sid.empty()) {
        section(x, y, "CREATE A LOBBY");
        int nw = S(280), pw = S(220);
        std::string ph = my_name() + "'s lobby";
        bool go = textbox(4, g_new_name, 40, x, y, nw, bh, ph.c_str());
        go = textbox(5, g_new_pw, 64, x + nw + gap, y, pw, bh, "Password (optional)", true) || go;
        if (button(x + nw + pw + 2 * gap, y, S(150), bh, "Create lobby", B_ACTIVE) || go) {
            if (g_new_pw.empty()) send("m lobby create %s", b64_or_dash(g_new_name).c_str());
            else send("m lobby create %s %s", b64_or_dash(g_new_name).c_str(), b64enc(g_new_pw).c_str());
            g_new_name.clear();
            g_new_pw.clear();
            g_lobby_view = 0;
        }
        y += bh + S(22);
    }

    section(x, y, "JOIN A LOBBY");
    bool go = textbox(3, g_code, 6, x, y, S(180), bh, "Code");
    if ((button(x + S(186), y, S(90), bh, "Join", B_NORMAL, g_code.size() == 6) || go) && g_code.size() == 6) {
        join_lobby(g_code, "");
        g_join_target = g_code;
        g_code.clear();
    }
    y += bh + S(12);
    if (st.join_need_pw) {
        std::string ph = "Password for " + st.join_code;
        go = textbox(6, g_join_pw, 64, x, y, S(280), bh, ph.c_str(), true);
        if ((button(x + S(280) + S(6), y, S(90), bh, "Join", B_ACTIVE, !g_join_pw.empty()) || go) && !g_join_pw.empty()) {
            join_lobby(st.join_code, g_join_pw);
            g_join_target = st.join_code;
            g_join_pw.clear();
        }
        y += bh + S(12);
    }
    if (!st.notice.empty()) text(F_BODY, x, y, C_BAD, st.notice, w);
}

static std::string size_name(float s)
{
    return s < 0.2f ? "tiny" : s < 0.375f ? "small" : s < 0.625f ? "medium" : s < 0.875f ? "large" : "huge";
}

static void tab_screen(int x, int y, int w, int h)
{
    int bh = S(40), bw = S(220), gap = S(10);
    bool in_game = g_engine && g_engine->IsInGame();
    section(x, y, "SHARED SCREEN");
    std::string status = "No shared screen on this map.";
    if (st.screen) status = "A " + size_name(st.screen_scale) + (st.screen_float ? " floating" : "") + " shared screen is up on this map.";
    text(F_BOLD, x, y, C_TEXT, status, w);
    y += g_font_tall[F_BOLD] + S(4);
    text(F_SMALL, x, y, C_SUB, std::string("Only ") + gui_shared_audience() + " can see it. Placing it again moves it.", w);
    y += g_font_tall[F_SMALL] + S(10);
    if (button(x, y, bw, bh, "Place shared screen", B_ACTIVE, in_game)) {
        cstv_place_begin(false);
        return;
    }
    if (button(x + bw + gap, y, S(110), bh, "Remove", B_DANGER, st.screen)) send("m remove");
    y += bh + S(22);

    section(x, y, "PERSONAL SCREEN");
    float ps = 0;
    bool pf = false, have = cstv_personal_screen(&ps, &pf);
    status = "No personal screen on this map.";
    if (have) status = "A " + size_name(ps) + (pf ? " floating" : "") + " personal screen is up on this map.";
    text(F_BOLD, x, y, C_TEXT, status, w);
    y += g_font_tall[F_BOLD] + S(4);
    text(F_SMALL, x, y, C_SUB, "Only you can see it. It's kept for this server and map and is back when you return.", w);
    y += g_font_tall[F_SMALL] + S(10);
    if (button(x, y, bw, bh, "Place personal screen", B_ACTIVE, in_game)) {
        cstv_place_begin(true);
        return;
    }
    if (button(x + bw + gap, y, S(110), bh, "Remove", B_DANGER, have)) cstv_remove_personal();
    y += bh + S(22);

    section(x, y, "HUD SCREEN");
    text(F_SMALL, x, y, C_SUB, "The video on your HUD, wherever you go, at full volume. Only you can see it.", w);
    y += g_font_tall[F_SMALL] + S(10);
    static const char *const onoff[] = {"On", "Off"};
    int hit = choice(x, y, onoff, 2, st.hud ? 0 : 1, S(84));
    if (hit >= 0 && (hit == 0) != st.hud) gui_hud_toggle();
    if (button(x + 2 * (S(84) + S(8)) + S(16), y, bw, S(38), "Edit HUD screen")) {
        g_hud_edit = true;
        g_drag = 0;
    }
    y += S(38) + S(22);

    // one slider per column: label, track, percent
    section(x, y, "OPACITY");
    struct { int id; const char *label, *pref; int *val; } op[] = {
        {2, "Placed", "opacity_screen", &st.opacity_screen}, {3, "HUD", "opacity_hud", &st.opacity_hud},
    };
    int col = (w - S(32)) / 2, lw = S(76), pw = S(56);
    for (int i = 0; i < 2; i++) {
        int ox = x + i * (col + S(32)), sw = col - lw - pw - S(8);
        text(F_BODY, ox, vcenter(F_BODY, y, S(30)), C_TEXT, op[i].label, lw - S(8));
        if (slider(op[i].id, ox + lw, y, sw, S(30), *op[i].val, 10, 100)) send("pref %s %d", op[i].pref, *op[i].val);
        char pct[16];
        snprintf(pct, sizeof pct, "%d%%", *op[i].val);
        text(F_BODY, ox + lw + sw + S(16), vcenter(F_BODY, y, S(30)), C_TEXT, pct);
    }
    y += S(30) + S(22);

    static const char *const help[] = {
        "Placing: it floats at the set distance, or sits on a surface nearer than that.",
        "Mouse wheel: size. Left / right click: further / closer. F: always float.",
        "Tab: shared / personal. E or Enter: place. Esc: cancel. Red outline: doesn't fit.",
    };
    for (const char *line : help) {
        text(F_SMALL, x, y, C_SUB, line, w);
        y += g_font_tall[F_SMALL] + S(4);
    }
}

static void tab_stream(int x, int y, int w, int h)
{
    section(x, y, "STREAM YOUR GAME");
    text(F_BODY, x, y, C_SUB, "Shows this game window to everyone in the room, like a Twitch channel.", w);
    y += g_font_tall[F_BODY] + S(4);
    text(F_BODY, x, y, C_SUB, "Only the game's own picture and sound go out (not Discord, music, or the CS:TV video).", w);
    y += g_font_tall[F_BODY] + S(24);

    const std::string &s = st.stream;
    Col c = s == "live" ? C_GOOD : s == "error" ? C_BAD : C_TEXT;
    std::string line = s == "live" ? "Live" : s == "starting" ? "Starting\xE2\x80\xA6" : s == "error" ? "Error" : "Not streaming";
    if (!st.stream_text.empty()) line += "  \xC2\xB7  " + st.stream_text;
    text(F_TITLE, x, y, c, line, w);
    y += g_font_tall[F_TITLE] + S(20);

    bool running = s == "live" || s == "starting";
    bool may = st.master && st.room_kind == "lobby" && st.owner;   // the hub only lets a lobby's owner and admins stream
    if (button(x, y, S(170), S(44), "Start streaming", B_ACTIVE, !running && may)) send("stream start");
    if (button(x + S(180), y, S(170), S(44), "Stop", B_DANGER, running)) send("stream stop");
    if (!may && !running) text(F_SMALL, x, y + S(44) + S(12), C_SUB, "Only a lobby's owner and admins can stream.", w);
}

static void tab_settings(int x, int y, int w, int h)
{
    int bh = S(28), lw = S(110);   // label column, as the website's settings rows
    section(x, y, "PLAYBACK");
    text(F_BODY, x, vcenter(F_BODY, y, bh), C_SUB, "Volume");
    int sw = std::min(w - lw - S(70), S(360));
    if (slider(1, x + lw + S(6), y, sw, bh, st.vol, 0, 100)) send("pref vol %d", st.vol);
    else if (g_drag == 1) cstv_volume_preview(st.vol);   // heard while sliding; saved when let go
    char v[16];
    snprintf(v, sizeof v, "%d%%", st.vol);
    text(F_BODY, x + lw + sw + S(20), vcenter(F_BODY, y, bh), C_TEXT, v);
    y += bh + S(8);
    // universal audio: no fading with distance from the screens, the sound the HUD screen gets
    int ulw = std::max(lw, text_w(F_BODY, "Universal audio") + S(14));   // (wider than the other labels)
    text(F_BODY, x, vcenter(F_BODY, y, bh), C_SUB, "Universal audio");
    static const char *const onoff[] = {"On", "Off"};
    int uhit = choice(x + ulw, y, onoff, 2, st.universal ? 0 : 1, S(84), true, bh);
    if (uhit >= 0 && (uhit == 0) != st.universal) send("pref universal %d", uhit == 0 ? 1 : 0);
    int hx = x + ulw + 2 * S(84) + S(6) + S(16);
    text(F_SMALL, hx, vcenter(F_SMALL, y, bh), C_SUB, "hear the video everywhere, like with the HUD screen", x + w - hx);
    y += bh + S(8);
    // Auto = up to 1440p (VP9 / AV1, scaled to the screen's 1080p: sharper than YouTube's 1080p)
    text(F_BODY, x, vcenter(F_BODY, y, bh), C_SUB, "Quality");
    static const char *const qn[] = {"Auto", "Source", "1440p", "1080p", "720p", "480p", "360p"};
    static const char *const qv[] = {"auto", "source", "1440", "1080", "720", "480", "360"};
    int active = 0;
    for (int i = 0; i < 7; i++)
        if (st.quality == qv[i]) active = i;
    int hit = dropdown(4, x + lw, y, S(140), bh, qn, 7, active);
    if (hit >= 0) send("pref quality %s", qv[hit]);
    y += bh + S(24);

    section(x, y, "SHOW");
    struct { const char *label, *pref; bool *val; } toggles[] = {
        {"Subtitles", "subs", &st.subs}, {"Stream chat beside the screen", "chat", &st.chat}, {"Up next notices", "upnext", &st.upnext},
    };
    int rh = std::max(S(14), g_font_tall[F_BODY]) + S(12);
    for (auto &t : toggles) {
        if (checkbox(x, y, t.label, *t.val)) send("pref %s %d", t.pref, *t.val ? 0 : 1);
        y += rh;
    }
    y += S(12);

    section(x, y, "LOBBY CHAT");
    text(F_BODY, x, vcenter(F_BODY, y, bh), C_SUB, "Text size");
    static const char *const tsn[] = {"Smaller", "Normal", "Large", "Larger", "Largest"};
    static const char *const tsv[] = {"smaller", "normal", "large", "larger", "largest"};
    int ts = 1;
    for (int i = 0; i < 5; i++)
        if (st.chat_text == tsv[i]) ts = i;
    hit = dropdown(6, x + lw, y, S(140), bh, tsn, 5, ts);
    if (hit >= 0) {
        st.chat_text = tsv[hit];   // at once (cstvd echoes it back)
        send("pref chat_text %s", tsv[hit]);
    }
    y += bh + S(24);

#ifdef _WIN32
    text(F_SMALL, x, y, C_TEXT_OFF, "Yours only, saved in %APPDATA%\\cstv\\prefs.", w);
#else
    text(F_SMALL, x, y, C_TEXT_OFF, "Yours only, saved in ~/.config/cstv/prefs.", w);
#endif
}

// ---------------------------------------------------------------- HUD screen
static int g_hud_tex = -1, g_hud_tex_see = -1;   // cstv/screen; cstv/preview (vertex alpha) below 100 %
static int g_hud_chat = -1, g_hud_chat_see = -1; // cstv/chat; cstv/chat_see: the stream chat beside it

static void hud_rect(int sw, int sh, int &x, int &y, int &w, int &h)
{
    w = (int)(std::min(std::max(st.hud_w, 0.08f), 0.9f) * sw);
    h = w * 9 / 16;
    x = std::min(std::max((int)(st.hud_x * sw), 0), sw - w);
    y = std::min(std::max((int)(st.hud_y * sh), 0), std::max(sh - h, 0));
}

static void draw_hud_screen(int sw, int sh)
{
    if (!gui_hud_on()) return;
    if (g_hud_tex < 0) {
        g_hud_tex = g_surf->CreateNewTextureID(false);
        g_surf->DrawSetTextureFile(g_hud_tex, "cstv/screen", true, false);
    }
    int a = st.opacity_hud * 255 / 100;
    if (a < 255 && g_hud_tex_see < 0) {
        g_hud_tex_see = g_surf->CreateNewTextureID(false);
        g_surf->DrawSetTextureFile(g_hud_tex_see, "cstv/preview", true, false);
    }
    int x, y, w, h;
    hud_rect(sw, sh, x, y, w, h);
    // the black backing would show through a see-through screen: just its edge then
    if (a < 255) outline(x - 1, y - 1, w + 2, h + 2, {0, 0, 0, 200 * a / 255});
    else rect(x - 1, y - 1, w + 2, h + 2, {0, 0, 0, 200});
    g_surf->DrawSetColor(255, 255, 255, a);
    g_surf->DrawSetTexture(a < 255 ? g_hud_tex_see : g_hud_tex);
    g_surf->DrawTexturedRect(x, y, x + w, y + h);
    // the stream's chat (Twitch, YouTube live) beside it, like beside a placed screen: on the side with room
    if (st.chat_state == 1) {
        if (g_hud_chat < 0) {
            g_hud_chat = g_surf->CreateNewTextureID(false);
            g_surf->DrawSetTextureFile(g_hud_chat, "cstv/chat", true, false);
        }
        if (a < 255 && g_hud_chat_see < 0) {
            g_hud_chat_see = g_surf->CreateNewTextureID(false);
            g_surf->DrawSetTextureFile(g_hud_chat_see, "cstv/chat_see", true, false);
        }
        int cw = h * 512 / 768, cx = x + w + 2 <= sw - cw ? x + w + 2 : x - 2 - cw;
        if (cx >= 0) {
            if (a < 255) outline(cx - 1, y - 1, cw + 2, h + 2, {0, 0, 0, 200 * a / 255});
            else rect(cx - 1, y - 1, cw + 2, h + 2, {0, 0, 0, 200});
            g_surf->DrawSetColor(255, 255, 255, a);
            g_surf->DrawSetTexture(a < 255 ? g_hud_chat_see : g_hud_chat);
            g_surf->DrawTexturedRect(cx, y, cx + cw, y + h);
        }
    }
}

enum { DRAG_HUD_MOVE = 10, DRAG_HUD_SIZE };
static int g_grab_dx, g_grab_dy;
static bool g_size_left, g_size_top;   // the corner the handle is on (kept for the whole resize)
static int g_size_ax, g_size_ay;       // while resizing: the corner across from the handle, which stays put

// The window's place, while editing: the HUD screen with a corner handle, and a short bar of help.
// The handle is on the corner facing the middle of the screen (screen top right -> handle bottom left, and
// so on), so it never sits against the edge and there is always room to make the screen bigger.
static void draw_hud_edit(int sw, int sh)
{
    int x, y, w, h;
    hud_rect(sw, sh, x, y, w, h);
    int hs = S(24);
    bool left = g_drag == DRAG_HUD_SIZE ? g_size_left : 2 * x + w > sw;
    bool top = g_drag == DRAG_HUD_SIZE ? g_size_top : 2 * y + h > sh;
    int hx = left ? x : x + w - hs, hy = top ? y : y + h - hs;
    if (take_click(hx, hy, hs, hs)) {
        g_drag = DRAG_HUD_SIZE;
        g_size_left = left;
        g_size_top = top;
        g_size_ax = left ? x + w : x;
        g_size_ay = top ? y + h : y;
        g_grab_dx = (left ? x : x + w) - g_mx;   // where in the handle it was grabbed: no jump on the first move
        g_grab_dy = (top ? y : y + h) - g_my;
    } else if (take_click(x, y, w, h)) {
        g_drag = DRAG_HUD_MOVE;
        g_grab_dx = g_mx - x;
        g_grab_dy = g_my - y;
    }
    if (g_drag == DRAG_HUD_MOVE) {
        st.hud_x = std::min(std::max(g_mx - g_grab_dx, 0), sw - w) / (float)sw;
        st.hud_y = std::min(std::max(g_my - g_grab_dy, 0), std::max(sh - h, 0)) / (float)sh;
    } else if (g_drag == DRAG_HUD_SIZE) {
        int cx = g_mx + g_grab_dx, cy = g_my + g_grab_dy;   // where the dragged corner is wanted
        int dw = left ? g_size_ax - cx : cx - g_size_ax, dh = top ? g_size_ay - cy : cy - g_size_ay;
        int roomw = left ? g_size_ax : sw - g_size_ax, roomh = top ? g_size_ay : sh - g_size_ay;
        int nw = std::min(std::max(dw, dh * 16 / 9), std::min(roomw, roomh * 16 / 9));
        nw = std::min(std::max(nw, (int)(0.08f * sw)), (int)(0.9f * sw));
        int nh = nw * 9 / 16;
        // + 0.5: hud_rect truncates, and the fixed corner must not creep by a pixel
        st.hud_w = (nw + 0.5f) / sw;
        st.hud_x = ((left ? g_size_ax - nw : g_size_ax) + 0.5f) / sw;
        st.hud_y = ((top ? g_size_ay - nh : g_size_ay) + 0.5f) / sh;
    }
    if (g_drag && !in.down) {
        g_drag = 0;
        save_hud();
    }
    hud_rect(sw, sh, x, y, w, h);
    if (g_drag != DRAG_HUD_SIZE) {   // after a move the screen may be in another quarter
        left = 2 * x + w > sw;
        top = 2 * y + h > sh;
    }
    for (int i = 0; i < std::max(1, S(2)); i++) outline(x - i, y - i, w + 2 * i, h + 2 * i, C_ACCENT);
    rect(left ? x : x + w - hs, top ? y : y + h - hs, hs, hs, C_ACCENT);

    int bw = std::min(sw - S(40), S(760)), bh = S(56);
    int bx = (sw - bw) / 2, by = sh - bh - S(140);
    rect(bx, by, bw, bh, C_BG);
    outline(bx, by, bw, bh, C_LINE);
    text(F_BODY, bx + S(16), vcenter(F_BODY, by, bh), C_TEXT, "Drag the HUD screen to move it, its corner to resize it.", bw - S(150));
    if (button(bx + bw - S(120), by + (bh - S(38)) / 2, S(104), S(38), "Done", B_ACTIVE) || in.enter) end_hud_edit();
    in.enter = false;
}

// ---------------------------------------------------------------- place-mode hint bar
static void draw_place_hint(int sw, int sh)
{
    PlaceHint p;
    if (!cstv_place_hint(&p)) return;
    std::string head = p.personal ? "Placing PERSONAL screen \xE2\x80\x94 only you see it"
                                  : std::string("Placing SHARED screen \xE2\x80\x94 visible to ") + gui_shared_audience();
    char keys[300], size[48];
    if (p.scale < p.want_scale - 0.005f) snprintf(size, sizeof size, "%.2f (%.2f fits here)", p.want_scale, p.scale);
    else snprintf(size, sizeof size, "%.2f", p.scale);
    snprintf(keys, sizeof keys, "Wheel: size %s  \xC2\xB7  F: always float (%s)  \xC2\xB7  Left / right click: distance %d  \xC2\xB7  "
             "Tab: %s  \xC2\xB7  E / Enter: place  \xC2\xB7  Esc: cancel",
             size, p.want_float ? "on" : "off", (int)p.dist, p.personal ? "shared" : "personal");
    int bw = std::min(sw - S(40), S(1180)), bh = S(16) + g_font_tall[F_BOLD] + S(6) + g_font_tall[F_SMALL] + S(14);
    int bx = (sw - bw) / 2, by = sh - bh - S(140);
    rect(bx, by, bw, bh, {20, 22, 27, 225});
    rect(bx, by, S(5), bh, p.personal ? Col{170, 110, 250, 255} : C_ACCENT);
    int tx = bx + S(20), tw = bw - S(36);
    int hw = text(F_BOLD, tx, by + S(14), C_TEXT, head, tw);
    if (!p.ok) text(F_BOLD, tx + hw + S(16), by + S(14), C_BAD, "doesn't fit here", tw - hw - S(16));
    text(F_SMALL, tx, by + S(14) + g_font_tall[F_BOLD] + S(6), C_SUB, keys, tw);
}

// An admin's warning (cstv.xyz): the message and one button; Esc and the toggle key don't close it.
static void draw_warning(int x, int y, int w, int h)
{
    int pad = S(40), bw = S(200), bh = S(44);
    int ty = y + S(40);
    rect(x + pad, ty, text_w(F_BOLD, "WARNING") + S(20), g_font_tall[F_BOLD] + S(8), C_BAD);
    text(F_BOLD, x + pad + S(10), ty + S(4), C_WHITE, "WARNING");
    ty += g_font_tall[F_BOLD] + S(24);
    // wrapped at spaces, worked out once per message and width
    static std::string wrapped_for;
    static int wrapped_w;
    static std::vector<std::string> lines;
    int maxw = w - 2 * pad;
    if (wrapped_for != st.warning || wrapped_w != maxw) {
        wrapped_for = st.warning;
        wrapped_w = maxw;
        lines.clear();
        std::string msg = st.warning;
        while (!msg.empty() && lines.size() < 12) {
            size_t cut = msg.size();
            while (cut > 1 && text_w(F_TITLE, msg.substr(0, cut)) > maxw) {
                size_t sp = msg.rfind(' ', cut - 1);
                cut = sp == std::string::npos || sp == 0 ? cut - 1 : sp;
            }
            lines.push_back(msg.substr(0, cut));
            msg.erase(0, cut);
            while (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
        }
    }
    for (const std::string &l : lines) {
        text(F_TITLE, x + pad, ty, C_TEXT, l);
        ty += g_font_tall[F_TITLE] + S(8);
    }
    if (button(x + pad, y + h - pad - bh, bw, bh, "I understand", B_ACTIVE)) {
        st.warning.clear();
        close_window();
    }
}

#include "cstv_prog.inc"

static void draw(int sw, int sh)
{
    if (g_vx < 0) { g_vx = sw / 2.0f; g_vy = sh / 2.0f; }
    g_vx = std::min(std::max(g_vx, 0.0f), sw - 1.0f);
    g_vy = std::min(std::max(g_vy, 0.0f), sh - 1.0f);
    g_mx = (int)g_vx;
    g_my = (int)g_vy;
    g_click = in.click;
    in.click = false;
    g_wheel = in.wheel;
    in.wheel = 0;

    if (g_mode == MODE_PROG) {
        prog_window(sw, sh);
        if (g_click) g_focus = 0;
        in.typed.clear();
        in.backspaces = in.tabs = in.nav_x = in.nav_y = in.flip = 0;
        in.paste = in.enter = in.del = false;
        return;
    }

    if (g_hud_edit) {
        draw_hud_edit(sw, sh);
        in.typed.clear();
        in.backspaces = in.tabs = 0;
        in.paste = in.enter = false;
        return;
    }

    int W = std::min(S(1180), sw - S(40)), H = std::min(S(720), sh - S(40));
    int x0 = (sw - W) / 2, y0 = (sh - H) / 2;
    rect(0, 0, sw, sh, C_DIM);
    int bar = S(46);
    rect(x0, y0, W, H, C_BG);
    outline(x0, y0, W, H, C_BORDER);
    rect(x0, y0, W, bar, C_BAR);
    rect(x0, y0 + bar - 1, W, 1, C_LINE);
    text(F_TITLE, x0 + S(18), vcenter(F_TITLE, y0, bar), C_TEXT, "CS:TV");
    dropdown_input();
    if (!st.warning.empty()) {   // an admin's warning: nothing else until it's acknowledged
        draw_warning(x0, y0 + bar, W, H - bar);
        return;
    }
    if (button(x0 + W - S(40), y0 + S(8), S(30), S(30), "\xC3\x97")) {
        close_window();
        return;
    }
    std::string ms = !st.master ? "cstv.xyz offline" : st.you_sid.empty() ? "cstv.xyz  \xC2\xB7  not signed in" : "cstv.xyz  \xC2\xB7  " + st.you_name;
    int mw = text_w(F_SMALL, ms);
    int mx = x0 + W - S(54) - mw;
    text(F_SMALL, mx, vcenter(F_SMALL, y0, bar), C_SUB, ms);
    int dot = S(8);
    rect(mx - dot - S(8), y0 + (bar - dot) / 2, dot, dot, st.master ? C_GOOD : C_BAD);

    static const char *const tabs[T_COUNT] = {"Lobby", "Screen", "Stream", "Settings"};
    int side = S(150), th = S(40);
    rect(x0 + 1, y0 + bar, side, H - bar - 1, C_SIDE);
    rect(x0 + side, y0 + bar, 1, H - bar - 1, C_LINE);
    for (int i = 0; i < T_COUNT; i++) {
        int ty = y0 + bar + S(10) + i * th;
        bool on = g_tab == i, hv = hover(x0, ty, side, th);
        if (on || hv) rect(x0 + 1, ty, side - 1, th, on ? C_ACCENT : C_TAB_HOVER);
        std::string label = tabs[i];
        if (i == T_STREAM && st.stream == "live") label += "  \xE2\x97\x8F";
        int f = on ? F_BOLD : F_BODY;
        text(f, x0 + S(18), vcenter(f, ty, th), on ? C_BLACK : C_TEXT, label, side - S(26));
        if (take_click(x0, ty, side, th)) {
            g_tab = i;
            g_focus = 0;
        }
    }

    int cx = x0 + side + S(22), cy = y0 + bar + S(18);
    int cw = W - side - S(44), ch = H - bar - S(18) - S(40);
    switch (g_tab) {
    case T_LOBBY: tab_lobby(cx, cy, cw, ch); break;
    case T_SCREEN: tab_screen(cx, cy, cw, ch); break;
    case T_STREAM: tab_stream(cx, cy, cw, ch); break;
    case T_SETTINGS: tab_settings(cx, cy, cw, ch); break;
    }

    dropdown_draw();
    if (!g_toast.empty() && plat_now() - g_toast_at < 3) text(F_BODY, cx, y0 + H - S(30), C_GOOD, g_toast, cw);
    text(F_SMALL, x0 + S(18), y0 + H - S(28), C_TEXT_OFF, "Esc closes", side - S(26));

    if (g_click) g_focus = 0;   // a click on nothing drops the text focus
    // edits for a text box that wasn't drawn this frame are dropped
    in.typed.clear();
    in.backspaces = in.tabs = 0;
    in.paste = in.enter = false;
}

// the window's cursor: a black dot with a thin light edge, so it shows on the dark window
static void draw_cursor()
{
    if (g_mode == MODE_PROG) {   // the Progress Log's: the plain arrow Minecraft has (the system's), black edged
        static const char *const arrow[] = {
            "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X", "X........X",
            "X.........X", "X......XXXX", "X...X..X", "X..XX..X", "X.X  X..X", "XX   X..X", "X     X..X", "      X..X", "       XX"};
        int p = std::max(1, g_font_for_h / 1080);
        for (int r = 0; r < (int)(sizeof arrow / sizeof *arrow); r++)
            for (int c = 0; arrow[r][c];) {
                char k = arrow[r][c];
                int n = 0;
                while (arrow[r][c + n] == k) n++;
                if (k != ' ') rect(g_mx + c * p, g_my + r * p, n * p, p, k == 'X' ? C_BLACK : C_WHITE);
                c += n;
            }
        return;
    }
    int x = g_mx, y = g_my, r = std::max(1, S(1.5f));
    rect(x - r - 1, y - r - 1, 2 * r + 2, 2 * r + 2, C_SUB);
    rect(x - r, y - r, 2 * r, 2 * r, C_BLACK);
}

// ---------------------------------------------------------------- hook + entry points
typedef void (*PaintTraverse_t)(void *, VPANEL, bool, bool);
static PaintTraverse_t g_orig_paint;
static VPANEL g_top;

static bool layout(int &sw, int &sh)
{
    g_surf->GetScreenSize(sw, sh);
    if (sw <= 0 || sh <= 0) return false;
    float s = std::min(sh / 1080.0f, sw / 1060.0f);
    s = (s < 0.6f ? 0.6f : s > 2.5f ? 2.5f : s) * MENU_SCALE;
    s = std::min(s, std::min(sw / 1000.0f, sh / 680.0f));   // the window always fits on the screen
    // A video settings change runs ISurface::ResetFontCaches (slot 136): it empties every font and reloads
    // only the scheme's, so ours draw nothing until set up again. An emptied font is 0 tall (the font
    // manager's amalgam RemoveAll zeroes its max height; checked in vguimatsurface.so 2026-10-24).
    bool fonts_lost = g_font_tall[F_BODY] > 0 && g_surf->GetFontTall(g_font[F_BODY]) <= 0;
    int book_t = g_book_t;
    book_scales(sw, sh);
    if (sh != g_font_for_h || s != g_s || fonts_lost || book_t != g_book_t) {
        g_s = s;
        setup_fonts(sh);
    }
    return true;
}

// IPanel::PaintTraverse runs for every panel, so only the top panel's name is looked at until found.
static void hk_paint(void *self, VPANEL panel, bool force, bool allow)
{
    g_orig_paint(self, panel, force, allow);
#ifdef _WIN32
    plat_input_install(win_event);
#endif
    if (!g_top) {
        const char *name = g_panel->GetName(panel);
        if (name && !strcmp(name, "MatSystemTopPanel")) g_top = panel;
    }
    if (panel != g_top) return;
    int sw = 0, sh = 0;
    bool placing = cstv_place_hint(nullptr);
    bool ours = (g_open || placing || gui_hud_on()) && layout(sw, sh);
    if (ours) draw_hud_screen(sw, sh);
    capture_paint();   // the stream gets the HUD and the HUD screen, but not the window or the place-mode hint
    if (!ours) return;
    if (placing && !g_open) draw_place_hint(sw, sh);
    if (g_open) {
        draw(sw, sh);
        if (g_open) draw_cursor();
    }
    if (g_lchat_showing && !gui_lchat_on()) {   // the Lobby page's chat isn't shown any more: the renderer can rest
        g_lchat_showing = false;
        send("lchat show 0");
    }
    static bool xchat_was;
    bool xchat = gui_xchat_on();
    if (xchat_was && !xchat) send("xchat 0");   // the Live tab closed: the stream chat only if wanted beside the screen
    xchat_was = xchat;
}

bool gui_install()
{
    if (g_installed) return true;
    g_surf = (vgui::ISurface *)plat_interface("vguimatsurface", VGUI_SURFACE_INTERFACE_VERSION);
    g_panel = (vgui::IPanel *)plat_interface("vgui2", VGUI_PANEL_INTERFACE_VERSION);
    g_engine = (IVEngineClient *)plat_interface("engine", VENGINE_CLIENT_INTERFACE_VERSION);
    if (!g_surf || !g_panel || !load_sdl()) return false;
#ifdef _WIN32
    const int paint_traverse = 41;   // IPanel has a virtual destructor: one slot on MSVC, two on GCC
#else
    const int paint_traverse = 42;
#endif
    g_orig_paint = (PaintTraverse_t)plat_swap_slot(g_panel, paint_traverse, (void *)hk_paint);
    if (!g_orig_paint) return false;
#ifndef _WIN32
    SDL_GetEventFilter_(&g_prev_filter, &g_prev_data);
    SDL_SetEventFilter_(gui_filter, nullptr);
#endif
    g_installed = true;
    return true;
}

bool gui_open() { return g_open; }

bool gui_preview_on() { return g_open && plat_now() - g_preview_at < 0.5; }

bool gui_xchat_on() { return g_open && plat_now() - g_xchat_at < 0.5; }

static bool gui_lchat_on() { return g_open && plat_now() - g_lchat_at < 0.5; }

bool gui_in_lobby() { return st.master && st.room_kind == "lobby"; }

int gui_screen_alpha() { return st.opacity_screen * 255 / 100; }

bool gui_hud_on()
{
    return g_installed && ((g_open && g_hud_edit) || (st.hud && g_engine && g_engine->IsInGame()));
}

void gui_hud_toggle()
{
    st.hud = !st.hud;
    send("pref hud %d", st.hud ? 1 : 0);
    cstv_print(st.hud ? "HUD screen on: only you see it. Move or resize it in \x04!cstv\x01 > Screen > Edit HUD screen." : "HUD screen off.");
}

const char *gui_shared_audience()
{
    static std::string s;
    std::string loc;
    for (auto &m : st.roster)
        if (m.id == st.my_id) loc = m.where;
    int n = 0;
    for (auto &m : st.roster)
        if (loc.empty() || m.where == loc) n++;
    if (!st.master) s = "players in your room (not connected to the master right now)";
    else if (st.room_kind == "lobby") s = "lobby " + st.room_name + " (" + std::to_string(n) + (n == 1 ? " player" : " players") + " here)";
    else s = "CS:TV players on this server in your room (" + std::to_string(n) + (n == 1 ? " player" : " players") + " here)";
    return s.c_str();
}

void gui_toggle()
{
    if (!g_installed) {
        cstv_print("The CS:TV window isn't available in this game (couldn't hook its UI). Chat commands still work.");
        return;
    }
    if (g_open && g_mode == MODE_CSTV) {
        if (st.warning.empty()) close_window();
        return;
    }
    if (g_open) close_window();   // the Progress Log was up: CS:TV instead
    g_mode = MODE_CSTV;
    open_window();
}

static void open_window()
{
    if (!g_installed || g_open) return;
    if (cstv_place_hint(nullptr)) cstv_place_event(PLACE_CANCEL, 0);
    in = {};
    g_focus = g_drag = 0;
    g_vx = -1;
    st.notice.clear();
    find_toggle_key();
    if (g_engine && g_engine->Con_IsVisible()) g_engine->ClientCmd_Unrestricted("hideconsole");   // typed `cstv` in it
    if (SDL_StartTextInput_) SDL_StartTextInput_();
#ifdef _WIN32
    plat_input_window(true);
    if (g_engine) g_engine->ClientCmd_Unrestricted("cl_mouseenable 0");   // the view doesn't turn while the window is up
#endif
    g_open = true;
}
