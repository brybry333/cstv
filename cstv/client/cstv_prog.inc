// ---------------------------------------------------------------- Progress Log (!prog): a Minecraft book
// A practice diary drawn as Minecraft's book: page 1 (the log: the day's attempts as boss bars, and the form to
// add one or change one) on its own, then spreads: 2 graph | 3 stats, 4 spots | 5 settings. Its pictures, font
// and sounds are Minecraft's own, taken from the player's install by tools/mcassets.py into cstrike/custom/cstv
// (resource/cstv/progbook.bin, progmc*.ttf, sound/progbook/*.wav); without them it's a plain book. The book's
// pixels are g_book_f screen px (x5 at 1080p), its text's and widgets' g_book_t (x2): finer, or a page would
// hold 14 lines as in the game. Pictures go up once per scale as VGUI textures (DrawSetTextureRGBA, upscaled
// here: VGUI filters them) and are drawn with DrawTexturedSubRect, the graph the same way when it changes.
// Keys: arrows move between the controls (nearest one that way), Enter presses, Tab steps through a row's
// inputs, PgUp / PgDn (or Left / Right with nothing picked) turn pages, Delete deletes the picked attempt or spot
// (asking Yes / No: arrows, Enter; Esc = No), Esc closes.
//
// The server's progresslog plugin keeps the entries (per player and map, one day each) and sends them with
// `prog_ui ...` (server-executable, cstv_client.cpp); what the book changes goes back as `sm_progui ...`.
// Everything is run time (seconds). "End" is a spot like the others but one time: your estimated finish, the
// 100 % that percentages show against. The other spots are a range (where you get to a place, varying).
//
// From the server:  prog_ui hello                          (answer: sm_progui hello 2)
//                   prog_ui reset "<map>" <today> <daystart> <autofinish> <saveat> <bin>
//                   prog_ui e <id> <day> <created> <source> "<from>" <from s> "<to>" <to s>    (-1 = no time)
//                   prog_ui d <id> | s "<spot>" <s> <to s> | sd "<spot>" | cfg <autofinish> <saveat> <daystart> <today> <bin>
//                   prog_ui done | open | msg "<text>" | attempt <0/1> "<from>" <from s> <now s> <last fail s>
// To the server:    sm_progui hello 2 | sync | view 0/1 | add "<from>" "<to>" | edit <id> from|to "<text>"
//                   del <id> | spot "<name>" "<from>" ["<to>"] | spotdel "<name>" | set autofinish|saveat|bin <n>
// Included into cstv_gui.cpp (its fonts, input and primitives).
#include <functional>
#ifndef _WIN32
#include <unistd.h>
#endif

struct PEntry { int id; std::string day; long created; int src; std::string from, to; float ft, tt; };
struct PSpot { std::string name; float lo, hi; };   // hi not past lo: the one time

static struct {
    std::string map, today;
    int daystart = 5, bin = 10;   // bin: seconds of a fail range (stats, graph), the player's setting
    bool autofinish = true, saveat_here = false, synced = false, have = false;
    std::vector<PEntry> e;        // by id
    std::vector<PSpot> spots;     // by where they start; "End" among them
    float end = -1;               // the spot "End": your estimated finish
    int version = 1;
    bool att = false;             // the attempt being played (while the book is open)
    std::string att_from;
    float att_ft = -1, att_now = -1, att_fail = -1;
} pg;

enum { PV_LOG, PV_GRAPH, PV_SPOTS, PV_COUNT };   // what the book is open at: page 1, pages 2 | 3, pages 4 | 5
static int g_pview;
static std::string g_pday;                       // page 1's day ("" = today)
static int g_psel;                               // the attempt being changed (its id), 0 none
static std::string g_pe_from, g_pe_to, g_padd_from, g_padd_to, g_pend, g_pend2;
static std::string g_pspot_name, g_pspot_lo, g_pspot_hi, g_pspot_edit;   // the spot form; the spot it changes ("" = new)
static int g_pscroll, g_pscroll_spots;
static int g_pall;                               // the input whose whole text is picked (Tab, arrows): typing replaces it
static int g_pg_mode;                            // the graph: 0 where attempts end, 1 how far runs from start get
static int g_pg_a = -1, g_pg_b = -1;             // its days: an index into g_pdays, g_pdays.size() = all days, -1 latest / none
static int g_pagg_version;
static std::vector<std::string> g_pdays;         // days with entries, oldest first

// ids: inputs 900-999 (picked whole when arrived at by keys), buttons and links 1000-1999, rows 100000 + entry id
// (page 1) and 200000 + spot index (page 4)
enum { PI_EDIT_FROM = 910, PI_EDIT_TO, PI_ADD_FROM, PI_ADD_TO, PI_END = 920, PI_SPOT_NAME, PI_SPOT_LO, PI_SPOT_HI, PI_END2 };
enum { PB_ADD = 1001, PB_SAVE, PB_DEL, PB_CANCEL, PB_DAY_PREV = 1010, PB_DAY_NEXT, PB_END, PB_MODE0 = 1020, PB_MODE1, PB_A_PREV,
       PB_A_NEXT, PB_B_PREV, PB_B_NEXT, PB_SPOT_ADD = 1030, PB_SPOT_CANCEL, PB_SPOT_DEL, PB_END2 = 1040, PB_AUTO_YES, PB_AUTO_NO,
       PB_SAVEAT_FAIL, PB_SAVEAT_HERE, PB_BIN = 1050, PB_DONE = 1090, PB_BACK, PB_FWD, PB_TAB = 1100, PB_YES = 1201, PB_NO };
enum { PR_ENTRY = 100000, PR_SPOT = 200000 };
static const int P_BINS[] = {5, 10, 15, 20, 30};

// Minecraft's colours on the page: its book text is black; the rest are its chat colours
static const Col M_INK = {0, 0, 0, 255}, M_DIM = {85, 85, 85, 255}, M_FAINT = {160, 150, 130, 255}, M_BLUE = {0, 0, 170, 255},
                 M_GREEN = {0, 140, 0, 255}, M_RED = {170, 0, 0, 255}, M_WHITE = {255, 255, 255, 255}, M_GRAY = {170, 170, 170, 255},
                 M_OFF = {160, 160, 160, 255}, M_YELLOW = {255, 255, 85, 255}, M_PICK = {0, 0, 255, 255}, M_HOVER = {80, 50, 10, 30},
                 M_SEL = {0, 0, 170, 34}, M_BAND = {139, 90, 43, 40}, M_LINE_A = {0, 0, 170, 255}, M_FILL_A = {85, 85, 255, 255},
                 M_LINE_B = {170, 0, 0, 255}, M_FILL_B = {255, 85, 85, 255};

static int BF(int v) { return v * g_book_f; }   // book pixels on the screen
static int BT(int v) { return v * g_book_t; }   // text and widget pixels

static void p_send(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_engine) g_engine->ServerCmd(buf, true);
}

// what can go inside the quotes of a console command
static std::string p_clean(const std::string &s)
{
    std::string o;
    for (char c : s)
        if ((unsigned char)c >= 0x20 && c != '"' && c != ';' && c != '\\') o += c;
    size_t a = o.find_first_not_of(' '), b = o.find_last_not_of(' ');
    o = a == std::string::npos ? "" : o.substr(a, b - a + 1);
    if (o.size() > 31) o.resize(31);
    return o;
}

static std::string p_clock(float s)
{
    char b[24];
    if (s < 0) return "-";
    int t = (int)(s + 0.0001f);
    if (t >= 3600) snprintf(b, sizeof b, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}

static std::string p_span(long secs)   // "1h 12m", "38m"
{
    char b[24];
    if (secs < 60) return secs > 0 ? "<1m" : "-";
    if (secs >= 3600) snprintf(b, sizeof b, "%ldh %02ldm", secs / 3600, secs / 60 % 60);
    else snprintf(b, sizeof b, "%ldm", secs / 60);
    return b;
}

static float p_pctv(float t) { return pg.end > 0 && t >= 0 ? std::min(100.0f, t / pg.end * 100.0f) : -1; }
static std::string p_pct(float t)
{
    float v = p_pctv(t);
    char b[16];
    if (v < 0) return "";
    snprintf(b, sizeof b, "%.0f%%", v);
    return b;
}

// "2026-10-28" -> "wed 10/28"
static std::string p_day_label(const std::string &d, bool weekday = true)
{
    if (d.size() < 10) return d;
    struct tm t = {};
    t.tm_year = atoi(d.substr(0, 4).c_str()) - 1900;
    t.tm_mon = atoi(d.substr(5, 2).c_str()) - 1;
    t.tm_mday = atoi(d.substr(8, 2).c_str());
    t.tm_hour = 12;
    mktime(&t);
    static const char *const wd[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    return (weekday ? std::string(wd[t.tm_wday % 7]) + " " : std::string()) + d.substr(5, 2) + "/" + d.substr(8, 2);
}

static std::string p_hhmm(long ts)
{
    time_t t = (time_t)ts;
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char b[8];
    strftime(b, sizeof b, "%H:%M", &lt);
    return b;
}

static std::string p_lower(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }
// how a point reads: times as they are, names in lower case
static std::string p_point(const std::string &s) { return s.empty() || (s[0] >= '0' && s[0] <= '9') ? s : p_lower(s); }
static std::string p_entry_text(const PEntry &e) { return p_point(e.from) + " \xE2\x80\x93 " + p_point(e.to); }

static bool p_is_end(const PSpot &s) { return !strcasecmp(s.name.c_str(), "End"); }
static float p_spot_hi(const PSpot &s) { return s.hi > s.lo ? s.hi : s.lo; }
static std::string p_spot_range(const PSpot &s)
{
    return s.hi > s.lo ? p_clock(s.lo) + " \xE2\x80\x93 " + p_clock(s.hi) : p_clock(s.lo);
}

// an attempt from Start (the run timer's start, or typed 0:00); one that got to the end (your estimated finish)
static bool p_from_start(const PEntry &e) { return !strcasecmp(e.from.c_str(), "Start") || (e.ft >= 0 && e.ft < 0.5f); }
static bool p_to_end(const PEntry &e) { return !strcasecmp(e.to.c_str(), "End") || (pg.end > 0 && e.tt >= pg.end - 0.01f); }
static bool p_fail(const PEntry &e) { return p_from_start(e) && !p_to_end(e) && e.tt >= 0; }   // practice isn't a fail
static float p_reach(const PEntry &e) { return p_to_end(e) && pg.end > 0 ? pg.end : e.tt; }   // how far it got

static void p_spots_changed()
{
    pg.end = -1;
    for (const PSpot &s : pg.spots)
        if (p_is_end(s)) pg.end = s.lo;
    std::sort(pg.spots.begin(), pg.spots.end(), [](const PSpot &a, const PSpot &b) { return a.lo < b.lo; });
    pg.version++;
}

static void p_aggregate()
{
    if (g_pagg_version == pg.version) return;
    g_pagg_version = pg.version;
    g_pdays.clear();
    for (const PEntry &e : pg.e)
        if (std::find(g_pdays.begin(), g_pdays.end(), e.day) == g_pdays.end()) g_pdays.push_back(e.day);
    std::sort(g_pdays.begin(), g_pdays.end());
}

static const PEntry *p_entry(int id)
{
    for (const PEntry &e : pg.e) if (e.id == id) return &e;
    return nullptr;
}

static void p_pick(int id)
{
    const PEntry *e = p_entry(id);
    g_psel = e ? id : 0;
    if (e) { g_pe_from = e->from; g_pe_to = e->to; }
    g_focus = 0;
}

static std::string p_viewday() { return g_pday.empty() ? pg.today : g_pday; }

// the scale all bars and the graph share: start .. your end (or the furthest anything got when there's no end yet)
static float p_scale()
{
    if (pg.end > 0) return pg.end;
    float m = 30;
    for (const PEntry &e : pg.e) m = std::max(m, std::max(e.ft, e.tt));
    for (const PSpot &s : pg.spots) m = std::max(m, p_spot_hi(s));
    return m * 1.05f;
}

// ---- the place a time is at: the narrowest spot around it (a one-time spot covers a fail range around its
// time), else the fail range it's in ("2:10-2:20", the player's range size)
struct PRegion { std::string name; float lo, hi; bool spot; };
static PRegion p_region(float t)
{
    const PSpot *best = nullptr;
    float blo = 0, bhi = 0;
    for (const PSpot &s : pg.spots) {
        if (p_is_end(s)) continue;
        float lo = s.lo, hi = p_spot_hi(s);
        if (hi <= lo) { lo -= pg.bin * 0.5f; hi += pg.bin * 0.5f; }
        if (t >= lo && t <= hi && (!best || hi - lo < bhi - blo)) { best = &s; blo = lo; bhi = hi; }
    }
    if (best) return {p_lower(best->name), blo, bhi, true};
    float lo = (float)((int)(std::max(t, 0.0f) / pg.bin) * pg.bin);
    return {p_clock(lo) + "\xE2\x80\x93" + p_clock(lo + pg.bin), lo, lo + pg.bin, false};
}

// ---- stats of a set of attempts (page 3; the graph's legend)
struct PStats {
    int tries = 0, starts = 0, fails = 0, days = 0;
    float best = -1, avg = -1;
    long secs = 0;                                  // first to last entry, each day
    std::vector<std::pair<PRegion, int>> where;     // the places runs from Start died at, most first
    PRegion dead; int dead_n = 0, dead_of = 0;      // the highest share of runs that got there and died there
    std::string prac; int prac_n = 0;               // the most practised start (not Start)
};

static PStats p_stats(const std::vector<const PEntry *> &v)
{
    PStats s;
    float sum = 0;
    std::vector<std::pair<std::string, int>> pr;
    std::vector<std::pair<std::string, std::pair<long, long>>> span;
    for (const PEntry *e : v) {
        s.tries++;
        if (p_from_start(*e)) {
            s.starts++;
            if (e->tt >= 0) { sum += p_reach(*e); s.best = std::max(s.best, p_reach(*e)); }
        } else {
            std::string k = p_point(e->from);
            auto it = std::find_if(pr.begin(), pr.end(), [&](const std::pair<std::string, int> &x) { return x.first == k; });
            if (it == pr.end()) pr.push_back({k, 1});
            else it->second++;
        }
        if (p_fail(*e)) {
            s.fails++;
            PRegion r = p_region(e->tt);
            auto it = std::find_if(s.where.begin(), s.where.end(), [&](const std::pair<PRegion, int> &x) { return x.first.name == r.name; });
            if (it == s.where.end()) s.where.push_back({r, 1});
            else it->second++;
        }
        auto d = std::find_if(span.begin(), span.end(), [&](const std::pair<std::string, std::pair<long, long>> &x) { return x.first == e->day; });
        if (d == span.end()) span.push_back({e->day, {e->created, e->created}});
        else { d->second.first = std::min(d->second.first, e->created); d->second.second = std::max(d->second.second, e->created); }
    }
    if (s.starts) s.avg = sum / s.starts;
    s.days = (int)span.size();
    for (auto &d : span) s.secs += d.second.second - d.second.first;
    std::stable_sort(s.where.begin(), s.where.end(), [](const std::pair<PRegion, int> &a, const std::pair<PRegion, int> &b) { return a.second > b.second; });
    for (auto &w : s.where) {
        int got = 0;
        for (const PEntry *e : v)
            if (p_from_start(*e) && e->tt >= 0 && p_reach(*e) >= w.first.lo) got++;
        if (got < 3) continue;   // (a place got to once or twice says nothing)
        if (!s.dead_of || w.second * s.dead_of > s.dead_n * got || (w.second * s.dead_of == s.dead_n * got && w.second > s.dead_n)) {
            s.dead = w.first;
            s.dead_n = w.second;
            s.dead_of = got;
        }
    }
    for (auto &p : pr)
        if (p.second > s.prac_n) { s.prac = p.first; s.prac_n = p.second; }
    return s;
}

static std::vector<const PEntry *> p_day_entries(const std::string &day)   // "" = every day
{
    std::vector<const PEntry *> v;
    for (const PEntry &e : pg.e) if (day.empty() || e.day == day) v.push_back(&e);
    return v;
}

// ---- from the server
void gui_prog_open();
static bool g_prog_server;   // this server runs the Progress Log plugin
static void p_view(int v, bool sound = true);
static void p_focus(int id);
static void p_ask(const std::string &title, const std::string &sub, std::function<void()> yes);
static void p_ask_close(bool yes);

void gui_prog_cmd(int argc, const char *const *argv)
{
    if (argc < 2) return;
    std::string v = argv[1];
    auto F = [&](int i) { return i < argc ? (float)atof(argv[i]) : -1.0f; };
    auto I = [&](int i) { return i < argc ? atoi(argv[i]) : 0; };
    auto A = [&](int i) { return std::string(i < argc ? argv[i] : ""); };
    g_prog_server = true;   // only the plugin sends these
    if (v == "hello") p_send("sm_progui hello 2");
    else if (v == "reset" && argc >= 7) {
        pg.map = A(2); pg.today = A(3); pg.daystart = I(4); pg.autofinish = I(5) != 0; pg.saveat_here = I(6) != 0;
        if (argc >= 8 && I(7) >= 5) pg.bin = I(7);
        pg.e.clear(); pg.spots.clear(); pg.end = -1;
        pg.synced = false; pg.have = true; pg.version++;
        g_psel = 0; g_pday.clear(); g_pg_a = g_pg_b = -1;
    } else if (v == "e" && argc >= 10) {
        PEntry n = {I(2), A(3), atol(argv[4]), I(5), A(6), A(8), F(7), F(9)};
        auto it = std::lower_bound(pg.e.begin(), pg.e.end(), n.id, [](const PEntry &a, int id) { return a.id < id; });
        if (it != pg.e.end() && it->id == n.id) *it = n;
        else pg.e.insert(it, n);
        if (g_psel == n.id) { g_pe_from = n.from; g_pe_to = n.to; }
        pg.version++;
    } else if (v == "d" && argc >= 3) {
        int id = I(2);
        pg.e.erase(std::remove_if(pg.e.begin(), pg.e.end(), [id](const PEntry &e) { return e.id == id; }), pg.e.end());
        if (g_psel == id) g_psel = 0;
        pg.version++;
    } else if (v == "s" && argc >= 4) {
        PSpot n = {A(2), F(3), argc >= 5 ? F(4) : -1.0f};
        bool found = false;
        for (PSpot &s : pg.spots) if (!strcasecmp(s.name.c_str(), n.name.c_str())) { s = n; found = true; }
        if (!found) pg.spots.push_back(n);
        p_spots_changed();
    } else if (v == "sd" && argc >= 3) {
        std::string n = A(2);
        pg.spots.erase(std::remove_if(pg.spots.begin(), pg.spots.end(), [&](const PSpot &s) { return !strcasecmp(s.name.c_str(), n.c_str()); }), pg.spots.end());
        p_spots_changed();
    } else if (v == "cfg" && argc >= 6) {
        pg.autofinish = I(2) != 0; pg.saveat_here = I(3) != 0; pg.daystart = I(4); pg.today = A(5);
        if (argc >= 7 && I(6) >= 5 && I(6) != pg.bin) { pg.bin = I(6); pg.version++; }
    } else if (v == "done") pg.synced = true;
    else if (v == "open") gui_prog_open();
    else if (v == "msg" && argc >= 3) toast(A(2));
    else if (v == "attempt" && argc >= 7) {
        pg.att = I(2) != 0; pg.att_from = A(3); pg.att_ft = F(4); pg.att_now = F(5); pg.att_fail = F(6);
    }
    // (testing: pages, a picked attempt, the mouse, the keyboard focus, a question, the graph's days)
    else if (v == "tab" && argc >= 3) p_view(I(2) <= 0 ? PV_LOG : I(2) <= 2 ? PV_GRAPH : PV_SPOTS, false);
    else if (v == "pick" && argc >= 3) p_pick(I(2));
    else if (v == "mouse" && argc >= 4) { g_vx = F(2); g_vy = F(3); }
    else if (v == "focus" && argc >= 3) p_focus(I(2));
    else if (v == "ask") p_ask("Delete this attempt?", "2:57 \xE2\x80\x93 end", [] {});
    else if (v == "noask") p_ask_close(false);
    else if (v == "key" && argc >= 3) {   // (testing: a key as the SDL filter would count it)
        std::string k = A(2);
        if (k == "up") in.nav_y--; else if (k == "down") in.nav_y++; else if (k == "left") in.nav_x--; else if (k == "right") in.nav_x++;
        else if (k == "pgdn") in.flip++; else if (k == "pgup") in.flip--; else if (k == "enter") in.enter = true;
        else if (k == "del") in.del = true; else if (k == "tab") in.tabs++; else if (k == "esc") prog_esc();
        else in.typed += k;
    }
    else if (v == "graph" && argc >= 5) { g_pg_mode = I(2); g_pg_a = I(3); g_pg_b = I(4); }
}

void gui_prog_open()
{
    if (!g_installed) return;
    if (g_open && g_mode == MODE_PROG) return;
    if (g_open) close_window();
    g_mode = MODE_PROG;
    open_window();
    if (!g_open) return;
    p_send("sm_progui view 1");
    if (!pg.have) p_send("sm_progui sync");
}

static void prog_on_close() { p_send("sm_progui view 0"); g_focus = 0; }

void gui_prog_toggle()
{
    if (g_open && g_mode == MODE_PROG) { close_window(); return; }
    if (!g_prog_server) { cstv_print("This server doesn't have the Progress Log."); return; }
    gui_prog_open();
}

void gui_prog_level_end() { g_prog_server = false; }

// ---------------------------------------------------------------- Minecraft's pictures
// progbook.bin (mcassets.py): "PBK1", a count, then (name[32], u16 w, u16 h, u32 offset) each, then RGBA pixels
enum { PS_BOOK, PS_BOOK_M, PS_FWD, PS_FWD_HL, PS_BACK, PS_BACK_HL, PS_TAB, PS_TAB_ON, PS_ICON_BOOK, PS_ICON_MAP, PS_ICON_COMPASS,
       PS_BUTTON, PS_BUTTON_HL, PS_BUTTON_OFF, PS_SCROLLER, PS_SCROLLER_BG, PS_TOOLTIP, PS_POPUP,
       PS_XP_BG, PS_XP_GREEN, PS_XP_BLUE, PS_XP_RED, PS_XP_RAINBOW, PS_FONT_ASCII, PS_FONT_NONLATIN, PS_COUNT };
static const struct { const char *name; bool mirror; } P_PICS[PS_COUNT] = {
    {"book", false}, {"book", true}, {"page_forward", false}, {"page_forward_hl", false}, {"page_backward", false},
    {"page_backward_hl", false}, {"tab", true}, {"tab_on", true}, {"icon_book", false}, {"icon_map", false},
    {"icon_compass", false}, {"button", false}, {"button_hl", false}, {"button_off", false}, {"scroller", false},
    {"scroller_bg", false}, {"tooltip_bg", false}, {"popup", false}, {"xp_bg", false}, {"xp_green", false},
    {"xp_blue", false}, {"xp_red", false}, {"xp_rainbow", false}, {"font_ascii", false}, {"font_nonlatin", false}};

// the font: where each character is on its two sheets (progfont.txt: "<hex code point> <sheet> <x> <y> <advance>",
// sheet -1 a space); its glyphs are 8 px high, the advance their width + 1, as the game draws them
struct PGlyph { uint32_t cp; int sheet, x, y, adv; };
static std::vector<PGlyph> g_pglyphs;   // by code point

struct PImg { int w = 0, h = 0; std::vector<unsigned char> px; };
static PImg g_pimg[PS_COUNT];
static int g_passets = -1;   // -1 not looked for yet, 0 none (a plain book), 1 loaded
static bool g_psounds, g_part;

// a file in cstrike/custom/cstv: from the game's folder (Steam starts the game in it), else the executable's
static FILE *p_open_asset(const char *rel)
{
    std::string a = std::string("cstrike/custom/cstv/") + rel;
    if (FILE *f = fopen(a.c_str(), "rb")) return f;
#ifndef _WIN32
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0;
        if (char *sl = strrchr(exe, '/')) {
            *sl = 0;
            if (FILE *f = fopen((std::string(exe) + "/" + a).c_str(), "rb")) return f;
        }
    }
#endif
    return nullptr;
}

static void p_load_assets()
{
    if (g_passets >= 0) return;
    g_passets = 0;
    FILE *f = p_open_asset("resource/cstv/progbook.bin");
    if (!f) return;
    std::vector<unsigned char> d;
    unsigned char buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
    fclose(f);
    if (d.size() < 8 || memcmp(d.data(), "PBK1", 4)) return;
    uint32_t count;
    memcpy(&count, &d[4], 4);
    size_t blob = 8 + (size_t)count * 40;
    if (count > 4096 || d.size() < blob) return;
    PImg frame;
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char *h = &d[8 + (size_t)i * 40];
        std::string name((const char *)h, strnlen((const char *)h, 32));
        uint16_t w, ht;
        uint32_t off;
        memcpy(&w, h + 32, 2);
        memcpy(&ht, h + 34, 2);
        memcpy(&off, h + 36, 4);
        size_t bytes = (size_t)w * ht * 4;
        if (!w || !ht || blob + off + bytes > d.size()) continue;
        PImg im;
        im.w = w;
        im.h = ht;
        im.px.assign(d.begin() + blob + off, d.begin() + blob + off + bytes);
        if (name == "tooltip_frame") frame = im;
        for (int s = 0; s < PS_COUNT; s++) {
            if (name != P_PICS[s].name) continue;
            g_pimg[s] = im;
            if (P_PICS[s].mirror)
                for (int y = 0; y < ht; y++)
                    for (int x = 0; x < w / 2; x++)
                        for (int c = 0; c < 4; c++) std::swap(g_pimg[s].px[(y * w + x) * 4 + c], g_pimg[s].px[(y * w + w - 1 - x) * 4 + c]);
        }
    }
    if (!g_pimg[PS_BOOK].w || !g_pimg[PS_BUTTON].w || !g_pimg[PS_XP_GREEN].w) return;
    // the tooltip: its frame over its background, one picture
    PImg &tb = g_pimg[PS_TOOLTIP];
    if (tb.w && frame.w == tb.w && frame.h == tb.h)
        for (size_t i = 0; i < tb.px.size(); i += 4) {
            int a = frame.px[i + 3];
            if (!a) continue;
            for (int c = 0; c < 3; c++) tb.px[i + c] = (unsigned char)((frame.px[i + c] * a + tb.px[i + c] * (255 - a)) / 255);
            tb.px[i + 3] = (unsigned char)std::max<int>(tb.px[i + 3], a);
        }
    // VGUI draws partly see-through pixels weaker than asked: the nearly solid ones (the tooltip's 240) solid
    for (PImg &im : g_pimg)
        for (size_t i = 3; i < im.px.size(); i += 4)
            if (im.px[i] >= 200) im.px[i] = 255;
    g_passets = 1;
    if (FILE *s = p_open_asset("sound/progbook/click.wav")) { g_psounds = true; fclose(s); }
    if (FILE *t = p_open_asset("resource/cstv/progfont.txt")) {
        char line[96];   // (not fscanf: glibc 2.38's __isoc23_fscanf, too new for friends, `make check-glibc`)
        while (fgets(line, sizeof line, t)) {
            char *p = line;
            long v[5];
            int n = 0;
            for (; n < 5; n++) {
                char *end;
                v[n] = n ? strtol(p, &end, 10) : (long)strtoul(p, &end, 16);
                if (end == p) break;
                p = end;
            }
            if (n == 5 && v[4] >= 0 && v[4] < 32 && v[1] >= -1 && v[1] <= 1) g_pglyphs.push_back({(uint32_t)v[0], (int)v[1], (int)v[2], (int)v[3], (int)v[4]});
        }
        fclose(t);
        std::sort(g_pglyphs.begin(), g_pglyphs.end(), [](const PGlyph &a, const PGlyph &b) { return a.cp < b.cp; });
        // Each 8 x 8 glyph on its own with a clear pixel around it: VGUI draws a picture a screen pixel lower
        // and further right than asked (togl), so a glyph would show the top of the one under it on the sheet.
        for (int s = PS_FONT_ASCII; s <= PS_FONT_NONLATIN; s++) {
            PImg &im = g_pimg[s];
            if (!im.w) continue;
            PImg out;
            out.w = im.w / 8 * 10;
            out.h = im.h / 8 * 10;
            out.px.assign((size_t)out.w * out.h * 4, 0);
            for (int y = 0; y < im.h / 8 * 8; y++)
                for (int x = 0; x < im.w / 8 * 8; x++)
                    memcpy(&out.px[((size_t)(y / 8 * 10 + 1 + y % 8) * out.w + x / 8 * 10 + 1 + x % 8) * 4], &im.px[((size_t)y * im.w + x) * 4], 4);
            im = std::move(out);
        }
        for (PGlyph &g : g_pglyphs) { g.x = g.x / 8 * 10 + 1; g.y = g.y / 8 * 10 + 1; }
    }
}

// The pictures for one scale in one VGUI texture: in columns, as high as the tallest, each pixel k x k.
struct PSpr { int x, y, w, h; };
struct PAtlas { int tex = -1, scale = 0, tw = 0, th = 0; PSpr s[PS_COUNT]; };
static PAtlas g_pbook, g_pui, g_pfont, g_ptabs;   // at the book's scale, at the widgets' (and the font's), the tabs'
static int p_tab_scale() { return g_book_t + 1; }  // the tabs a bit bigger than the widgets, smaller than the book

static int p_pow2(int v) { int p = 64; while (p < v) p *= 2; return p; }

// VGUI keeps a texture at the size it was first given: a bigger one is a new texture
static void p_upload(int &tex, int &tw, int &th, int w, int h, const std::function<void(std::vector<unsigned char> &, int)> &fill)
{
    int W = p_pow2(w), H = p_pow2(h);
    if (tex < 0 || W > tw || H > th) {
        tex = g_surf->CreateNewTextureID(true);
        tw = W;
        th = H;
    }
    std::vector<unsigned char> buf((size_t)tw * th * 4, 0);
    fill(buf, tw);
    g_surf->DrawSetTextureRGBA(tex, buf.data(), tw, th, 0, true);
}

static void p_blit(std::vector<unsigned char> &buf, int stride, const PImg &im, int x0, int y0, int k)
{
    for (int y = 0; y < im.h; y++)
        for (int x = 0; x < im.w; x++) {
            const unsigned char *s = &im.px[((size_t)y * im.w + x) * 4];
            for (int dy = 0; dy < k; dy++)
                for (int dx = 0; dx < k; dx++) memcpy(&buf[(((size_t)(y0 + y) * k + dy) * stride + (size_t)(x0 + x) * k + dx) * 4], s, 4);
        }
}

static void p_build(PAtlas &a, std::initializer_list<int> which, int k)
{
    if (a.scale == k) return;
    std::vector<int> list;
    for (int i : which) if (g_pimg[i].w) list.push_back(i);
    std::stable_sort(list.begin(), list.end(), [](int p, int q) { return g_pimg[p].h > g_pimg[q].h; });
    int colh = list.empty() ? 1 : g_pimg[list[0]].h, x = 0, y = 0, colw = 0, aw = 1;
    for (int i : list) {
        const PImg &im = g_pimg[i];
        if (y && y + im.h > colh) { x += colw + 1; y = 0; colw = 0; }
        a.s[i] = {x, y, im.w, im.h};
        y += im.h + 1;
        colw = std::max(colw, im.w);
        aw = std::max(aw, x + im.w);
    }
    p_upload(a.tex, a.tw, a.th, aw * k, colh * k, [&](std::vector<unsigned char> &buf, int stride) {
        for (int i : list) p_blit(buf, stride, g_pimg[i], a.s[i].x, a.s[i].y, k);
    });
    a.scale = k;
}

// part of a picture (its pixels sx, sy, sw, sh) into a box on the screen
static void p_part(const PAtlas &a, int i, int sx, int sy, int sw, int sh, int x, int y, int w, int h, Col c = M_WHITE)
{
    const PSpr &s = a.s[i];
    float k = (float)a.scale;
    g_surf->DrawSetColor(c.r, c.g, c.b, c.a);
    g_surf->DrawSetTexture(a.tex);
    g_surf->DrawTexturedSubRect(x, y, x + w, y + h, (s.x + sx) * k / a.tw, (s.y + sy) * k / a.th, (s.x + sx + sw) * k / a.tw, (s.y + sy + sh) * k / a.th);
}

static void p_pic(const PAtlas &a, int i, int x, int y, Col c = M_WHITE)
{
    p_part(a, i, 0, 0, a.s[i].w, a.s[i].h, x, y, a.s[i].w * a.scale, a.s[i].h * a.scale, c);
}

// a nine-slice picture: its corners (b pixels) as they are, its edges and middle stretched
static void p_nine(const PAtlas &a, int i, int b, int x, int y, int w, int h)
{
    const PSpr &s = a.s[i];
    int B = b * a.scale;
    int xs[4] = {x, x + B, x + w - B, x + w}, ys[4] = {y, y + B, y + h - B, y + h};
    int us[4] = {0, b, s.w - b, s.w}, vs[4] = {0, b, s.h - b, s.h};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            if (xs[c + 1] > xs[c] && ys[r + 1] > ys[r])
                p_part(a, i, us[c], vs[r], us[c + 1] - us[c], vs[r + 1] - vs[r], xs[c], ys[r], xs[c + 1] - xs[c], ys[r + 1] - ys[r]);
}

static void p_sound(const char *name)
{
    if (!g_psounds || !g_engine) return;
    char c[64];
    snprintf(c, sizeof c, "play progbook/%s.wav", name);
    g_engine->ClientCmd_Unrestricted(c);
}

static void p_flip_sound()
{
    static int n;
    char b[8];
    snprintf(b, sizeof b, "flip%d", 1 + n++ % 3);
    p_sound(b);
}

// ---------------------------------------------------------------- keys: what has the keyboard
struct PNav { int id, x, y, w, h; };
static std::vector<PNav> g_pnav, g_pnav_last;   // the controls drawn this frame, last frame
static int g_psplit = 1 << 30;                  // where the right page starts (a spread)

static void p_nav(int id, int x, int y, int w, int h) { g_pnav.push_back({id, x, y, w, h}); }
static bool p_chrome(int id) { return id >= PB_DONE && id < PB_TAB + PV_COUNT; }   // the book's own: Done, arrows, tabs
static bool p_is_input(int id) { return id >= 900 && id < 1000; }
static void p_focus(int id) { g_focus = id; g_pall = p_is_input(id) ? id : 0; }

// on to the nearest control that way (one beside it on its line before one further down)
static bool p_nav_go(int dx, int dy)
{
    const PNav *cur = nullptr;
    for (const PNav &n : g_pnav_last) if (n.id == g_focus) cur = &n;
    if (!cur) return false;
    int cx = cur->x + cur->w / 2, cy = cur->y + cur->h / 2;
    const PNav *best = nullptr;
    long bs = 0;
    for (const PNav &n : g_pnav_last) {
        if (n.id == cur->id) continue;
        int nx = n.x + n.w / 2, ny = n.y + n.h / 2;
        long along, across;
        if (dx) {
            along = (long)(nx - cx) * dx;
            bool same_line = n.y < cur->y + cur->h && cur->y < n.y + n.h;
            across = same_line ? 0 : std::abs(ny - cy) + 10000;
        } else {
            along = (long)(ny - cy) * dy;
            across = std::abs(nx - cx);
        }
        if (along <= 0) continue;
        long score = along + across * (dx ? 4 : 2);
        if (!best || score < bs) { best = &n; bs = score; }
    }
    if (!best) return false;
    p_focus(best->id);
    return true;
}

// the first control in reading order (the left page's top line, its leftmost), or the last line's; the pages'
// controls before the book's own (tabs, arrows, Done)
static void p_nav_first(bool last)
{
    for (int pass = 0; pass < 2; pass++) {
        const PNav *line = nullptr;
        auto key = [&](const PNav &n) { return (long)(n.x >= g_psplit) * 100000 + n.y + n.h / 2; };
        for (const PNav &n : g_pnav_last)
            if ((pass || !p_chrome(n.id)) && (!line || (last ? key(n) > key(*line) : key(n) < key(*line)))) line = &n;
        if (!line) continue;
        const PNav *best = line;   // on its line (overlapping it, same page), the leftmost
        for (const PNav &n : g_pnav_last)
            if ((pass || !p_chrome(n.id)) && (n.x >= g_psplit) == (line->x >= g_psplit) && n.y < line->y + line->h && line->y < n.y + n.h && n.x < best->x) best = &n;
        p_focus(best->id);
        return;
    }
}

// a list's rows (ids base + index): Up / Down along them, scrolled to stay in sight; past the top, on up the page
static void p_list_keys(int base, int count, int &first, int visible)
{
    if (!in.nav_y || g_focus < base || g_focus >= base + 100000) return;
    int i = g_focus - base + in.nav_y;
    in.nav_y = 0;
    if (i < 0) { p_nav_go(0, -1); return; }
    i = std::min(i, count - 1);
    if (i < first) first = i;
    if (i >= first + visible) first = i - visible + 1;
    g_focus = base + i;
}

// ---------------------------------------------------------------- text in Minecraft's font
// Drawn from its glyph pictures, a pixel g_book_t screen px like the widgets (so a button's word sits in it as in
// the game); bold drawn twice a pixel apart and a pixel wider, as the game does. F_MC / F_MCB (and F_MC2 / F_MCB2
// twice the size); without the pictures VGUI's font in those sizes.
static bool p_glyphs() { return g_part && g_pfont.tex >= 0 && !g_pglyphs.empty(); }

static uint32_t p_utf8(const std::string &s, size_t &i)
{
    unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = c & (0x3F >> n);
    for (; n > 0 && i < s.size(); n--) cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    return cp;
}

static const PGlyph *p_glyph(uint32_t cp)
{
    auto it = std::lower_bound(g_pglyphs.begin(), g_pglyphs.end(), cp, [](const PGlyph &g, uint32_t c) { return g.cp < c; });
    if (it != g_pglyphs.end() && it->cp == cp) return &*it;
    return cp == '?' ? nullptr : p_glyph('?');
}

static int p_fk(int f) { return g_book_t * (f >= F_MC2 ? 2 : 1); }
static bool p_fbold(int f) { return f == F_MCB || f == F_MCB2; }

static int mt_w(int f, const std::string &s)
{
    if (!p_glyphs()) return text_w(f, s);
    int w = 0;
    for (size_t i = 0; i < s.size();)
        if (const PGlyph *g = p_glyph(p_utf8(s, i))) w += g->adv + (p_fbold(f) && g->sheet >= 0 ? 1 : 0);
    return w * p_fk(f);
}

static int mt_h(int f) { return p_glyphs() ? 8 * p_fk(f) : g_font_tall[f]; }

// at most maxw wide (0 = no limit), cut with an ellipsis; returns the width drawn
static int mt(int f, int x, int y, Col c, const std::string &s0, int maxw = 0)
{
    if (!p_glyphs()) return text(f, x, y, c, s0, maxw);
    std::string s = s0;
    if (maxw > 0 && mt_w(f, s) > maxw) {
        while (!s.empty() && mt_w(f, s + "\xE2\x80\xA6") > maxw) {
            size_t i = s.size() - 1;
            while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
            s.erase(i);
        }
        s += "\xE2\x80\xA6";
    }
    int k = p_fk(f), cx = x;
    bool bold = p_fbold(f);
    g_surf->DrawSetColor(c.r, c.g, c.b, c.a);
    g_surf->DrawSetTexture(g_pfont.tex);
    float sc = (float)g_pfont.scale;
    for (size_t i = 0; i < s.size();) {
        const PGlyph *g = p_glyph(p_utf8(s, i));
        if (!g) continue;
        if (g->sheet >= 0 && g->adv > 1) {
            const PSpr &sp = g_pfont.s[g->sheet ? PS_FONT_NONLATIN : PS_FONT_ASCII];
            float u0 = (sp.x + g->x) * sc / g_pfont.tw, v0 = (sp.y + g->y) * sc / g_pfont.th;
            float u1 = (sp.x + g->x + g->adv - 1) * sc / g_pfont.tw, v1 = (sp.y + g->y + 8) * sc / g_pfont.th;
            for (int b = 0; b <= (bold ? 1 : 0); b++)
                g_surf->DrawTexturedSubRect(cx + b * k, y, cx + b * k + (g->adv - 1) * k, y + 8 * k, u0, v0, u1, v1);
        }
        cx += (g->adv + (bold && g->sheet >= 0 ? 1 : 0)) * k;
    }
    return cx - x;
}

static int mt_vc(int f, int y, int h) { return y + (h - mt_h(f)) / 2; }

// ---------------------------------------------------------------- the widgets
static std::vector<std::pair<std::string, Col>> g_ptip;   // the tooltip this frame (lines), drawn last
static int g_ptip_x = -1, g_ptip_y;                       // where (a control picked with the keys), -1 at the mouse

static void p_tip(const std::string &s, Col c = M_WHITE) { g_ptip.push_back({s, c}); }
static void p_tip_at(int x, int y) { g_ptip_x = x; g_ptip_y = y; }

// with Minecraft's text shadow: the colour at a quarter, a pixel down and right
static int p_stext(int f, int x, int y, Col c, const std::string &s, int maxw = 0)
{
    int k = BT(f >= F_MC2 ? 2 : 1);
    mt(f, x + k, y + k, {c.r / 4, c.g / 4, c.b / 4, c.a}, s, maxw);
    return mt(f, x, y, c, s, maxw);
}

static int p_text_r(int f, int xr, int y, Col c, const std::string &s)   // right-aligned at xr
{
    int w = mt_w(f, s);
    mt(f, xr - w, y, c, s);
    return w;
}

// Minecraft's button (w widget pixels wide, 20 high: its picture's two ends, as the game draws a narrower one).
// lit: -1 a plain button, 1 the chosen one of a choice, 0 one not chosen. True when pressed (a click, or Enter
// while it has the keyboard).
static bool p_button(int id, int x, int y, int w, const std::string &label, bool enabled = true, int lit = -1)
{
    int W = BT(w), H = BT(20);
    if (enabled) p_nav(id, x, y, W, H);
    bool hv = enabled && (hover(x, y, W, H) || g_focus == id);
    int pic = hv ? PS_BUTTON_HL : !enabled || lit == 0 ? PS_BUTTON_OFF : PS_BUTTON;
    if (g_part) {
        int half = w / 2;
        p_part(g_pui, pic, 0, 0, half, 20, x, y, BT(half), H);
        p_part(g_pui, pic, 200 - (w - half), 0, w - half, 20, x + BT(half), y, BT(w - half), H);
    } else {
        rect(x, y, W, H, M_INK);
        rect(x + BT(1), y + BT(1), W - BT(2), H - BT(2), hv ? Col{120, 130, 190, 255} : pic == PS_BUTTON_OFF ? Col{44, 44, 44, 255} : Col{110, 110, 110, 255});
    }
    Col tc = !enabled ? M_OFF : lit == 1 ? M_YELLOW : lit == 0 && !hv ? M_OFF : M_WHITE;
    int tw = std::min(mt_w(F_MC, label), W - BT(8));
    p_stext(F_MC, x + (W - tw) / 2, y + BT(6), tc, label, W - BT(8));
    bool go = enabled && (take_click(x, y, W, H) || (g_focus == id && in.enter));
    if (go) {
        if (g_focus == id) in.enter = false;
        p_sound("click");
    }
    return go;
}

// a word you click, like a link in a written book: blue and underlined under the mouse or the keyboard
static bool p_link(int id, int x, int y, const std::string &label, bool enabled = true, int f = F_MCB, Col c = M_INK)
{
    int w = mt_w(f, label), h = mt_h(f);
    int bx = x - BT(2), by = y - BT(2), bw = w + BT(4), bh = h + BT(3);
    if (enabled) p_nav(id, bx, by, bw, bh);
    bool hv = enabled && (hover(bx, by, bw, bh) || g_focus == id);
    Col col = !enabled ? M_FAINT : hv ? M_BLUE : c;
    mt(f, x, y, col, label);
    if (hv) rect(x, y + BT(8), w, BT(1), col);
    bool go = enabled && (take_click(bx, by, bw, bh) || (g_focus == id && in.enter));
    if (go && g_focus == id) in.enter = false;
    return go;
}

// typed straight onto the page, as in a book and quill: the text and Minecraft's blinking _ after it, a dotted
// line under it to show where. True when Enter is pressed in it.
static bool p_input(int id, std::string &s, size_t maxlen, int x, int y, int w, const char *hint)
{
    int h = mt_h(F_MC) + BT(3);
    p_nav(id, x, y - BT(2), w, h + BT(2));
    if (g_pall != g_focus) g_pall = 0;
    bool hv = hover(x, y - BT(2), w, h + BT(2));
    if (take_click(x, y - BT(2), w, h + BT(2))) { g_focus = id; g_pall = 0; }
    bool focused = g_focus == id, enter = false;
    if (focused) {
        std::string add = in.typed;
        if (in.paste && SDL_GetClipboardText_) {
            if (char *clip = SDL_GetClipboardText_()) {
                add += clip;
                if (SDL_free_) SDL_free_(clip);
            }
        }
        if (in.backspaces > 0 || !add.empty()) {
            if (g_pall == id) { s.clear(); in.backspaces = 0; }   // the picked text goes first
            g_pall = 0;
        }
        for (; in.backspaces > 0 && !s.empty(); in.backspaces--) {
            size_t i = s.size() - 1;
            while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
            s.erase(i);
        }
        for (char c : add)
            if ((unsigned char)c >= 0x20 && s.size() < maxlen) s += c;
        enter = in.enter;
        in.typed.clear();
        in.backspaces = 0;
        in.paste = in.enter = false;
    }
    Col line = focused ? M_INK : hv ? M_DIM : M_FAINT;
    for (int dx = 0; dx + BT(1) <= w; dx += BT(2)) rect(x + dx, y + h - BT(1), BT(1), BT(1), line);
    if (s.empty() && !focused) {
        mt(F_MC, x, y, M_FAINT, hint, w);
        return false;
    }
    std::string t = s;   // its end, when it doesn't fit
    int room = w - mt_w(F_MC, "_");
    while (!t.empty() && mt_w(F_MC, t) > room) {
        size_t i = 1;
        while (i < t.size() && ((unsigned char)t[i] & 0xC0) == 0x80) i++;
        t.erase(0, i);
    }
    bool picked = focused && g_pall == id && !t.empty();
    if (picked) rect(x - BT(1), y - BT(1), mt_w(F_MC, t) + BT(2), mt_h(F_MC) + BT(1), M_PICK);
    mt(F_MC, x, y, picked ? M_WHITE : M_INK, t);
    if (focused && !picked && (long)(plat_now() / 0.3) % 2 == 0) mt(F_MC, x + mt_w(F_MC, t), y, M_INK, "_");
    return enter;
}

// Tab: on to the next input of the row (after the last, the first again), its text picked
static void p_tabs(std::initializer_list<int> ring)
{
    if (in.tabs <= 0) return;
    const int *id = ring.begin(), n = (int)ring.size();
    for (int i = 0; i < n; i++)
        if (g_focus == id[i]) {
            g_focus = g_pall = id[(i + in.tabs) % n];
            in.tabs = 0;
            return;
        }
}

// Minecraft's XP bar (182 x 5) as the track from start to your end: its background, and its filled picture over
// the part from a to b (seconds), in the attempt's colour: green from Start, blue practice from somewhere else,
// red practice that got to the end, a rainbow (moving) for a full run
enum { PK_START, PK_PRACTICE, PK_PRACTICE_END, PK_FULL };
static int p_bar_kind(bool from_start, bool to_end) { return from_start ? (to_end ? PK_FULL : PK_START) : to_end ? PK_PRACTICE_END : PK_PRACTICE; }

static void p_xp(int x, int y, float scale, float a, float b, int kind, int alpha = 255)
{
    int k = g_book_t;
    int ta = std::min(std::max((int)(std::max(a, 0.0f) / scale * 182 + 0.5f), 0), 181);
    int tb = b < 0 ? ta : std::min(std::max((int)(b / scale * 182 + 0.5f), ta + 1), 182);
    if (g_part) {
        p_pic(g_pui, PS_XP_BG, x, y);
        if (b < 0) return;
        static const int pic[4] = {PS_XP_GREEN, PS_XP_BLUE, PS_XP_RED, PS_XP_RAINBOW};
        int frame = kind == PK_FULL ? (int)(plat_now() * 8) % std::max(1, g_pui.s[PS_XP_RAINBOW].h / 5) : 0;
        p_part(g_pui, pic[kind], ta, frame * 5, tb - ta, 5, x + ta * k, y, (tb - ta) * k, 5 * k, {255, 255, 255, alpha});
        return;
    }
    rect(x, y, 182 * k, 5 * k, {9, 16, 12, 255});
    if (b < 0) return;
    static const Col c[4] = {{128, 255, 32, 255}, {80, 140, 255, 255}, {255, 80, 70, 255}, {255, 200, 40, 255}};
    rect(x + ta * k, y + k, (tb - ta) * k, 3 * k, {c[kind].r, c[kind].g, c[kind].b, alpha});
}

// Minecraft's tooltip, at the mouse (drawn last, over everything)
static void p_draw_tip(int sw, int sh)
{
    if (g_ptip.empty()) return;
    int w = 0, n = (int)g_ptip.size();
    for (auto &l : g_ptip) w = std::max(w, mt_w(F_MC, l.first));
    int h = BT(8) + (n > 1 ? BT(2) + (n - 1) * BT(10) : 0);
    int ax = g_ptip_x >= 0 ? g_ptip_x : g_mx, ay = g_ptip_x >= 0 ? g_ptip_y : g_my;
    int x = ax + BT(12), y = ay - BT(12);
    if (x + w + BT(4) > sw) x = std::max(BT(4), ax - BT(16) - w);
    y = std::min(std::max(y, BT(4)), sh - h - BT(4));
    if (g_part) p_nine(g_pui, PS_TOOLTIP, 10, x - BT(12), y - BT(12), w + BT(24), h + BT(24));
    else {
        rect(x - BT(3), y - BT(3), w + BT(6), h + BT(6), {16, 0, 16, 255});
        outline(x - BT(3), y - BT(3), w + BT(6), h + BT(6), {80, 0, 255, 255});
    }
    for (int i = 0; i < n; i++) p_stext(F_MC, x, y + (i ? BT(2) + i * BT(10) : 0), g_ptip[i].second, g_ptip[i].first);
}

// ---- Yes / No: over the book; arrows pick, Enter takes, Esc is No, clicks work
static struct { bool open; std::string title, sub; std::function<void()> yes; int back; } g_pask;

static void p_ask(const std::string &title, const std::string &sub, std::function<void()> yes)
{
    g_pask.open = true;
    g_pask.title = title;
    g_pask.sub = sub;
    g_pask.yes = std::move(yes);
    g_pask.back = g_focus;
    g_focus = PB_NO;
}

static void p_ask_close(bool yes)
{
    g_pask.open = false;
    g_focus = g_pask.back;
    if (yes && g_pask.yes) g_pask.yes();
}

static bool prog_esc()
{
    if (!g_pask.open) return false;
    p_ask_close(false);
    return true;
}

static void p_draw_ask(int sw, int sh)
{
    rect(0, 0, sw, sh, {0, 0, 0, 130});
    int w = BT(200), h = BT(70), x = (sw - w) / 2, y = (sh - h) / 2;
    if (g_part) p_nine(g_pui, PS_POPUP, 6, x, y, w, h);
    else { rect(x, y, w, h, {48, 48, 48, 255}); outline(x, y, w, h, M_INK); }
    p_stext(F_MC, x + (w - mt_w(F_MC, g_pask.title)) / 2, y + BT(10), M_WHITE, g_pask.title);
    p_stext(F_MC, x + (w - mt_w(F_MC, g_pask.sub)) / 2, y + BT(23), M_GRAY, g_pask.sub, w - BT(16));
    if (in.nav_x) g_focus = in.nav_x < 0 ? PB_YES : PB_NO;
    if (g_focus != PB_YES && g_focus != PB_NO) g_focus = PB_NO;
    int bw = 80, by = y + h - BT(30);
    if (p_button(PB_YES, x + BT(14), by, bw, "Yes")) { p_ask_close(true); return; }
    if (p_button(PB_NO, x + w - BT(14 + bw), by, bw, "No")) p_ask_close(false);
}

// ---------------------------------------------------------------- the book
static void p_view(int v, bool sound)
{
    v = std::min(std::max(v, 0), PV_COUNT - 1);
    if (v == g_pview) return;
    g_pview = v;
    if (g_focus != PB_BACK && g_focus != PB_FWD && (g_focus < PB_TAB || g_focus >= PB_TAB + PV_COUNT)) g_focus = 0;
    g_pall = 0;
    if (sound) p_flip_sound();
}

// a page's first line: its name, and where you are in the book ("Page 2 of 5", as the game writes it)
static void p_head(int x, int y, int w, const std::string &title, int page)
{
    char pi[32];
    snprintf(pi, sizeof pi, "Page %d of 5", page);
    int piw = p_text_r(F_MC, x + w, y, M_INK, pi);
    mt(F_MCB, x, y, M_INK, title, w - piw - BT(8));
}

// the scale bars share: x for a time (whole widget pixels, as the boss bar's)
struct PTrack {
    int x, w;
    float scale;
    int X(float t) const { return x + (int)(std::min(std::max(t, 0.0f), scale) / scale * (w / g_book_t)) * g_book_t; }
};

// spot names along a track (each over its middle, those with no room left out); their ranges in faint bands.
// (no "end" over page 1's bars: the rows that got there say it on the right)
static void p_spot_scale(const PTrack &tr, int y, int top, int bottom, bool start)
{
    int last = tr.x - 10000, endx = tr.x + tr.w;
    if (start) {
        mt(F_MC, tr.x, y, M_FAINT, "start");
        last = tr.x + mt_w(F_MC, "start") + BT(4);
    }
    for (const PSpot &s : pg.spots) {
        if (p_is_end(s) || s.lo > tr.scale) continue;
        int x0 = tr.X(s.lo), x1 = tr.X(p_spot_hi(s));
        rect(x0, top, std::max(BT(1), x1 - x0), bottom - top, M_BAND);
        std::string n = p_lower(s.name);
        int nw = mt_w(F_MC, n), nx = (x0 + x1) / 2 - nw / 2;
        nx = std::max(nx, tr.x);
        if (nx < last || nx + nw > endx) continue;   // no room for its name
        mt(F_MC, nx, y, M_DIM, n);
        last = nx + nw + BT(4);
    }
}

// the attempt's kind, by its bar: from start / practice; to the end or not
static const char *p_kind(const PEntry &e)
{
    bool s = p_from_start(e), end = p_to_end(e);
    return s && end ? "full run" : s ? "from start" : end ? "practice to the end" : "practice";
}

// ---- page 1: the log
static void p_est_finish(int xr, int y)
{
    if (g_focus == PI_END) {   // being changed: an input, Enter keeps it
        int iw = BT(34);
        int lx = xr - iw - BT(4) - mt_w(F_MC, "est. finish");
        mt(F_MC, lx, y, M_DIM, "est. finish");
        if (p_input(PI_END, g_pend, 15, xr - iw, y, iw, "4:10")) {
            std::string t = p_clean(g_pend);
            if (!t.empty()) p_send("sm_progui spot \"End\" \"%s\"", t.c_str());
            g_focus = 0;
        }
        return;
    }
    std::string v = pg.end > 0 ? p_clock(pg.end) : "set";
    int vw = mt_w(F_MCB, v);
    mt(F_MC, xr - vw - BT(4) - mt_w(F_MC, "est. finish"), y, M_DIM, "est. finish");
    if (p_link(PB_END, xr - vw, y, v, true, F_MCB, pg.end > 0 ? M_GREEN : M_FAINT)) {
        g_pend = pg.end > 0 ? p_clock(pg.end) : "";
        p_focus(PI_END);
    }
}

// add an attempt, or change the picked one
static void p_entry_form(int x, int y, int w)
{
    const PEntry *e = g_psel ? p_entry(g_psel) : nullptr;
    if (g_psel && !e) g_psel = 0;
    std::string &from = e ? g_pe_from : g_padd_from, &to = e ? g_pe_to : g_padd_to;
    int iw = BT(42), cx = x, ty = y + BT(6);
    cx += mt(F_MC, cx, ty, M_DIM, "from") + BT(4);
    bool go = p_input(e ? PI_EDIT_FROM : PI_ADD_FROM, from, 31, cx, ty, iw, "1:00");
    cx += iw + BT(8);
    cx += mt(F_MC, cx, ty, M_DIM, "to") + BT(4);
    go |= p_input(e ? PI_EDIT_TO : PI_ADD_TO, to, 31, cx, ty, iw, "3:33");
    if (e) p_tabs({PI_EDIT_FROM, PI_EDIT_TO});
    else p_tabs({PI_ADD_FROM, PI_ADD_TO});
    cx += iw + BT(8);
    if (e) {
        go |= p_button(PB_SAVE, cx, y, 36, "Save");
        cx += BT(38);
        if (p_button(PB_DEL, cx, y, 42, "Delete")) {
            int id = e->id;
            p_ask("Delete this attempt?", p_entry_text(*e), [id] { p_send("sm_progui del %d", id); if (g_psel == id) g_psel = 0; });
            return;
        }
        cx += BT(44);
        if (p_button(PB_CANCEL, cx, y, 42, "Cancel")) { g_psel = 0; g_focus = 0; return; }
        if (go) {
            std::string f = p_clean(g_pe_from), t = p_clean(g_pe_to);
            if (!f.empty() && f != e->from) p_send("sm_progui edit %d from \"%s\"", e->id, f.c_str());
            if (!t.empty() && t != e->to) p_send("sm_progui edit %d to \"%s\"", e->id, t.c_str());
            g_focus = PR_ENTRY + e->id;   // back to its row
            g_psel = 0;
        }
    } else {
        go |= p_button(PB_ADD, cx, y, 40, "Add");
        if (go) {
            std::string f = p_clean(g_padd_from), t = p_clean(g_padd_to);
            if (!f.empty() && !t.empty()) {
                p_send("sm_progui add \"%s\" \"%s\"", f.c_str(), t.c_str());
                g_padd_from.clear();
                g_padd_to.clear();
                p_focus(PI_ADD_FROM);   // ready for the next one
            } else p_focus(f.empty() ? PI_ADD_FROM : PI_ADD_TO);
        }
    }
}

static void p_page_log(int x, int y, int w, int h)
{
    const int bottom = y + h;
    std::string day = p_viewday();
    bool today = day == pg.today;
    std::vector<const PEntry *> list = p_day_entries(day);
    p_aggregate();
    int prev = -1, next = -1;
    for (int i = 0; i < (int)g_pdays.size(); i++) {
        if (g_pdays[i] < day) prev = i;
        if (g_pdays[i] > day && next < 0) next = i;
    }
    p_head(x, y, w, pg.have ? pg.map : "...", 1);
    y += BT(13);
    // the day (steps to the days around it with entries), and your estimated finish
    int cx = x;
    if (p_link(PB_DAY_PREV, cx, y, "<", prev >= 0)) { g_pday = g_pdays[prev] == pg.today ? "" : g_pdays[prev]; g_psel = 0; }
    cx += mt_w(F_MCB, "<") + BT(5);
    cx += mt(F_MC, cx, y, M_INK, today ? "today, " + p_day_label(day) : p_day_label(day)) + BT(5);
    if (p_link(PB_DAY_NEXT, cx, y, ">", next >= 0 || !today)) {
        g_pday = next >= 0 && g_pdays[next] != pg.today ? g_pdays[next] : "";
        g_psel = 0;
    }
    p_est_finish(x + w, y);
    y += BT(12);
    // how the day went: best counts runs from Start only
    PStats st = p_stats(list);
    if (!list.empty()) {
        cx = x;
        cx += mt(F_MC, cx, y, M_DIM, std::to_string(st.tries) + (st.tries == 1 ? " try  \xC2\xB7  best " : " tries  \xC2\xB7  best "));
        mt(F_MC, cx, y, st.best >= 0 ? M_BLUE : M_FAINT, p_clock(st.best));
    } else mt(F_MC, x, y, M_FAINT, "nothing logged this day");
    y += BT(14);
    p_entry_form(x, y, w);
    y += BT(28);

    // the bars, newest on top (the attempt being played first)
    bool live = today && pg.att && pg.att_now >= 0;
    // columns: the row's number (or "now"), its label, the XP bar (fixed: the game's picture), the % at the right;
    // the label gets what's left, so nothing runs into the next column
    int idxw = std::max(mt_w(F_MC, "now"), mt_w(F_MC, "100")) + BT(3);
    int labw = std::max(BT(20), w - idxw - BT(182) - mt_w(F_MC, "100%") - BT(2));
    PTrack tr = {x + idxw + labw, BT(182), p_scale()};
    int rh = BT(12), top = y + BT(11), count = (int)list.size() + (live ? 1 : 0);
    int visible = std::max(1, (bottom - top) / rh);
    p_spot_scale(tr, y, top, top + std::min(count, visible) * rh, true);
    // keys on the rows: Up / Down, Enter changes it, Delete asks
    int base_live = live ? 1 : 0;
    auto row_entry = [&](int i) -> const PEntry * { int k = i - base_live; return k >= 0 && k < (int)list.size() ? list[list.size() - 1 - k] : nullptr; };
    if (g_focus >= PR_ENTRY && g_focus < PR_SPOT && in.nav_y) {
        int at = -1;
        for (int i = 0; i < count; i++) if (const PEntry *e = row_entry(i)) if (PR_ENTRY + e->id == g_focus) at = i;
        if (at >= 0) {
            int i = at + in.nav_y;
            in.nav_y = 0;
            if (i < base_live) p_nav_go(0, -1);
            else {
                i = std::min(i, count - 1);
                if (i < g_pscroll) g_pscroll = i;
                if (i >= g_pscroll + visible) g_pscroll = i - visible + 1;
                g_focus = PR_ENTRY + row_entry(i)->id;
            }
        }
    }
    if (g_wheel && hover(x, top, w, bottom - top)) { g_pscroll -= g_wheel * 2; g_wheel = 0; }
    g_pscroll = std::min(std::max(g_pscroll, 0), std::max(0, count - visible));
    for (int r = 0; r < visible && g_pscroll + r < count; r++) {
        int i = g_pscroll + r, ry = top + r * rh, by = ry + (rh - BT(5)) / 2, ty = mt_vc(F_MC, ry, rh);
        if (live && i == 0) {
            bool pulse = (long)(plat_now() * 2) % 2 == 0;
            mt(F_MC, x, ty, M_BLUE, "now");
            mt(F_MC, x + idxw, ty, M_INK, p_point(pg.att_from) + " \xE2\x80\x93 " + p_clock(pg.att_now), labw - BT(4));
            p_xp(tr.x, by, tr.scale, pg.att_ft, pg.att_now, p_bar_kind(pg.att_ft >= 0 && pg.att_ft < 0.5f, false), pulse ? 255 : 140);
            p_text_r(F_MC, x + w, ty, M_DIM, p_pct(pg.att_now));
            continue;
        }
        const PEntry &e = *row_entry(i);
        int id = PR_ENTRY + e.id;
        bool sel = e.id == g_psel, kb = g_focus == id, hv = hover(x - BT(2), ry, w + BT(4), rh), fin = p_to_end(e);
        p_nav(id, x, ry, w, rh);
        if (sel) rect(x - BT(2), ry, w + BT(4), rh, M_SEL);
        else if (hv || kb) rect(x - BT(2), ry, w + BT(4), rh, M_HOVER);
        char num[12];
        snprintf(num, sizeof num, "%d", (int)list.size() - (i - base_live));
        mt(F_MC, x, ty, kb ? M_INK : M_FAINT, kb ? ">" : num);
        mt(F_MC, x + idxw, ty, sel ? M_BLUE : M_INK, p_entry_text(e), labw - BT(4));
        p_xp(tr.x, by, tr.scale, e.ft, e.tt, p_bar_kind(p_from_start(e), fin));
        std::string pct = fin ? "end" : p_pct(e.tt);
        p_text_r(F_MC, x + w, ty, fin ? M_GREEN : M_DIM, pct);
        if (kb && !hv) p_tip_at(x + w + BT(6), ry + rh / 2);
        if (hv || kb) {
            p_tip(p_entry_text(e));
            p_tip(std::string(p_kind(e)) + "  \xC2\xB7  " + p_hhmm(e.created), M_GRAY);
        }
        if (take_click(x - BT(2), ry, w + BT(4), rh) || (kb && in.enter)) {
            in.enter = false;
            if (sel) { g_psel = 0; g_focus = id; }
            else { p_pick(e.id); p_focus(PI_EDIT_FROM); }
        }
        if (kb && in.del) {
            in.del = false;
            int eid = e.id;
            p_ask("Delete this attempt?", p_entry_text(e), [eid] { p_send("sm_progui del %d", eid); if (g_psel == eid) g_psel = 0; });
        }
    }
    if (!count) mt(F_MC, tr.x, top + BT(4), M_FAINT, "no attempts yet");
    // Minecraft's scroll bar, outside the writing
    if (count > visible) {
        int sx = x + w + BT(5), sh2 = visible * rh, th = std::max(BT(8), sh2 * visible / count);
        int ty2 = top + (sh2 - th) * g_pscroll / std::max(1, count - visible);
        if (g_part) {
            p_nine(g_pui, PS_SCROLLER_BG, 1, sx, top, BT(6), sh2);
            p_nine(g_pui, PS_SCROLLER, 1, sx, ty2, BT(6), th);
        } else {
            rect(sx, top, BT(6), sh2, {0, 0, 0, 120});
            rect(sx, ty2, BT(6), th, {160, 160, 160, 255});
        }
    }
}

// ---- page 2: the graph, of runs from Start only (practice stays on page 1). Where runs end (a smoothed spread
// of fail times: % of the day's fails within a fail range), or how far they get (% still going at each time). A
// day, or a day against another.
struct PCanvas {
    int w, h;
    std::vector<unsigned char> px;
    PCanvas(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_ * 4, 0) {}
    void put(int x, int y, Col c)
    {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        unsigned char *p = &px[((size_t)y * w + x) * 4];
        p[0] = (unsigned char)c.r; p[1] = (unsigned char)c.g; p[2] = (unsigned char)c.b; p[3] = (unsigned char)c.a;
    }
};

struct PSeries { std::string label; std::vector<const PEntry *> e; PStats st; std::vector<float> v; Col line, fill; bool any; };
static struct { int tex = -1, tw = 0, th = 0; std::string key; } g_pchart;

static std::string p_series_day(int idx) { return idx >= 0 && idx < (int)g_pdays.size() ? g_pdays[idx] : ""; }
static std::string p_series_label(int idx)
{
    if (idx >= (int)g_pdays.size()) return "all days";
    std::string d = p_series_day(idx);
    return d == pg.today ? "today" : p_day_label(d);
}

// the curve over cols columns of the graph (0 .. scale seconds)
static void p_curve(PSeries &s, int cols, float scale)
{
    s.v.assign(cols, 0);
    s.any = false;
    if (g_pg_mode == 0) {   // where attempts end: a quartic kernel a fail range wide each side (no libm)
        std::vector<float> f;
        for (const PEntry *e : s.e) if (p_fail(*e)) f.push_back(e->tt);
        if (f.empty()) return;
        s.any = true;
        float hb = (float)pg.bin;
        for (int x = 0; x < cols; x++) {
            float t = (x + 0.5f) * scale / cols, sum = 0;
            for (float fi : f) {
                float u = (t - fi) / hb;
                if (u > -1 && u < 1) sum += 0.9375f * (1 - u * u) * (1 - u * u);
            }
            s.v[x] = sum / (f.size() * hb) * pg.bin * 100;
        }
    } else {                // how far runs from start get
        int n = 0;
        for (const PEntry *e : s.e) if (p_from_start(*e) && e->tt >= 0) n++;
        if (!n) return;
        s.any = true;
        for (int x = 0; x < cols; x++) {
            float t = (x + 0.5f) * scale / cols;
            int past = 0;
            for (const PEntry *e : s.e) if (p_from_start(*e) && e->tt >= 0 && p_reach(*e) > t) past++;
            s.v[x] = 100.0f * past / n;
        }
    }
}

// the graph's step and top: 4 lines of a round step
static void p_ymax(float m, float &step, float &top)
{
    if (g_pg_mode == 1) { step = 25; top = 100; return; }
    static const float steps[] = {1, 2, 5, 10, 20, 25, 50};
    step = 50;
    for (float s : steps) if (4 * s >= m * 1.08f) { step = s; break; }
    top = 4 * step;
}

static int p_y(float v, float top, int ph) { return ph - 1 - (int)(std::min(v, top) / top * (ph - 1) + 0.5f); }

// drawn a pixel at a time in the widgets' pixels (dithered fills, a line a pixel thick), into a texture when
// anything in it changes
static void p_chart_draw(PSeries *ser, int ns, int pw, int ph, float scale, float top, float step, int tick)
{
    PCanvas c(pw, ph);
    const Col grid = {120, 95, 60, 150}, axis = {63, 48, 30, 255};
    for (const PSpot &s : pg.spots) {
        if (p_is_end(s) || s.lo > scale) continue;
        int x0 = (int)(s.lo / scale * pw), x1 = std::max(x0 + 1, (int)(p_spot_hi(s) / scale * pw));
        for (int x = x0; x < x1; x++) for (int y = 0; y < ph; y++) c.put(x, y, M_BAND);
    }
    for (float v = step; v <= top + 0.01f; v += step) {
        int y = p_y(v, top, ph);
        for (int x = 0; x < pw; x += 2) c.put(x, y, grid);
    }
    for (int t = tick; t < scale; t += tick) {
        int x = (int)(t / scale * pw);
        for (int y = 0; y < ph; y += 2) c.put(x, y, grid);
    }
    for (int k = ns - 1; k >= 0; k--) {   // the compared day under, the main day on top
        PSeries &s = ser[k];
        if (!s.any) continue;
        std::vector<int> py(pw);
        for (int x = 0; x < pw; x++) py[x] = p_y(s.v[x], top, ph);
        for (int x = 0; x < pw; x++)
            for (int y = py[x] + 1; y < ph - 1; y++)
                if (((x + y + k) & 1) == 0) c.put(x, y, s.fill);
        for (int x = 0; x < pw; x++) {
            int a = x ? std::min(py[x - 1], py[x]) : py[x], b = x ? std::max(py[x - 1], py[x]) : py[x];
            for (int y = a; y <= b; y++) c.put(x, y, s.line);
        }
    }
    for (int x = 0; x < pw; x++) c.put(x, ph - 1, axis);
    for (int y = 0; y < ph; y++) c.put(0, y, axis);
    int k = g_book_t;
    p_upload(g_pchart.tex, g_pchart.tw, g_pchart.th, pw * k, ph * k, [&](std::vector<unsigned char> &buf, int stride) {
        PImg im;
        im.w = pw;
        im.h = ph;
        im.px = c.px;
        p_blit(buf, stride, im, 0, 0, k);
    });
}

static void p_page_graph(int x, int y, int w, int h)
{
    const int bottom = y + h, k = g_book_t;
    p_aggregate();
    int nd = (int)g_pdays.size();
    p_head(x, y, w, "Graph", 2);
    y += BT(14);
    // what it shows
    int cx = x;
    static const char *const modes[2] = {"where runs end", "how far runs get"};
    for (int m = 0; m < 2; m++) {
        bool on = g_pg_mode == m;
        if (p_link(PB_MODE0 + m, cx, y, modes[m], true, on ? F_MCB : F_MC, on ? M_INK : M_DIM)) g_pg_mode = m;
        if (on) rect(cx, y + BT(8), mt_w(F_MCB, modes[m]), BT(1), M_INK);
        cx += mt_w(on ? F_MCB : F_MC, modes[m]) + BT(10);
    }
    y += BT(14);
    // the days: a, and b to compare (or none); "all days" after the last
    if (g_pg_a < 0 || g_pg_a > nd) g_pg_a = std::max(0, nd - 1);
    if (g_pg_b < -1 || g_pg_b > nd) g_pg_b = -1;
    struct { int *idx; int prev, next, lo; Col line; const char *lead; } rows[2] = {
        {&g_pg_a, PB_A_PREV, PB_A_NEXT, 0, M_LINE_A, ""}, {&g_pg_b, PB_B_PREV, PB_B_NEXT, -1, M_LINE_B, "vs "}};
    for (auto &r : rows) {
        rect(x, y + BT(1), BT(6), BT(6), r.line);
        cx = x + BT(10);
        cx += mt(F_MC, cx, y, M_DIM, r.lead);
        if (p_link(r.prev, cx, y, "<", *r.idx > r.lo)) (*r.idx)--;
        cx += mt_w(F_MCB, "<") + BT(5);
        cx += mt(F_MC, cx, y, *r.idx < 0 ? M_FAINT : M_INK, *r.idx < 0 ? "none" : p_series_label(*r.idx)) + BT(5);
        if (p_link(r.next, cx, y, ">", *r.idx < nd)) (*r.idx)++;
        y += BT(12);
    }
    y += BT(4);

    // the curves
    float scale = p_scale();
    PSeries ser[2];
    int ns = 0;
    for (int i = 0; i < 2; i++) {
        int idx = i ? g_pg_b : g_pg_a;
        if (idx < 0 || !nd) continue;
        PSeries &s = ser[ns++];
        s.label = p_series_label(idx);
        for (const PEntry *e : p_day_entries(idx >= nd ? "" : p_series_day(idx)))
            if (p_from_start(*e)) s.e.push_back(e);
        s.st = p_stats(s.e);
        s.line = i ? M_LINE_B : M_LINE_A;
        s.fill = i ? M_FILL_B : M_FILL_A;
    }
    int gut = BT(26), px0 = x + gut, pw = (w - gut) / k, legend = BT(12) * std::max(ns, 1) + BT(4);
    int py0 = y + BT(11), ph = (bottom - BT(12) - legend - py0) / k;
    if (pw < 20 || ph < 20) return;
    for (int i = 0; i < ns; i++) p_curve(ser[i], pw, scale);
    float m = 0, step, top;
    for (int i = 0; i < ns; i++) for (float v : ser[i].v) m = std::max(m, v);
    p_ymax(m, step, top);
    int tick = 15;
    for (int t : {15, 30, 60, 120, 300}) { tick = t; if (t / scale * pw >= 34) break; }
    char key[160];
    snprintf(key, sizeof key, "%d %d %d %d %d %d %d %d %.2f %.2f", pg.version, g_pg_mode, g_pg_a, g_pg_b, pw, ph, k, pg.bin, scale, top);
    if (key != g_pchart.key) {
        p_chart_draw(ser, ns, pw, ph, scale, top, step, tick);
        g_pchart.key = key;
    }
    PTrack tr = {px0, pw * k, scale};
    p_spot_scale(tr, y, py0, py0, false);   // (the bands are in the graph)
    // the graph, its numbers up the side and its times along the bottom
    g_surf->DrawSetColor(255, 255, 255, 255);
    g_surf->DrawSetTexture(g_pchart.tex);
    g_surf->DrawTexturedSubRect(px0, py0, px0 + pw * k, py0 + ph * k, 0, 0, (float)(pw * k) / g_pchart.tw, (float)(ph * k) / g_pchart.th);
    for (float v = 0; v <= top + 0.01f; v += step) {
        char b[16];
        snprintf(b, sizeof b, "%.0f%%", v);
        p_text_r(F_MC, px0 - BT(3), py0 + p_y(v, top, ph) * k - BT(4), M_DIM, b);
    }
    int ly = py0 + ph * k + BT(3), lastx = -10000;
    for (int t = 0; t <= (int)scale; t += tick) {
        std::string s = p_clock((float)t);
        int tx = px0 + (int)(t / scale * pw) * k - mt_w(F_MC, s) / 2;
        tx = std::min(std::max(tx, px0), px0 + pw * k - mt_w(F_MC, s));
        if (tx < lastx) continue;
        mt(F_MC, tx, ly, M_DIM, s);
        lastx = tx + mt_w(F_MC, s) + BT(6);
    }
    if (!ns || (!ser[0].any && (ns < 2 || !ser[1].any))) {
        bool runs = false;
        for (int i = 0; i < ns; i++) runs |= !ser[i].e.empty();
        std::string t = !nd ? "nothing logged on this map yet" : g_pg_mode == 0 && runs ? "no fails" : "no runs from start";
        mt(F_MC, px0 + (pw * k - mt_w(F_MC, t)) / 2, py0 + ph * k / 2 - BT(4), M_FAINT, t);
    }
    // the mouse on it: a line down, the curves' points, the numbers there
    if (hover(px0, py0, pw * k, ph * k) && ns) {
        int col = std::min(std::max((g_mx - px0) / k, 0), pw - 1);
        float t = (col + 0.5f) * scale / pw;
        rect(px0 + col * k, py0, k, ph * k, {0, 0, 0, 110});
        PRegion r = p_region(t);
        p_tip(p_clock(t) + (r.spot ? "  \xC2\xB7  " + r.name : ""));
        for (int i = 0; i < ns; i++) {
            PSeries &s = ser[i];
            if (!s.any) continue;
            int vy = py0 + p_y(s.v[col], top, ph) * k;
            rect(px0 + col * k - BT(2), vy - BT(2), BT(5), BT(5), M_INK);
            rect(px0 + col * k - BT(1), vy - BT(1), BT(3), BT(3), s.fill);
            char b[96];
            if (g_pg_mode == 0) {
                int n = 0;
                for (const PEntry *e : s.e) if (p_fail(*e) && e->tt >= r.lo && e->tt <= r.hi) n++;
                snprintf(b, sizeof b, "%s: %d of %d fails (%.0f%%) at %s", s.label.c_str(), n, s.st.fails, s.st.fails ? 100.0f * n / s.st.fails : 0.0f, r.name.c_str());
            } else {
                int past = 0, n = 0;
                for (const PEntry *e : s.e) if (p_from_start(*e) && e->tt >= 0) { n++; if (p_reach(*e) > t) past++; }
                snprintf(b, sizeof b, "%s: %d of %d runs got past (%.0f%%)", s.label.c_str(), past, n, n ? 100.0f * past / n : 0.0f);
            }
            p_tip(b, i ? M_FILL_B : Col{120, 120, 255, 255});
        }
    }
    // the legend: each day's runs, best, and fails
    y = ly + BT(14);
    for (int i = 0; i < ns; i++) {
        PSeries &s = ser[i];
        rect(x, y + BT(1), BT(6), BT(6), s.line);
        cx = x + BT(10);
        cx += mt(F_MCB, cx, y, M_INK, s.label) + BT(6);
        char b[96];
        if (g_pg_mode == 0) snprintf(b, sizeof b, "%d %s \xC2\xB7 best %s \xC2\xB7 %d fails", s.st.starts, s.st.starts == 1 ? "run" : "runs", p_clock(s.st.best).c_str(), s.st.fails);
        else snprintf(b, sizeof b, "%d %s \xC2\xB7 best %s", s.st.starts, s.st.starts == 1 ? "run" : "runs", p_clock(s.st.best).c_str());
        mt(F_MC, cx, y, M_DIM, b, x + w - cx);
        y += BT(12);
    }
}

// ---- page 3: stats, today and all time
static void p_page_stats(int x, int y, int w, int h)
{
    const int bottom = y + h;
    p_head(x, y, w, "Stats", 3);
    y += BT(18);
    PStats td = p_stats(p_day_entries(pg.today)), all = p_stats(p_day_entries(""));
    int c1 = x + BT(84), c2 = x + BT(84) + (w - BT(84)) / 2, cw = (w - BT(84)) / 2 - BT(4);
    mt(F_MCB, c1, y, M_DIM, "today");
    mt(F_MCB, c2, y, M_DIM, "all time");
    y += BT(13);
    struct Cell { std::string s; Col c; std::string tip; };
    auto region = [](const PRegion &r, int n, int of) -> Cell {
        if (!n) return {"-", M_FAINT, ""};
        char b[64];
        if (of) snprintf(b, sizeof b, " %.0f%%", 100.0f * n / of);
        else snprintf(b, sizeof b, " \xC3\x97%d", n);
        char t[96];
        if (of) snprintf(t, sizeof t, "%d of %d that got there died there", n, of);
        else snprintf(t, sizeof t, "%d fails", n);
        return {r.name + b, M_RED, (r.spot ? p_clock(r.lo) + " \xE2\x80\x93 " + p_clock(r.hi) + "  \xC2\xB7  " : std::string()) + t};
    };
    auto num = [](int n, Col c = M_INK) -> Cell { return {std::to_string(n), n ? c : M_FAINT, ""}; };
    auto clk = [](float t, Col c = M_INK) -> Cell { return {p_clock(t), t >= 0 ? c : M_FAINT, ""}; };
    struct Row { const char *label; Cell a, b; } rows[] = {
        {"attempts", num(td.tries), num(all.tries)},
        {"best from start", clk(td.best, M_BLUE), clk(all.best, M_BLUE)},
        {"avg from start", clk(td.avg), clk(all.avg)},
        {"most failed", region(td.where.empty() ? PRegion() : td.where[0].first, td.where.empty() ? 0 : td.where[0].second, 0),
         region(all.where.empty() ? PRegion() : all.where[0].first, all.where.empty() ? 0 : all.where[0].second, 0)},
        {"deadliest", region(td.dead, td.dead_n, td.dead_of), region(all.dead, all.dead_n, all.dead_of)},
        {"most practised", {td.prac_n ? "from " + td.prac + " \xC3\x97" + std::to_string(td.prac_n) : "-", td.prac_n ? M_INK : M_FAINT, ""},
         {all.prac_n ? "from " + all.prac + " \xC3\x97" + std::to_string(all.prac_n) : "-", all.prac_n ? M_INK : M_FAINT, ""}},
        {"time practising", {p_span(td.secs), td.secs ? M_INK : M_FAINT, ""}, {p_span(all.secs), all.secs ? M_INK : M_FAINT, ""}},
        {"days practised", {"", M_FAINT, ""}, num(all.days)},
    };
    int rh = BT(13);
    for (const Row &r : rows) {
        mt(F_MC, x, y, M_DIM, r.label, BT(82));
        const Cell *cells[2] = {&r.a, &r.b};
        int xs[2] = {c1, c2};
        for (int i = 0; i < 2; i++) {
            mt(F_MC, xs[i], y, cells[i]->c, cells[i]->s, cw);
            if (!cells[i]->tip.empty() && hover(xs[i], y - BT(2), cw, rh)) p_tip(cells[i]->s, M_WHITE), p_tip(cells[i]->tip, M_GRAY);
        }
        for (int dx = 0; dx < w; dx += BT(2)) rect(x + dx, y + rh - BT(3), BT(1), BT(1), {200, 185, 160, 255});
        y += rh;
    }
    // where runs from Start die, all time: the places, most fails first, with today's beside
    y += BT(8);
    mt(F_MCB, x, y, M_INK, "Fails by place");
    mt(F_MCB, c1 + BT(56), y, M_DIM, "today");
    p_text_r(F_MCB, x + w, y, M_DIM, "all");
    y += BT(13);
    int maxn = all.where.empty() ? 1 : all.where[0].second;
    for (auto &wp : all.where) {
        if (y + rh > bottom) break;
        int tn = 0;
        for (auto &t : td.where) if (t.first.name == wp.first.name) tn = t.second;
        mt(F_MC, x, y, wp.first.spot ? M_INK : M_DIM, wp.first.name, BT(82));
        int bw = (c1 + BT(50) - c1) * wp.second / std::max(maxn, 1);
        rect(c1, y + BT(1), std::max(BT(1), bw), BT(6), M_RED);
        p_text_r(F_MC, c1 + BT(76), y, tn ? M_INK : M_FAINT, std::to_string(tn));
        p_text_r(F_MC, x + w, y, M_INK, std::to_string(wp.second));
        if (hover(x, y - BT(2), w, rh) && wp.first.spot) p_tip(wp.first.name), p_tip(p_clock(wp.first.lo) + " \xE2\x80\x93 " + p_clock(wp.first.hi), M_GRAY);
        y += BT(11);
    }
    if (all.where.empty()) mt(F_MC, x, y, M_FAINT, "no fails logged yet");
}

// ---- page 4: spots (a name for where you get to a place: a range of times)
static void p_page_spots(int x, int y, int w, int h)
{
    const int bottom = y + h;
    p_head(x, y, w, "Spots", 4);
    y += BT(16);
    int nw = BT(92), tw = BT(38), cx = x;
    mt(F_MC, cx, y, M_DIM, "name");
    mt(F_MC, cx + nw + BT(6), y, M_DIM, "from");
    mt(F_MC, cx + nw + tw + BT(12), y, M_DIM, "to");
    y += BT(12);
    bool editing = !g_pspot_edit.empty();
    int iy = y + BT(6);
    bool go = p_input(PI_SPOT_NAME, g_pspot_name, 31, cx, iy, nw, "end slope");
    go |= p_input(PI_SPOT_LO, g_pspot_lo, 15, cx + nw + BT(6), iy, tw, "3:40");
    go |= p_input(PI_SPOT_HI, g_pspot_hi, 15, cx + nw + tw + BT(12), iy, tw, "3:55");
    p_tabs({PI_SPOT_NAME, PI_SPOT_LO, PI_SPOT_HI});
    int bx = cx + nw + 2 * tw + BT(20);
    go |= p_button(PB_SPOT_ADD, bx, y, editing ? 34 : 40, editing ? "Save" : "Add");
    if (editing && p_button(PB_SPOT_CANCEL, bx + BT(36), y, 40, "Cancel")) {
        g_pspot_edit.clear(); g_pspot_name.clear(); g_pspot_lo.clear(); g_pspot_hi.clear(); g_focus = 0;
    }
    if (go) {
        std::string n = p_clean(g_pspot_name), lo = p_clean(g_pspot_lo), hi = p_clean(g_pspot_hi);
        if (!n.empty() && !lo.empty()) {
            if (editing && strcasecmp(n.c_str(), g_pspot_edit.c_str())) p_send("sm_progui spotdel \"%s\"", p_clean(g_pspot_edit).c_str());
            p_send("sm_progui spot \"%s\" \"%s\" \"%s\"", n.c_str(), lo.c_str(), hi.c_str());
            g_pspot_edit.clear(); g_pspot_name.clear(); g_pspot_lo.clear(); g_pspot_hi.clear();
            p_focus(PI_SPOT_NAME);
        } else p_focus(n.empty() ? PI_SPOT_NAME : PI_SPOT_LO);
    }
    y += BT(28);
    // the spots, each with its range on the track and the fails in it (runs from Start)
    std::vector<const PSpot *> list;
    for (const PSpot &s : pg.spots) if (!p_is_end(s)) list.push_back(&s);
    float scale = p_scale();
    int rh = BT(24), count = (int)list.size(), visible = std::max(1, (bottom - y) / rh);
    p_list_keys(PR_SPOT, count, g_pscroll_spots, visible);
    if (g_wheel && hover(x, y, w, bottom - y)) { g_pscroll_spots -= g_wheel; g_wheel = 0; }
    g_pscroll_spots = std::min(std::max(g_pscroll_spots, 0), std::max(0, count - visible));
    for (int r = 0; r < visible && g_pscroll_spots + r < count; r++) {
        int i = g_pscroll_spots + r, ry = y + r * rh, id = PR_SPOT + i;
        const PSpot &s = *list[i];
        bool kb = g_focus == id, hv = hover(x - BT(2), ry, w + BT(4), rh - BT(2)), ed = !strcasecmp(s.name.c_str(), g_pspot_edit.c_str());
        p_nav(id, x, ry, w, rh - BT(2));
        if (ed) rect(x - BT(2), ry, w + BT(4), rh - BT(2), M_SEL);
        else if (hv || kb) rect(x - BT(2), ry, w + BT(4), rh - BT(2), M_HOVER);
        int n = 0;
        for (const PEntry &e : pg.e) if (p_fail(e) && e.tt >= s.lo - (s.hi > s.lo ? 0 : pg.bin * 0.5f) && e.tt <= p_spot_hi(s) + (s.hi > s.lo ? 0 : pg.bin * 0.5f)) n++;
        int nx = x + mt(F_MCB, x, ry + BT(2), ed ? M_BLUE : M_INK, p_lower(s.name), BT(100)) + BT(8);
        mt(F_MC, nx, ry + BT(2), M_DIM, p_spot_range(s));
        bool del_room = hv || kb;
        int dw = del_room ? mt_w(F_MCB, "\xC3\x97") + BT(8) : 0;
        p_text_r(F_MC, x + w - dw, ry + BT(2), n ? M_RED : M_FAINT, std::to_string(n) + (n == 1 ? " fail" : " fails"));
        p_xp(x, ry + BT(13), scale, s.lo, std::max(p_spot_hi(s), s.lo + scale / 182), PK_START);
        bool del = del_room && p_link(PB_SPOT_DEL, x + w - mt_w(F_MCB, "\xC3\x97"), ry + BT(2), "\xC3\x97", true, F_MCB, M_RED);
        if (kb && in.del) { in.del = false; del = true; }
        if (del) {
            std::string nm = s.name;
            p_ask("Delete this spot?", p_lower(nm) + "  " + p_spot_range(s), [nm] { p_send("sm_progui spotdel \"%s\"", p_clean(nm).c_str()); });
            break;
        }
        if (take_click(x - BT(2), ry, w + BT(4), rh - BT(2)) || (kb && in.enter)) {   // into the form, to change it
            in.enter = false;
            g_pspot_edit = s.name;
            g_pspot_name = s.name;
            g_pspot_lo = p_clock(s.lo);
            g_pspot_hi = s.hi > s.lo ? p_clock(s.hi) : "";
            p_focus(PI_SPOT_LO);
        }
    }
    if (!count) mt(F_MC, x, y + BT(4), M_FAINT, "no spots yet");
}

// ---- page 5: settings
static void p_page_settings(int x, int y, int w, int h)
{
    p_head(x, y, w, "Settings", 5);
    y += BT(20);
    int lx = x, cx = x + BT(112), rh = BT(28), ty = BT(6);
    mt(F_MC, lx, y + ty, M_DIM, "estimated finish");
    bool go = p_input(PI_END2, g_pend2, 15, cx, y + ty, BT(40), pg.end > 0 ? p_clock(pg.end).c_str() : "4:10");
    go |= p_button(PB_END2, cx + BT(48), y, 40, "Save");
    if (go) {
        std::string t = p_clean(g_pend2);
        if (!t.empty()) { p_send("sm_progui spot \"End\" \"%s\"", t.c_str()); g_pend2.clear(); g_focus = 0; }
        else p_focus(PI_END2);
    }
    y += rh;
    mt(F_MC, lx, y + ty, M_DIM, "log finished runs");
    if (p_button(PB_AUTO_YES, cx, y, 40, "Yes", true, pg.autofinish ? 1 : 0) && !pg.autofinish) { pg.autofinish = true; p_send("sm_progui set autofinish 1"); }
    if (p_button(PB_AUTO_NO, cx + BT(42), y, 40, "No", true, pg.autofinish ? 0 : 1) && pg.autofinish) { pg.autofinish = false; p_send("sm_progui set autofinish 0"); }
    y += rh;
    mt(F_MC, lx, y + ty, M_DIM, "progsave ends at");
    if (p_button(PB_SAVEAT_FAIL, cx, y, 70, "last fail", true, pg.saveat_here ? 0 : 1) && pg.saveat_here) { pg.saveat_here = false; p_send("sm_progui set saveat 0"); }
    if (p_button(PB_SAVEAT_HERE, cx + BT(72), y, 70, "where I am", true, pg.saveat_here ? 1 : 0) && !pg.saveat_here) { pg.saveat_here = true; p_send("sm_progui set saveat 1"); }
    y += rh;
    mt(F_MC, lx, y + ty, M_DIM, "fail range");
    for (int i = 0; i < 5; i++) {
        char b[8];
        snprintf(b, sizeof b, "%ds", P_BINS[i]);
        if (p_button(PB_BIN + i, cx + BT(i * 30), y, 28, b, true, pg.bin == P_BINS[i] ? 1 : 0) && pg.bin != P_BINS[i]) {
            pg.bin = P_BINS[i];
            pg.version++;
            p_send("sm_progui set bin %d", pg.bin);
        }
    }
    y += rh;
    char d[32];
    snprintf(d, sizeof d, "%d:00", pg.daystart);
    mt(F_MC, lx, y + ty, M_DIM, "new day at");
    mt(F_MC, cx, y + ty, M_INK, d);
    y += BT(18);
    const char *key = g_engine ? g_engine->Key_LookupBinding("sm_progsave") : nullptr;
    mt(F_MC, lx, y + ty, M_DIM, "progsave key");
    mt(F_MC, cx, y + ty, key && *key ? M_INK : M_FAINT, key && *key ? p_lower(key) : "none (bind one to sm_progsave)");
}

// Minecraft's page arrow, highlighted under the mouse or keyboard; turns the page
static bool p_arrow(int id, int x, int y, bool fwd)
{
    int w = BF(23), h = BF(13);
    p_nav(id, x, y, w, h);
    bool hv = hover(x, y, w, h) || g_focus == id;
    if (g_part) p_pic(g_pbook, fwd ? (hv ? PS_FWD_HL : PS_FWD) : (hv ? PS_BACK_HL : PS_BACK), x, y);
    else p_stext(F_MCB2, x + BF(6), y, hv ? M_BLUE : M_INK, fwd ? ">" : "<");
    bool go = take_click(x, y, w, h) || (g_focus == id && in.enter);
    if (go && g_focus == id) in.enter = false;
    return go;
}

// the recipe book's tabs on the book's right edge (under it), with an item each: book and quill, map, compass
static void p_book_tabs(int bx, int by)
{
    static const int icon[PV_COUNT] = {PS_ICON_BOOK, PS_ICON_MAP, PS_ICON_COMPASS};
    static const char *const name[PV_COUNT] = {"Log", "Graph \xC2\xB7 Stats", "Spots \xC2\xB7 Settings"};
    int q = p_tab_scale();
    for (int i = 0; i < PV_COUNT; i++) {
        bool on = g_pview == i;
        int x = bx - q * (on ? 6 : 8), y = by + BF(14) + i * q * 29, w = q * 35, h = q * 27, vx = bx;
        p_nav(PB_TAB + i, vx, y, x + w - vx, h);
        bool hv = hover(vx, y, x + w - vx, h) || g_focus == PB_TAB + i;
        if (g_part) {
            p_pic(g_ptabs, on ? PS_TAB_ON : PS_TAB, x, y);
            p_pic(g_ptabs, icon[i], x + q * 13, y + q * 5);
        } else {
            rect(vx, y, x + w - vx - q * 5, h, on ? Col{198, 198, 198, 255} : Col{140, 140, 140, 255});
            mt(F_MCB, vx + q * 6, y + q * 9, M_INK, std::to_string(i ? i * 2 : 1));
        }
        if (g_focus == PB_TAB + i) {
            rect(vx, y + q * 2, x + w - vx - q * 7, h - q * 4, {255, 255, 255, 70});
            p_tip_at(x + w - q * 8, y + h / 2);
        }
        if (hv) p_tip(name[i]);
        if (take_click(vx, y, x + w - vx, h) || (g_focus == PB_TAB + i && in.enter)) { in.enter = false; p_view(i); }
    }
}

static void p_plain_page(int x, int y, int w, int h)   // the book without Minecraft's pictures
{
    rect(x, y, w, h, {92, 46, 26, 255});
    rect(x + BF(4), y + BF(4), w - BF(8), h - BF(8), {250, 244, 226, 255});
}

// the whole window in Progress Log mode
static void prog_window(int sw, int sh)
{
    p_load_assets();
    const int F = g_book_f;
    if (g_passets == 1) {
        p_build(g_pbook, {PS_BOOK, PS_BOOK_M, PS_FWD, PS_FWD_HL, PS_BACK, PS_BACK_HL}, F);
        p_build(g_ptabs, {PS_TAB, PS_TAB_ON, PS_ICON_BOOK, PS_ICON_MAP, PS_ICON_COMPASS}, p_tab_scale());
        p_build(g_pui, {PS_BUTTON, PS_BUTTON_HL, PS_BUTTON_OFF, PS_SCROLLER, PS_SCROLLER_BG, PS_TOOLTIP, PS_POPUP, PS_XP_BG,
                        PS_XP_GREEN, PS_XP_BLUE, PS_XP_RED, PS_XP_RAINBOW}, g_book_t);
        if (!g_pglyphs.empty()) p_build(g_pfont, {PS_FONT_ASCII, PS_FONT_NONLATIN}, g_book_t);
    }
    g_part = g_passets == 1 && g_pbook.tex >= 0 && g_pui.tex >= 0 && g_ptabs.tex >= 0;
    g_pnav_last.swap(g_pnav);
    g_pnav.clear();
    g_ptip.clear();
    g_ptip_x = -1;
    dropdown_input();

    // a question up: the book under it gets neither the mouse nor the keys
    int mx = g_mx, my = g_my;
    bool click = g_click;
    auto keep = in;
    if (g_pask.open) {
        g_mx = g_my = -100000;
        g_click = false;
        in.nav_x = in.nav_y = in.flip = in.tabs = in.backspaces = 0;
        in.del = in.enter = in.paste = false;
        in.typed.clear();
    } else {
        // the keys: PgUp / PgDn turn pages, so do Left / Right with nothing picked; arrows go between controls
        // (a list row's Up / Down: its list takes them, rows scrolled out of sight included)
        if (in.flip) p_view(g_pview + (in.flip > 0 ? 1 : -1));
        bool known = g_focus >= PR_ENTRY;
        for (const PNav &n : g_pnav_last) if (n.id == g_focus) known = true;
        if (!g_focus || !known) {
            if (in.nav_x) p_view(g_pview + (in.nav_x > 0 ? 1 : -1));
            else if (in.nav_y) p_nav_first(in.nav_y < 0);
        } else if (in.nav_x) p_nav_go(in.nav_x > 0 ? 1 : -1, 0);
        else if (in.nav_y && g_focus < PR_ENTRY) p_nav_go(0, in.nav_y > 0 ? 1 : -1);
        if (g_focus < PR_ENTRY) in.nav_y = 0;
        in.nav_x = 0;
    }

    rect(0, 0, sw, sh, {16, 16, 16, 176});   // the world behind dimmed, as behind the game's screens
    int pages = g_pview == PV_LOG ? 1 : 2, pw = BF(146);
    int bw = pw * pages, bh = BF(180), gap = BT(8), done_h = BT(20);
    int x0 = (sw - bw) / 2, y0 = std::max(0, (sh - bh - gap - done_h) / 2);
    g_psplit = pages == 2 ? x0 + pw : 1 << 30;
    p_book_tabs(x0 + bw, y0);
    if (g_part) {
        if (pages == 1) p_pic(g_pbook, PS_BOOK, x0, y0);
        else { p_pic(g_pbook, PS_BOOK_M, x0, y0); p_pic(g_pbook, PS_BOOK, x0 + pw, y0); }
    } else for (int p = 0; p < pages; p++) p_plain_page(x0 + p * pw, y0, pw, bh);

    // the writing: inside the paper, above the page arrows (book pixels 14 .. 132 across, 12 .. 154 down)
    int ax = BF(14), ay = BF(12), aw = BF(118), ah = BF(142);
    if (!pg.have) {
        std::string t = "...";
        mt(F_MC, x0 + (bw - mt_w(F_MC, t)) / 2, y0 + bh / 2, M_FAINT, t);
    } else switch (g_pview) {
        case PV_LOG: p_page_log(x0 + ax, y0 + ay, aw, ah); break;
        case PV_GRAPH: p_page_graph(x0 + ax, y0 + ay, aw, ah); p_page_stats(x0 + pw + ax, y0 + ay, aw, ah); break;
        case PV_SPOTS: p_page_spots(x0 + ax, y0 + ay, aw, ah); p_page_settings(x0 + pw + ax, y0 + ay, aw, ah); break;
    }
    if (!pg.synced) {
        bool blink = (long)(plat_now() * 3) % 2 == 0;
        rect(x0 + bw - BF(10), y0 + BF(5), BT(2), BT(2), blink ? M_BLUE : M_FAINT);
    }
    // the page arrows where the game has them (back on the left page, on along the right)
    int ay2 = y0 + BF(156);
    if (g_pview > 0 && p_arrow(PB_BACK, x0 + BF(146 - 96 - 23), ay2, false)) p_view(g_pview - 1);
    if (g_pview < PV_COUNT - 1 && p_arrow(PB_FWD, x0 + bw - pw + BF(96), ay2, true)) p_view(g_pview + 1);
    if (p_button(PB_DONE, (sw - BT(200)) / 2, y0 + bh + gap, 200, "Done")) { close_window(); return; }
    if (!g_toast.empty() && plat_now() - g_toast_at < 3) {
        int tw = mt_w(F_MC, g_toast), tx = (sw - tw) / 2, ty = std::max(BT(14), y0 - BT(18));
        if (g_part) p_nine(g_pui, PS_TOOLTIP, 10, tx - BT(12), ty - BT(12), tw + BT(24), BT(8) + BT(24));
        p_stext(F_MC, tx, ty, M_WHITE, g_toast);
    }

    if (g_pask.open) {
        g_mx = mx;
        g_my = my;
        g_click = click;
        in.nav_x = keep.nav_x;
        in.enter = keep.enter;
        p_draw_ask(sw, sh);
        in.nav_x = 0;
        in.enter = false;
    } else p_draw_tip(sw, sh);
    dropdown_draw();
}
