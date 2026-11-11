// cstvd — CS:TV host helper. Streams a URL with libmpv (yt-dlp for most sites, streamlink with ads
// removed for Twitch channels), renders frames in software at 1920x1080, compresses them to DXT1 and
// publishes them in shared memory for the game-side client. Audio goes straight to the sound system.
// Nothing is written to disk: mpv's cache is RAM-only and yt-dlp runs with --no-cache-dir.
// Protocol and shared-memory layout: ../common/cstv_proto.h. Linux and Windows (MinGW-w64): what
// differs between them is in cstv_os.h.
#define _GNU_SOURCE
#ifdef _WIN32
#define _WIN32_WINNT 0x0A00
#endif
#include "cstv_proto.h"
#include <errno.h>
#include "cstv_os.h"   // first of the system headers on Windows: winsock2.h must come before windows.h
#include <ctype.h>
#include <locale.h>
#include <math.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/prctl.h>
#endif

// yt-dlp format for a height cap; see yt_format_for()
#define YT_FORMAT(h) "bv*[height<=" #h "][protocol=https][format_id!*=-sr]+ba[protocol=https]/bv*[height<=" #h "]+ba/b[height<=" #h "]/bv*+ba/b"

#define exe_dir os_exe_dir   // where cstvd lives (cstv_overlay.py and cstv_ytdl.py sit next to it)

// the programs cstvd runs: Linux finds them in PATH, the Windows build carries them next to cstvd.exe
#ifdef _WIN32
#define PY_REL         "python/python.exe"
#define YTDLP_REL      "yt-dlp.exe"
#define STREAMLINK_REL "streamlink/bin/streamlink.exe"
#else
#define PY_REL         NULL
#define YTDLP_REL      NULL
#define STREAMLINK_REL NULL
#endif
static const char *tool(const char *name, const char *win_rel, char *buf, size_t n)
{
#ifdef _WIN32
    (void)name;
    os_exe_path(buf, n, win_rel);
    return buf;
#else
    (void)win_rel; (void)buf; (void)n;
    return name;
#endif
}
static void overlay_start(void);
static int is_twitch_channel_l(const char *u, char *login, size_t n);
static int is_yt_live_url(const char *u);
#include "dxt1_fast.h"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#define ENC_THREADS 4

static void logf_(const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(line, sizeof line, "cstvd: ");
    vsnprintf(line + n, sizeof line - n - 1, fmt, ap);
    va_end(ap);
    strcat(line, "\n");
    os_log_line(line);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

// ------------------------------------------------------------------ shared memory
static struct cstv_shm *shm;

static void set_status(enum cstv_state st, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(shm->status, sizeof shm->status, fmt, ap);
    va_end(ap);
    shm->state = st;
    logf_("%s", shm->status);
}

static int open_shm(void)
{
    shm = os_shm_create(CSTV_SHM_PATH, sizeof *shm);
    if (!shm) { logf_("cannot create %s: %s", CSTV_SHM_PATH, strerror(errno)); return -1; }
    memset(shm, 0, offsetof(struct cstv_shm, buf));
    shm->magic = CSTV_MAGIC;
    shm->version = CSTV_VERSION;
    shm->w = CSTV_W;
    shm->h = CSTV_H;
    shm->format = CSTV_FMT_DXT1;
    shm->frame_bytes = CSTV_FRAME_BYTES;
    shm->latest = CSTV_NONE;
    shm->reading = CSTV_NONE;
    return 0;
}

// ------------------------------------------------------------------ DXT1 encode
struct enc_job { const uint8_t *rgba; uint8_t *out; int t; };

// The encoder loop twice: built for AVX2 (the compiler vectorises it, ~2x faster) and for any x86-64 CPU.
// The Linux build targets x86-64-v3 anyway; the Windows build runs on CPUs we don't know, so it picks
// at startup (use_avx2) instead of crashing on an AVX2 instruction.
#define ENC_ROWS(j)                                                                                  \
    do {                                                                                             \
        int bw = CSTV_W / 4, bh = CSTV_H / 4;                                                        \
        for (int by = (j)->t; by < bh; by += ENC_THREADS)                                            \
            for (int bx = 0; bx < bw; bx++)                                                          \
                dxt1_fast_block((j)->out + ((size_t)by * bw + bx) * 8,                               \
                                (j)->rgba + ((size_t)by * 4 * CSTV_W + bx * 4) * 4, (size_t)CSTV_W * 4); \
    } while (0)

#if defined(__x86_64__) && defined(__GNUC__)
__attribute__((target("avx2,fma,bmi2"))) static void enc_rows_avx2(struct enc_job *j) { ENC_ROWS(j); }
#define HAVE_ENC_AVX2 1
#endif
static void enc_rows_base(struct enc_job *j) { ENC_ROWS(j); }
static int use_avx2;

static void *enc_worker(void *p)
{
#ifdef HAVE_ENC_AVX2
    if (use_avx2) { enc_rows_avx2(p); return NULL; }
#endif
    enc_rows_base(p);
    return NULL;
}

static void publish_frame(const uint8_t *rgba)
{
    uint32_t latest = shm->latest, reading = shm->reading, w = 0;
    while (w == latest || w == reading) w++;   // a buffer nobody is looking at
    pthread_t th[ENC_THREADS];
    struct enc_job jobs[ENC_THREADS];
    for (int t = 0; t < ENC_THREADS; t++) {
        jobs[t] = (struct enc_job){rgba, shm->buf[w], t};
        pthread_create(&th[t], NULL, enc_worker, &jobs[t]);
    }
    for (int t = 0; t < ENC_THREADS; t++) pthread_join(th[t], NULL);
    __atomic_store_n(&shm->latest, w, __ATOMIC_RELEASE);
    __atomic_add_fetch(&shm->seq, 1, __ATOMIC_RELEASE);
}

// ------------------------------------------------------------------ mpv
static mpv_handle *mpv;
static mpv_render_context *rc;
static pthread_mutex_t upd_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t upd_cv = PTHREAD_COND_INITIALIZER;
static int upd_flag;

static void on_render_update(void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&upd_mu);
    upd_flag = 1;
    pthread_cond_signal(&upd_cv);
    pthread_mutex_unlock(&upd_mu);
}

static void on_mpv_wakeup(void *ctx)
{
    (void)ctx;
    os_wake();
}

// ---- info card (drawn by cstv_overlay.py into CSTV_CARD_PATH), blended into the frames while visible
static struct cstv_card_shm *card;

static void map_card(void)
{
    if (card) return;
    const struct cstv_card_shm *p = os_shm_open_ro(CSTV_CARD_PATH, sizeof *card);
    if (!p) return;
    if (p->magic == CSTV_CARD_MAGIC && p->w == CSTV_CARD_W && p->h == CSTV_CARD_H) card = (struct cstv_card_shm *)p;
    else os_shm_close(p, sizeof *card);
}

static int card_visible(void)
{
    if (!card) map_card();
    return card && now_ns() < card->until_ns;
}

static void blend_card(uint8_t *rgba)
{
    int cx = (int)card->x, cy = (int)card->y;
    for (int y = 0; y < CSTV_CARD_H && cy + y < CSTV_H; y++) {
        const uint8_t *s = card->buf + (size_t)y * CSTV_CARD_W * 4;
        uint8_t *d = rgba + ((size_t)(cy + y) * CSTV_W + cx) * 4;
        for (int x = 0; x < CSTV_CARD_W && cx + x < CSTV_W; x++, s += 4, d += 4) {
            unsigned a = s[3];
            if (!a) continue;
            d[0] = (uint8_t)((s[0] * a + d[0] * (255 - a)) / 255);
            d[1] = (uint8_t)((s[1] * a + d[1] * (255 - a)) / 255);
            d[2] = (uint8_t)((s[2] * a + d[2] * (255 - a)) / 255);
        }
    }
}

static atomic_int have_frame;   // a frame was rendered for the current video
static atomic_int file_loaded;  // mpv opened the current link (frames before that belong to the previous one)
static atomic_int video_ready;  // ...and started playing it: until then mpv may still redraw the previous
                                // video's last frame (e.g. when we set tracks), which looked like a still image
static atomic_int load_gen, frame_gen;   // which load the latest accepted frame belongs to
static int seek_wait;   // a seek holds the picture until enough is buffered
static int catching;    // playing a little faster/slower to get back in step with the server
static int resync_pending;   // a quality switch stalled playback: get back in step once it plays again
static int switch_h, switch_tries;
static int64_t switch_tried[4];
static double switch_pos0;
static int stream_broken;    // YouTube refused chunks mid-playback (403): reload with fresh links
static double sync_pos;      // the server's position at its last sync, and when that arrived
static uint64_t sync_at;
// How far we may drift from the server clock before correcting (the server says: 0.15 s while others
// watch too, 5 s alone). Within 1.5 s: play a little faster / slower; beyond: seek.
static double sync_tol = 0.4;
static int refresh_count;
static uint64_t last_refresh;
static char refresh_url[2048];
static uint64_t resync_set_at;

// ---- status screens the helper draws itself: "Nothing playing", "Loading...", errors.
// The game shows whatever is in the frame buffer, so modded players see these instead of the placeholder.
enum { SCR_VIDEO, SCR_IDLE, SCR_LOADING, SCR_ERROR, SCR_AUDIO, SCR_PAUSED };
// status screens and toasts were laid out on a 720-line frame; these scale them to the real one
#define UI_SCALE (CSTV_H / 720.0f)
#define UI(v) ((int)((v) * CSTV_H / 720))
static atomic_int screen_mode = SCR_IDLE;
static atomic_int screen_gen;            // bumped when the text below changes
static pthread_mutex_t screen_mu = PTHREAD_MUTEX_INITIALIZER;
static char screen_title[96], screen_line[200];
static stbtt_fontinfo font_bold, font_reg;
static int fonts_ok;

static void set_screen(int mode, const char *title, const char *line)
{
    pthread_mutex_lock(&screen_mu);
    snprintf(screen_title, sizeof screen_title, "%s", title ? title : "");
    snprintf(screen_line, sizeof screen_line, "%s", line ? line : "");
    pthread_mutex_unlock(&screen_mu);
    screen_mode = mode;
    screen_gen++;
}

static unsigned char *read_file(const char *path)
{
    FILE *f = os_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = n > 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    return d;
}

static int load_font(stbtt_fontinfo *fi, const char *const *paths)
{
    for (; *paths; paths++) {
        unsigned char *d = read_file(*paths);
        if (d && stbtt_InitFont(fi, d, stbtt_GetFontOffsetForIndex(d, 0))) return 1;
        free(d);
    }
    return 0;
}

static void load_fonts(void)
{
#ifdef _WIN32
    static char b1[1100], r1[1100];   // the Windows build carries its fonts
    os_exe_path(b1, sizeof b1, "fonts/NotoSans-Bold.ttf");
    os_exe_path(r1, sizeof r1, "fonts/NotoSans-Regular.ttf");
    const char *const bold[] = {b1, NULL};
    const char *const reg[] = {r1, NULL};
#else
    static const char *const bold[] = {"/usr/share/fonts/noto/NotoSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", NULL};
    static const char *const reg[] = {"/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", NULL};
#endif
    fonts_ok = load_font(&font_bold, bold) && load_font(&font_reg, reg);
    if (!fonts_ok) logf_("no font found: status screens will be blank");
}

static int utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    int cp = *p++, extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    if (extra) cp &= 0x3F >> extra;
    while (extra-- > 0 && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);
    *s = (const char *)p;
    return cp;
}

static int text_width(stbtt_fontinfo *f, float px, const char *s)
{
    float sc = stbtt_ScaleForPixelHeight(f, px), w = 0;
    int prev = 0;
    while (*s) {
        int cp = utf8_next(&s), adv, lsb;
        stbtt_GetCodepointHMetrics(f, cp, &adv, &lsb);
        if (prev) w += stbtt_GetCodepointKernAdvance(f, prev, cp) * sc;
        w += adv * sc;
        prev = cp;
    }
    return (int)w;
}

static void draw_text(uint8_t *rgba, stbtt_fontinfo *f, float px, int x, int baseline, const char *s, const uint8_t col[3])
{
    static unsigned char glyph[256 * 256];
    float sc = stbtt_ScaleForPixelHeight(f, px), xpos = (float)x;
    int prev = 0;
    while (*s) {
        int cp = utf8_next(&s), adv, lsb, x0, y0, x1, y1;
        if (prev) xpos += stbtt_GetCodepointKernAdvance(f, prev, cp) * sc;
        stbtt_GetCodepointBitmapBox(f, cp, sc, sc, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0 && gw <= 256 && gh <= 256) {
            stbtt_MakeCodepointBitmap(f, glyph, gw, gh, gw, sc, sc, cp);
            for (int gy = 0; gy < gh; gy++) {
                int py = baseline + y0 + gy;
                if (py < 0 || py >= CSTV_H) continue;
                for (int gx = 0; gx < gw; gx++) {
                    int pxx = (int)xpos + x0 + gx;
                    unsigned a = glyph[gy * gw + gx];
                    if (!a || pxx < 0 || pxx >= CSTV_W) continue;
                    uint8_t *d = rgba + ((size_t)py * CSTV_W + pxx) * 4;
                    for (int c = 0; c < 3; c++) d[c] = (uint8_t)((col[c] * a + d[c] * (255 - a)) / 255);
                }
            }
        }
        stbtt_GetCodepointHMetrics(f, cp, &adv, &lsb);
        xpos += adv * sc;
        prev = cp;
    }
}

// shorten to fit `maxw` pixels, ending in an ellipsis
static void fit_text(stbtt_fontinfo *f, float px, char *s, int maxw)
{
    if (text_width(f, px, s) <= maxw) return;
    size_t n = strlen(s);
    while (n > 0) {
        do n--; while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80);
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%.*s\xE2\x80\xA6", (int)n, s);
        if (text_width(f, px, tmp) <= maxw) { strcpy(s, tmp); return; }
    }
}

// ---- short notices over the picture, only on this player's screen ("Captions on", "Quality: 720p")
static pthread_mutex_t toast_mu = PTHREAD_MUTEX_INITIALIZER;
static char toast_text[160];
static _Atomic uint64_t toast_until;

static void toast(const char *fmt, ...)
{
    char t[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&toast_mu);
    snprintf(toast_text, sizeof toast_text, "%s", t);
    pthread_mutex_unlock(&toast_mu);
    toast_until = now_ns() + 2500000000ull;
}

static int toast_visible(void) { return now_ns() < toast_until; }

static void draw_toast(uint8_t *rgba)
{
    if (!fonts_ok) return;
    static const uint8_t white[3] = {239, 239, 241};
    char t[160];
    pthread_mutex_lock(&toast_mu);
    snprintf(t, sizeof t, "%s", toast_text);
    pthread_mutex_unlock(&toast_mu);
    const float px = 34 * UI_SCALE;
    int bw = text_width(&font_bold, px, t) + UI(48), bh = UI(62), x0 = UI(40), y0 = CSTV_H - UI(40) - bh;
    if (bw > CSTV_W - UI(80)) bw = CSTV_W - UI(80);
    for (int y = y0; y < y0 + bh; y++) {
        uint8_t *d = rgba + ((size_t)y * CSTV_W + x0) * 4;
        for (int x = 0; x < bw; x++, d += 4)
            for (int c = 0; c < 3; c++) d[c] = (uint8_t)(d[c] / 4 + 10);
    }
    draw_text(rgba, &font_bold, px, x0 + UI(24), y0 + UI(43), t, white);
}

static void compose_screen(uint8_t *rgba, int mode, int dots)
{
    static const uint8_t bg_top[3] = {22, 22, 27}, bg_bot[3] = {12, 12, 15};
    static const uint8_t white[3] = {239, 239, 241}, muted[3] = {150, 150, 162}, faint[3] = {80, 80, 92}, red[3] = {255, 120, 120};
    for (int y = 0; y < CSTV_H; y++) {
        uint8_t c[3];
        for (int k = 0; k < 3; k++) c[k] = (uint8_t)(bg_top[k] + (bg_bot[k] - bg_top[k]) * y / CSTV_H);
        uint8_t *row = rgba + (size_t)y * CSTV_W * 4;
        for (int x = 0; x < CSTV_W; x++) { row[x * 4] = c[0]; row[x * 4 + 1] = c[1]; row[x * 4 + 2] = c[2]; row[x * 4 + 3] = 255; }
    }
    if (!fonts_ok) return;
    char title[96], line[256];
    pthread_mutex_lock(&screen_mu);
    snprintf(title, sizeof title, "%s", screen_title);
    snprintf(line, sizeof line, "%s", screen_line);
    pthread_mutex_unlock(&screen_mu);
    const float tpx = 84 * UI_SCALE, lpx = 38 * UI_SCALE, hpx = 28 * UI_SCALE, mpx = 26 * UI_SCALE;
    if (mode == SCR_LOADING) {
        // "Loading" stays put while the dots come and go
        int w = text_width(&font_bold, tpx, "Loading...");
        int x = (CSTV_W - w) / 2;
        char t[16];
        snprintf(t, sizeof t, "Loading%.*s", dots, "...");
        draw_text(rgba, &font_bold, tpx, x, UI(360), t, white);
    } else {
        draw_text(rgba, &font_bold, tpx, (CSTV_W - text_width(&font_bold, tpx, title)) / 2, UI(360), title, mode == SCR_ERROR ? red : white);
    }
    fit_text(&font_reg, lpx, line, CSTV_W - UI(160));
    draw_text(rgba, &font_reg, lpx, (CSTV_W - text_width(&font_reg, lpx, line)) / 2, UI(430), line, muted);
    if (mode == SCR_IDLE) {
        const char *hint = "!queue <url>    !tvnext    !tvprev    !tv +30 / -10    !tvyt    !tvttv";
        draw_text(rgba, &font_reg, hpx, (CSTV_W - text_width(&font_reg, hpx, hint)) / 2, UI(520), hint, faint);
    }
    draw_text(rgba, &font_bold, mpx, CSTV_W - UI(40) - text_width(&font_bold, mpx, "CS:TV"), CSTV_H - UI(34), "CS:TV", faint);
}

static void *render_thread(void *p)
{
    (void)p;
    static uint8_t rgba[CSTV_W * CSTV_H * 4];   // what mpv rendered
    static uint8_t out[CSTV_W * CSTV_H * 4];    // + the info card
    int size[2] = {CSTV_W, CSTV_H};
    size_t stride = CSTV_W * 4;
    const char *fmt = "rgb0";   // byte order R,G,B,x = what the DXT1 encoder reads
    int block = 1;              // wait for each frame's display time: frames come out at the video's own pace
    mpv_render_param rp[] = {
        {MPV_RENDER_PARAM_SW_SIZE, size},
        {MPV_RENDER_PARAM_SW_FORMAT, (void *)fmt},
        {MPV_RENDER_PARAM_SW_STRIDE, &stride},
        {MPV_RENDER_PARAM_SW_POINTER, rgba},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
        {0}};
    int card_was = 0;
    for (;;) {
        pthread_mutex_lock(&upd_mu);
        if (!upd_flag) {   // wake at least every 100 ms so the card can appear/disappear while paused
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&upd_cv, &upd_mu, &ts);
        }
        int upd = upd_flag;
        upd_flag = 0;
        pthread_mutex_unlock(&upd_mu);
        int fresh = 0;
        if (upd && (mpv_render_context_update(rc) & MPV_RENDER_UPDATE_FRAME)) {
            mpv_render_context_render(rc, rp);
            if (file_loaded && video_ready && (screen_mode == SCR_LOADING || screen_mode == SCR_VIDEO)) {
                have_frame = 1;
                frame_gen = load_gen;
                screen_mode = SCR_VIDEO;
                fresh = 1;
            }
        }
        int cv = card_visible(), tv = toast_visible(), ov = cv || tv;   // something drawn over the picture
        int mode = screen_mode;
        if (mode != SCR_VIDEO) {
            static int last_mode = -1, last_dots = -1, last_gen = -1;
            int dots = mode == SCR_LOADING ? (int)((now_ns() / 400000000ull) % 4) : 0;
            if (mode != last_mode || dots != last_dots || screen_gen != last_gen || ov || card_was) {
                last_mode = mode;
                last_dots = dots;
                last_gen = screen_gen;
                compose_screen(out, mode, dots);
                if (cv) blend_card(out);
                if (tv) draw_toast(out);
                publish_frame(out);
            }
            card_was = ov;
            continue;
        }
        if (have_frame && (fresh || ov || card_was)) {
            if (ov) {
                memcpy(out, rgba, sizeof out);
                if (cv) blend_card(out);
                if (tv) draw_toast(out);
                publish_frame(out);
            } else {
                publish_frame(rgba);
            }
        }
        card_was = ov;
    }
    return NULL;
}

static int init_mpv(void)
{
    mpv = mpv_create();
    if (!mpv) return -1;
    const char *opts[][2] = {
        {"vo", "libmpv"},
        {"cache", "yes"},
        {"cache-on-disk", "no"},             // RAM only
        // fetch in 8 MB pieces like YouTube's own player: YouTube throttles a single open-ended request
        // for more than ~10 MB to about 2x the video's bitrate (0.35 MB/s measured, vs 13 MB/s chunked),
        // which left 1080p and every seek/quality switch starving
        {"stream-lavf-o", "request_size=8000000,multiple_requests=1"},
        {"demuxer-max-bytes", "64MiB"},
        {"demuxer-max-back-bytes", "16MiB"},
        {"ytdl", "yes"},
        {"ytdl-format", YT_FORMAT(1440)},
        {"rtsp-transport", "tcp"},            // CS:TV streams (the relay's RTSP is TCP only)
        {"ytdl-raw-options", "no-cache-dir=,sub-langs=en.*,write-subs=,write-auto-subs="},
        // every quality becomes its own (lazily opened) track, so a quality change is a track switch
        // (~instant, audio keeps playing) instead of a reload; first load is as fast as without it
        {"sid", "no"},                        // subtitles off until the player turns them on
        {"slang", "en,eng"},
        {"sub-font-size", "46"},
        {"sub-border-size", "3"},
        {"hwdec", "auto-copy"},
        {"keep-open", "yes"},
        {"cache-pause-initial", "yes"},      // start with a little buffer instead of stuttering at once
        {"cache-pause-wait", "1"},
        {"idle", "yes"},
        {"audio-client-name", "CS:TV"},
        {"volume", "100"},
        {"msg-level", "all=warn,ytdl_hook=info"},
        {"terminal", "yes"},
    };
    for (size_t i = 0; i < sizeof opts / sizeof *opts; i++)
        if (mpv_set_option_string(mpv, opts[i][0], opts[i][1]) < 0) logf_("mpv option %s rejected", opts[i][0]);
    // all_formats: every quality becomes its own (lazily opened) track, so a quality change is a track
    // switch instead of a reload (first load is as fast as without it).
    // ytdl_path: cstv_ytdl.py answers from the prefetch cache (the next video, looked up while the current
    // one plays) and otherwise runs yt-dlp.
    char so[1400];
#ifdef _WIN32
    // mpv needs a program here, not a script: cstv_ytdl.exe runs the bundled Python with cstv_ytdl.py
    char ytw[1100];
    os_exe_path(ytw, sizeof ytw, "cstv_ytdl.exe");
    snprintf(so, sizeof so, "ytdl_hook-all_formats=yes,ytdl_hook-ytdl_path=%s", ytw);
#else
    snprintf(so, sizeof so, "ytdl_hook-all_formats=yes,ytdl_hook-ytdl_path=%s/cstv_ytdl.py", exe_dir);
#endif
    if (mpv_set_option_string(mpv, "script-opts", so) < 0) logf_("mpv option script-opts rejected");
    // for tuning: CSTV_MPV_OPTS="name=value;name=value" overrides the above
    const char *extra = getenv("CSTV_MPV_OPTS");
    if (extra && *extra) {
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s", extra);
        char *save = NULL;
        for (char *kv = strtok_r(tmp, ";", &save); kv; kv = strtok_r(NULL, ";", &save)) {
            char *eq = strchr(kv, '=');
            if (!eq) continue;
            *eq = 0;
            logf_("mpv option override %s=%s%s", kv, eq + 1, mpv_set_option_string(mpv, kv, eq + 1) < 0 ? " (rejected)" : "");
        }
    }
    if (mpv_initialize(mpv) < 0) return -1;
    mpv_request_log_messages(mpv, "warn");   // to notice refused chunks (403) mid-playback
    mpv_render_param cp[] = {{MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_SW}, {0}};
    if (mpv_render_context_create(&rc, mpv, cp) < 0) return -1;
    mpv_render_context_set_update_callback(rc, on_render_update, NULL);
    mpv_set_wakeup_callback(mpv, on_mpv_wakeup, NULL);
    mpv_observe_property(mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, 0, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "eof-reached", MPV_FORMAT_FLAG);
    pthread_t th;
    load_fonts();
    set_screen(SCR_IDLE, "Nothing playing", "type  !link <url>  in chat");
    overlay_start();   // warm up the chat renderer now, not when the first chat is needed
    pthread_create(&th, NULL, render_thread, NULL);
    pthread_detach(th);
    return 0;
}

// ------------------------------------------------------------------ playback control
static char cur_url[2048];
static int cur_live, cur_paused;
static os_proc streamlink_proc;
#define streamlink_running() os_proc_valid(&streamlink_proc)
static double pending_seek = -1;   // seek once the file is loaded
static double cur_duration;        // 0 = unknown / live
static char cur_login[64];         // Twitch channel being shown, "" otherwise
static int opt_chat;               // this player wants Twitch chat
static int opt_subs;               // this player wants subtitles
// "auto" = the best up to 1440p: the screen texture is 1920x1080, but YouTube's 1440p (VP9 / AV1 only) scaled
// down is sharper than its 1080p, and decoding is on the GPU (hwdec auto-copy); 4K would only cost bandwidth
// (and stall over the VPN). "source" = the real best.
#define AUTO_MAX_H 1440
#define TW_AUTO "1440p60,1440p30,1440p,1080p60,1080p30,1080p,720p60,720p30,720p,best"
static char opt_tw_quality[128] = TW_AUTO;
// yt-dlp format for a height cap (0 = best): YouTube's regular https (DASH) streams first. Its HLS
// "Premium" formats (refused for non-Premium viewers: the stream opens, then every chunk is 403) and the
// AI-upscaled "-sr" ones are only a last resort.
static void yt_format_for(int h, char *out, size_t n)
{
    if (h > 0)
        snprintf(out, n, "bv*[height<=%d][protocol=https][format_id!*=-sr]+ba[protocol=https]/bv*[height<=%d]+ba/b[height<=%d]/bv*+ba/b", h, h, h);
    else
        snprintf(out, n, "bv*[protocol=https][format_id!*=-sr]+ba[protocol=https]/bv*+ba/b");
}
static char opt_yt_format[256] = YT_FORMAT(1440);
static double cur_speed = 1.0;

// ---- cstv_overlay.py (Twitch chat panel + info card + the Lobby page's chat), started when first needed.
// Its stdout answers the lobby chat's pickers (overlay_poll).
static os_proc overlay_proc;
static int overlay_fd = -1, overlay_out = -1;
static void overlay_replay(void);

static void overlay_start(void)
{
    if (overlay_fd >= 0) close(overlay_fd);
    if (overlay_out >= 0) close(overlay_out);
    overlay_fd = overlay_out = -1;
    char script[1100], py[1100];
    os_exe_path(script, sizeof script, "cstv_overlay.py");
    const char *argv[] = {tool("python3", PY_REL, py, sizeof py), script, NULL};
    if (os_spawn(argv, &overlay_fd, OS_OUT_PIPE, &overlay_out, &overlay_proc)) { overlay_fd = overlay_out = -1; return; }
#ifndef _WIN32
    fcntl(overlay_out, F_SETFL, O_NONBLOCK);
#endif
    logf_("overlay renderer started");
    overlay_replay();
}

static void overlay_send(const char *fmt, ...)
{
    if (overlay_fd >= 0 && !os_proc_running(&overlay_proc)) { close(overlay_fd); overlay_fd = -1; }
    if (overlay_fd < 0) overlay_start();
    if (overlay_fd < 0) return;
    char buf[512], *p = buf;
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof buf && (p = malloc((size_t)n + 1))) vsnprintf(p, (size_t)n + 1, fmt, ap2);   // GIF result lines are long
    va_end(ap2);
    if (n > 0 && p && write(overlay_fd, p, n) != n) logf_("overlay write failed");
    if (p != buf) free(p);
}

// A line for the lobby chat renderer ("hub" = from the hub, "lchat_ui" = from the game). Only base64url,
// digits, spaces, commas and dashes are passed on (so it needs no JSON escaping).
static int line_safe(const char *line)
{
    for (const char *c = line; *c; c++)
        if (!isalnum((unsigned char)*c) && !strchr(" ,-_.", *c)) return 0;
    return 1;
}

// The lobby's recent chat lines, replayed if the renderer starts again (it keeps them in memory only)
#define LCHAT_KEEP 60
static char *lchat_keep[LCHAT_KEEP];
static int lchat_keep_n;
static char lchat_emote_line[96];

static void overlay_line(const char *cmd, const char *line)
{
    if (!line_safe(line)) return;
    overlay_send("{\"cmd\":\"%s\",\"line\":\"%s\"}\n", cmd, line);
}

static void overlay_replay(void)
{
    if (lchat_emote_line[0]) overlay_line("hub", lchat_emote_line);
    for (int i = 0; i < lchat_keep_n; i++) overlay_line("hub", lchat_keep[i]);
}

static void lchat_remember(const char *line)
{
    if (!strncmp(line, "lchat_reset", 11)) {
        for (int i = 0; i < lchat_keep_n; i++) free(lchat_keep[i]);
        lchat_keep_n = 0;
        return;
    }
    if (!strncmp(line, "lchat_emotes ", 13)) {
        snprintf(lchat_emote_line, sizeof lchat_emote_line, "%s", line);
        return;
    }
    if (strncmp(line, "lchat ", 6)) return;
    if (lchat_keep_n == LCHAT_KEEP) {
        free(lchat_keep[0]);
        memmove(lchat_keep, lchat_keep + 1, (LCHAT_KEEP - 1) * sizeof *lchat_keep);
        lchat_keep_n--;
    }
    char *c = strdup(line);
    if (c) lchat_keep[lchat_keep_n++] = c;
}

static char cur_ytid[16];          // YouTube video id of the current link ("" = not YouTube)
static char cur_ytlive[512];       // a youtube.com/@channel/live link (the overlay finds the video id for chat)
static int chat_state;             // what the game was told: 1 = a chat panel is being drawn (2 = for the window only)
static int chat_gui;               // the game's window shows the live chat (its Lobby page's Live tab): draw it
static int chat_avail = -1;        // what the game was told: the current video has a live chat to show
static void game_send(const char *fmt, ...);

// Chat panel: a Twitch channel's chat, or a YouTube LIVE stream's chat (known once the stream loaded). Drawn
// when the player wants the panel beside the screen (pref chat) or the window's Live tab is open.
static void update_chat(void)
{
    static char shown[600];
    char want[600] = "";
    int on = opt_chat || chat_gui;
    int avail = cur_login[0] || (cur_live && (cur_ytid[0] || (cur_ytlive[0] && !strpbrk(cur_ytlive, "\"\\"))));
    if (avail != chat_avail) {
        chat_avail = avail;
        game_send("chatavail %d\n", avail);
    }
    if (on && cur_login[0]) snprintf(want, sizeof want, "tw:%s", cur_login);
    else if (on && cur_live && cur_ytid[0]) snprintf(want, sizeof want, "yt:%s", cur_ytid);
    else if (on && cur_live && cur_ytlive[0] && !strpbrk(cur_ytlive, "\"\\")) snprintf(want, sizeof want, "yu:%s", cur_ytlive);
    if (want[0] && !shown[0] && !have_frame) return;   // video first: chat starts once the picture is up
    int st = !want[0] ? 0 : opt_chat ? 1 : 2;   // 2: the game shows it in the window, not beside the screen
    if (st != chat_state) {
        chat_state = st;
        game_send("chat %d\n", st);   // the game hides the panel while there is no chat
    }
    if (!strcmp(want, shown)) return;
    snprintf(shown, sizeof shown, "%s", want);
    if (!strncmp(want, "tw:", 3)) overlay_send("{\"cmd\":\"channel\",\"login\":\"%s\"}\n", want + 3);
    else if (!strncmp(want, "yt:", 3)) overlay_send("{\"cmd\":\"youtube\",\"video\":\"%s\"}\n", want + 3);
    else if (!strncmp(want, "yu:", 3)) overlay_send("{\"cmd\":\"youtube\",\"url\":\"%s\"}\n", want + 3);
    else if (overlay_fd >= 0) overlay_send("{\"cmd\":\"channel\",\"login\":null}\n");
}
static uint64_t stall_start;
static uint64_t loaded_at;

// Never wait on streamlink for long: if it can't finish (e.g. stuck writing into a pipe nobody reads),
// it gets 0.2 s after SIGTERM and is then killed (it holds nothing worth a clean exit, and its own
// shutdown takes over a second). An unbounded wait here once froze the whole helper.
static void kill_streamlink(void)
{
    os_proc_kill(&streamlink_proc, 200);
}

#ifdef _WIN32
// mpv on Windows can't read a pipe by its number (no fd://), so streamlink's output reaches it over a
// local HTTP connection: we listen on 127.0.0.1, answer mpv's request, and copy the pipe into it.
// Either side closing ends it: mpv stopping closes the pipe (streamlink then stops), streamlink ending
// closes the connection.
struct relay { SOCKET ls; int fd; };

static void *relay_thread(void *arg)
{
    struct relay r = *(struct relay *)arg;
    free(arg);
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(r.ls, &rs);
    struct timeval tv = {30, 0};
    SOCKET c = select(0, &rs, NULL, NULL, &tv) == 1 ? accept(r.ls, NULL, NULL) : INVALID_SOCKET;
    closesocket(r.ls);
    if (c != INVALID_SOCKET) {
        char req[4096];
        int got = 0, n;
        while (got < (int)sizeof req - 1 && (n = recv(c, req + got, (int)sizeof req - 1 - got, 0)) > 0) {
            got += n;
            req[got] = 0;
            if (strstr(req, "\r\n\r\n")) break;
        }
        static const char hdr[] = "HTTP/1.0 200 OK\r\nContent-Type: video/mp2t\r\nConnection: close\r\n\r\n";
        if (send(c, hdr, (int)sizeof hdr - 1, 0) == (int)sizeof hdr - 1) {
            static char buf[1 << 16];   // one relay at a time (a new stream kills the old streamlink first)
            int rd;
            while ((rd = read(r.fd, buf, sizeof buf)) > 0) {
                int off = 0;
                while (off < rd) {
                    int w = send(c, buf + off, rd - off, 0);
                    if (w <= 0) goto done;
                    off += w;
                }
            }
        }
    done:
        closesocket(c);
    }
    close(r.fd);
    return NULL;
}

static int relay_start(int fd, char *url, size_t n)
{
    SOCKET ls = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (ls == INVALID_SOCKET) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int al = sizeof a;
    struct relay *r = malloc(sizeof *r);
    pthread_t t;
    if (!r || bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1) || getsockname(ls, (struct sockaddr *)&a, &al)) {
        free(r);
        closesocket(ls);
        return -1;
    }
    r->ls = ls;
    r->fd = fd;
    if (pthread_create(&t, NULL, relay_thread, r)) { free(r); closesocket(ls); return -1; }
    pthread_detach(t);
    snprintf(url, n, "http://127.0.0.1:%d/stream.ts", ntohs(a.sin_port));
    return 0;
}
#endif

// ---- lines back to the game (srv lines from the master can be long: a whole queue)
static sock_t game_fd = SOCK_BAD;

static void game_send(const char *fmt, ...)
{
    if (game_fd == SOCK_BAD) return;
    char buf[1024], *p = buf;
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof buf && (p = malloc((size_t)n + 1))) vsnprintf(p, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    if (n > 0 && p && send(game_fd, p, n, SOCK_SEND_FLAGS) != n) logf_("could not send to the game");
    if (p != buf) free(p);
}

static void master_send(const char *fmt, ...);
static long cur_rev;   // the master's link revision the current video belongs to (for started / ended)

static void b64encode(const char *in, char *out, size_t outsz)   // base64url, no padding
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 2 < outsz; p++) {
        acc = (acc << 8) | *p;
        bits += 8;
        while (bits >= 6 && o + 2 < outsz) { bits -= 6; out[o++] = T[(acc >> bits) & 63]; }
    }
    if (bits > 0 && o + 1 < outsz) out[o++] = T[(acc << (6 - bits)) & 63];
    out[o] = 0;
}

// ---- link safety. Every viewer's mpv/yt-dlp fetches whatever link anyone in the room sets, so links
// to loopback, LAN and other non-public addresses are refused (checked here; mpv and yt-dlp resolve the
// name again themselves, and redirects aren't followed up: accepted). rtmp:// only from the host the hub
// relays our own streams on (it says which in "streamhost" when we connect).
static char hub_url[512] = "wss://cstv.xyz/game";   // CSTV_HUB_URL, else hub= in the prefs
static char stream_host[256];

// host of a link (no userinfo, port or IPv6 brackets)
static int url_host(const char *u, char *host, size_t n)
{
    const char *p = strstr(u, "://");
    if (!p) return 0;
    p += 3;
    size_t al = strcspn(p, "/?#");
    for (size_t i = al; i > 0; i--)
        if (p[i - 1] == '@') { p += i; al -= i; break; }
    size_t hl;
    if (*p == '[') {
        const char *e = memchr(p, ']', al);
        if (!e) return 0;
        p++;
        hl = (size_t)(e - p);
    } else {
        hl = 0;
        while (hl < al && p[hl] != ':') hl++;
    }
    if (!hl || hl >= n) return 0;
    for (size_t i = 0; i < hl; i++) host[i] = (char)tolower((unsigned char)p[i]);
    host[hl] = 0;
    return 1;
}

static int v4_global(uint32_t a)   // host byte order
{
    static const struct { uint32_t net, mask; } bad[] = {
        {0x00000000, 0xFF000000}, {0x0A000000, 0xFF000000}, {0x64400000, 0xFFC00000},   // 0/8, 10/8, CGNAT
        {0x7F000000, 0xFF000000}, {0xA9FE0000, 0xFFFF0000}, {0xAC100000, 0xFFF00000},   // loopback, link-local, 172.16/12
        {0xC0000000, 0xFFFFFF00}, {0xC0000200, 0xFFFFFF00}, {0xC0A80000, 0xFFFF0000},   // 192.0.0/24, TEST-NET-1, 192.168/16
        {0xC6120000, 0xFFFE0000}, {0xC6336400, 0xFFFFFF00}, {0xCB007100, 0xFFFFFF00},   // benchmarking, TEST-NET-2/3
        {0xE0000000, 0xE0000000},                                                       // multicast, reserved, broadcast
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        if ((a & bad[i].mask) == bad[i].net) return 0;
    return 1;
}

static int addr_global(const struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) return v4_global(ntohl(((const struct sockaddr_in *)sa)->sin_addr.s_addr));
    if (sa->sa_family != AF_INET6) return 0;
    const uint8_t *b = ((const struct sockaddr_in6 *)sa)->sin6_addr.s6_addr;
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF}, nat64[12] = {0, 0x64, 0xFF, 0x9B};
    if (!memcmp(b, mapped, 12) || !memcmp(b, nat64, 12))   // an IPv4 address inside: judge that
        return v4_global((uint32_t)b[12] << 24 | b[13] << 16 | b[14] << 8 | b[15]);
    if (b[0] == 0x20 && b[1] == 0x02) return v4_global((uint32_t)b[2] << 24 | b[3] << 16 | b[4] << 8 | b[5]);   // 6to4
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0D && b[3] == 0xB8) return 0;   // documentation
    return (b[0] & 0xE0) == 0x20;   // global unicast 2000::/3 only (no ::1, fc00::/7, fe80::/10, ff00::/8, ...)
}

// 1: every address the name resolves to is public. numeric_only: don't ask DNS (-1 = it's a name)
static int host_global(const char *host, int numeric_only)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = numeric_only ? AI_NUMERICHOST : 0;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc) return numeric_only && rc == EAI_NONAME ? -1 : 0;
    int ok = 1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) ok &= addr_global(ai->ai_addr);
    freeaddrinfo(res);
    return ok;
}

// the form of a link: http(s) (host checked separately) or rtmp from the master
static int url_syntax_ok(const char *u)
{
    size_t n = strlen(u);
    if (n < 8 || n >= 2048) return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)u[i] <= ' ' || u[i] == 0x7f) return 0;
    char host[256];
    if (!url_host(u, host, sizeof host)) return 0;
    if (!strncmp(u, "rtmp://", 7)) return stream_host[0] && !strcasecmp(host, stream_host);
    return !strncmp(u, "https://", 8) || !strncmp(u, "http://", 7);
}

// the full check; may wait for DNS (threads, or after the cache in check_load)
static int url_ok(const char *u)
{
    char host[256];
    if (!url_syntax_ok(u)) return 0;
    if (!strncmp(u, "rtmp://", 7)) return 1;
    return url_host(u, host, sizeof host) && host_global(host, 0) == 1;
}

// ---- YouTube playlists. A playlist link plays one video at a time, loaded on its own (fast); the list
// is looked up on the side (first PL_MAX entries from the current one) so that, when the video ends or
// someone skips, we can tell the master which video is next ("ended"), and the master switches
// everyone, so all viewers stay on the same video.
#define PL_MAX 30
static pthread_mutex_t pl_mu = PTHREAD_MUTEX_INITIALIZER;
static char pl_list[64];            // list id the entries belong to
static int pl_start;                // playlist position (1-based) of pl_ids[0]
static int pl_n;
static char pl_ids[PL_MAX][16];
static char pl_titles[PL_MAX][64];
static int pl_busy;                 // a lookup is running
static unsigned pl_gen;
static char cur_vid[16], cur_list[64];   // the video + list of the current link ("" = not a playlist)
static int cur_index;               // index= of the current link (0 = not given)
static int want_next, next_sent, pl_lookups;
static int pl_wait;                 // a playlist page without a video: load its first entry once known
static double pl_wait_pos;
static uint64_t pl_wait_at;

static int yt_param(const char *url, const char *key, char *out, size_t n)
{
    const char *p = strchr(url, '?');
    size_t kl = strlen(key);
    for (; p && *p; p = strchr(p + 1, '&')) {
        if (!strncmp(p + 1, key, kl) && p[1 + kl] == '=') {
            const char *v = p + 2 + kl;
            size_t i = 0;
            for (; v[i] && v[i] != '&' && v[i] != '#' && i + 1 < n; i++) out[i] = v[i];
            out[i] = 0;
            return i > 0;
        }
    }
    return 0;
}

static int id_ok(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_') return 0;
    return 1;
}

// youtube.com/watch?v=, youtu.be/<id>, /shorts/<id>, /live/<id>; plus list= and index=
static int yt_parse(const char *url, char vid[16], char list[64], int *index)
{
    vid[0] = list[0] = 0;
    *index = 0;
    const char *h = strstr(url, "://");
    if (!h) return 0;
    h += 3;
    const char *path = strchr(h, '/');
    if (!path) return 0;
    size_t hl = (size_t)(path - h);
    char host[128];
    if (hl >= sizeof host) return 0;
    memcpy(host, h, hl);
    host[hl] = 0;
    int short_link = !strcmp(host, "youtu.be") || !strcmp(host, "www.youtu.be");
    int yt = hl >= 11 && !strcmp(host + hl - 11, "youtube.com") && (hl == 11 || host[hl - 12] == '.');
    if (!short_link && !yt) return 0;
    const char *idp = NULL;
    if (short_link) idp = path + 1;
    else if (!strncmp(path, "/shorts/", 8)) idp = path + 8;
    else if (!strncmp(path, "/live/", 6)) idp = path + 6;
    else if (!strncmp(path, "/watch", 6)) yt_param(url, "v", vid, 16);
    if (idp) {
        size_t i = 0;
        for (; idp[i] && idp[i] != '?' && idp[i] != '#' && idp[i] != '/' && i < 15; i++) vid[i] = idp[i];
        vid[i] = 0;
    }
    yt_param(url, "list", list, 64);
    char ix[16];
    if (yt_param(url, "index", ix, sizeof ix)) *index = atoi(ix);
    if (!id_ok(vid)) vid[0] = 0;
    if (!id_ok(list)) list[0] = 0;
    return 1;
}

// ---- prefetch: look the next video up while the current one plays (cstv_ytdl.py keeps the answer)
static char prefetched[2048];

static void *prefetch_thread(void *arg)
{
    char *url = arg, script[1100], py[1100];
    if (!url_ok(url)) { logf_("not looking up %s: not a public address", url); free(url); return NULL; }
    os_exe_path(script, sizeof script, "cstv_ytdl.py");
    const char *argv[] = {tool("python3", PY_REL, py, sizeof py), script, "--prefetch", url, NULL};
    os_proc p;
    if (os_spawn(argv, NULL, OS_OUT_NULL, NULL, &p) == 0) os_proc_wait(&p);
    free(url);
    return NULL;
}

static void prefetch(const char *url)
{
    char vid[16], list[64], single[128];
    int index;
    const char *u = url;
    if (is_twitch_channel_l(url, NULL, 0) || is_yt_live_url(url) || !strncmp(url, "rtmp://", 7)) return;   // live: nothing to look up
    if (yt_parse(url, vid, list, &index) && list[0]) {   // load_url plays playlist videos as the bare video link
        if (!vid[0]) return;
        snprintf(single, sizeof single, "https://www.youtube.com/watch?v=%s", vid);
        u = single;
    }
    if (!strcmp(u, prefetched)) return;
    snprintf(prefetched, sizeof prefetched, "%s", u);
    char *copy = strdup(u);
    pthread_t t;
    if (copy && pthread_create(&t, NULL, prefetch_thread, copy) == 0) pthread_detach(t);
    else free(copy);
    logf_("looking up the next video ahead: %s", u);
}

struct title_job { char url[2048]; char uid[24]; };

static void *title_thread(void *arg)
{
    struct title_job *j = arg;
    char out[512] = "", yt[1100];
    if (!url_ok(j->url)) { free(j); return NULL; }
    const char *argv[] = {tool("yt-dlp", YTDLP_REL, yt, sizeof yt), "--no-cache-dir", "--no-warnings", "--flat-playlist",
                          "-I", "1", "--print", "%(title).60s", "--", j->url, NULL};
    os_proc pr;
    int fd;
    if (os_spawn(argv, NULL, OS_OUT_PIPE, &fd, &pr) == 0) {
        size_t len = 0;
        int r;
        while (len + 1 < sizeof out && (r = (int)read(fd, out + len, (unsigned)(sizeof out - 1 - len))) > 0) len += (size_t)r;
        out[len] = 0;
        close(fd);
        os_proc_wait(&pr);
    }
    out[strcspn(out, "\n")] = 0;
    size_t L = strlen(out);   // yt-dlp appends " YYYY-MM-DD HH:MM" to live streams' titles
    if (L > 17 && out[L - 17] == ' ' && out[L - 12] == '-' && out[L - 9] == '-' && out[L - 6] == ' ' && out[L - 3] == ':') out[L - 17] = 0;
    size_t tl = strlen(out);
    if (tl > 90) {   // cut on a UTF-8 character boundary
        tl = 90;
        while (tl > 0 && ((unsigned char)out[tl] & 0xC0) == 0x80) tl--;
        out[tl] = 0;
    }
    if (out[0]) {
        char bt[200];
        b64encode(out, bt, sizeof bt);
        master_send("title %s %s\n", j->uid, bt);
    }
    free(j);
    return NULL;
}

struct pl_job { char url[2048]; char list[64]; int start; unsigned gen; };

static void *pl_thread(void *arg)
{
    struct pl_job *j = arg;
    char range[32];
    snprintf(range, sizeof range, "%d:%d", j->start, j->start + PL_MAX - 1);
    const size_t cap = 64 * 1024;
    char *out = malloc(cap);
    size_t len = 0;
    if (!out) { free(j); return NULL; }
    char yt[1100];
    const char *argv[] = {tool("yt-dlp", YTDLP_REL, yt, sizeof yt), "--no-cache-dir", "--no-warnings", "--flat-playlist",
                          "-I", range, "--print", "%(id)s %(title).50s", "--", j->url, NULL};
    os_proc pr;
    int fd;
    if (os_spawn(argv, NULL, OS_OUT_PIPE, &fd, &pr) == 0) {
        int r;
        while (len + 1 < cap && (r = (int)read(fd, out + len, (unsigned)(cap - 1 - len))) > 0) len += (size_t)r;
        close(fd);
        os_proc_wait(&pr);
    }
    out[len] = 0;
    pthread_mutex_lock(&pl_mu);
    if (j->gen == pl_gen) {
        pl_n = 0;
        pl_start = j->start;
        snprintf(pl_list, sizeof pl_list, "%s", j->list);
        char *save = NULL;
        for (char *line = strtok_r(out, "\n", &save); line && pl_n < PL_MAX; line = strtok_r(NULL, "\n", &save)) {
            char *sp = strchr(line, ' ');
            if (sp) *sp = 0;
            if (strlen(line) > 15 || !id_ok(line)) continue;
            snprintf(pl_ids[pl_n], 16, "%s", line);
            const char *t = sp ? sp + 1 : "";
            size_t tl = strlen(t);
            if (tl > 63) {   // cut on a UTF-8 character boundary
                tl = 63;
                while (tl > 0 && ((unsigned char)t[tl] & 0xC0) == 0x80) tl--;
            }
            memcpy(pl_titles[pl_n], t, tl);
            pl_titles[pl_n][tl] = 0;
            pl_n++;
        }
        pl_busy = 0;
        logf_("playlist %s: %d videos from #%d", pl_list, pl_n, pl_start);
    }
    pthread_mutex_unlock(&pl_mu);
    free(out);
    free(j);
    return NULL;
}

// Radio mixes (list ids starting with RD) are rebuilt around whatever video they are looked up from: the
// current video comes first when asked from the start, and isn't there at all when asked from its index.
static int list_start(const char *list, int index)
{
    return (!strncmp(list, "RD", 2) || index < 1) ? 1 : index;
}

// is `vid` in the list we have, with at least one video after it? (-1 = no)
static int pl_find(const char *vid)
{
    int found = -1;
    pthread_mutex_lock(&pl_mu);
    if (!strcmp(pl_list, cur_list))
        for (int i = 0; i + 1 < pl_n; i++)
            if (!strcmp(pl_ids[i], vid)) { found = i; break; }
    pthread_mutex_unlock(&pl_mu);
    return found;
}

// the playlist window around the current video is known (the master doesn't take it yet: kept for `ended`)
static char win_sent_vid[16];

static void playlist_lookup(const char *url, const char *list, int start)
{
    struct pl_job *j = calloc(1, sizeof *j);
    if (!j) return;
    snprintf(j->url, sizeof j->url, "%s", url);
    snprintf(j->list, sizeof j->list, "%s", list);
    j->start = start > 0 ? start : 1;
    pthread_mutex_lock(&pl_mu);
    j->gen = ++pl_gen;
    pl_busy = 1;
    pthread_mutex_unlock(&pl_mu);
    pl_lookups++;
    pthread_t t;
    if (pthread_create(&t, NULL, pl_thread, j) == 0) pthread_detach(t);
    else { free(j); pthread_mutex_lock(&pl_mu); pl_busy = 0; pthread_mutex_unlock(&pl_mu); }
}

// Tell the master the video is over, and which video follows it in its playlist (once the list is known).
static void send_next(void)
{
    if (next_sent || !cur_url[0]) { want_next = 0; return; }
    if (!cur_list[0] || !cur_vid[0]) {   // not a playlist: just "this video finished" (the server may have a queue)
        master_send("ended %ld - -\n", cur_rev);
        next_sent = 1;
        want_next = 0;
        return;
    }
    pthread_mutex_lock(&pl_mu);
    int busy = pl_busy, found = -1, have = !strcmp(pl_list, cur_list);
    if (have)
        for (int i = 0; i < pl_n; i++)
            if (!strcmp(pl_ids[i], cur_vid)) { found = i; break; }
    char url[256] = "", title[64] = "";
    if (found >= 0 && found + 1 < pl_n) {
        snprintf(url, sizeof url, "https://www.youtube.com/watch?v=%s&list=%s&index=%d", pl_ids[found + 1], cur_list,
                 pl_start + found + 1);
        snprintf(title, sizeof title, "%s", pl_titles[found + 1]);
    }
    int last_known = found >= 0 && found + 1 >= pl_n;
    int start = found >= 0 ? pl_start + found : (cur_index > 0 ? cur_index : 1);
    pthread_mutex_unlock(&pl_mu);
    if (url[0]) {
        char bu[400], bt[100];
        b64encode(url, bu, sizeof bu);
        b64encode(title, bt, sizeof bt);
        master_send("ended %ld %s %s\n", cur_rev, bu, bt[0] ? bt : "-");
        logf_("playlist: next is %s (%s)", url, title);
        next_sent = 1;
        want_next = 0;
    } else if (busy) {
        // wait for the lookup to finish; called again from the main loop
    } else if (pl_lookups < 3 && (!have || (last_known && pl_n == PL_MAX) || found < 0)) {
        playlist_lookup(cur_url, cur_list, list_start(cur_list, start));   // not in what we have yet: look further
    } else {
        logf_("playlist: no next video (end of the list)");
        master_send("ended %ld - -\n", cur_rev);
        next_sent = 1;
        want_next = 0;
    }
}

// when we last gave up on a video and asked the game for the next one, and why: with nothing next the game
// answers "stop", and the screen should keep saying why instead of "Nothing playing"
static uint64_t gave_up_at;
static char gave_up_why[200];
static void give_up(const char *why)
{
    snprintf(gave_up_why, sizeof gave_up_why, "%s", why);
    gave_up_at = now_ns();
}

static void stop_playback(const char *why)
{
    const char *cmd[] = {"stop", NULL};
    mpv_command(mpv, cmd);
    kill_streamlink();
    cur_url[0] = 0;
    cur_login[0] = 0;
    cur_vid[0] = cur_list[0] = cur_ytid[0] = 0;
    want_next = pl_wait = 0;
    have_frame = 0;
    shm->latest = CSTV_NONE;
    shm->live = 0;
    shm->pos_s = 0;
    update_chat();
    set_status(CSTV_IDLE, "stopped (%s)", why);
    set_screen(SCR_IDLE, "Nothing playing", "type  !link <url>  in chat");
}

// twitch.tv/<channel> (live) -> streamlink; VODs/clips and everything else -> mpv + yt-dlp.
// On a match the channel login is copied to `login` (if given).
static int is_twitch_channel_l(const char *u, char *login, size_t n);
static int is_twitch_channel_l(const char *u, char *login, size_t n)
{
    const char *p = strstr(u, "://");
    if (!p) return 0;
    p += 3;
    if (!strncmp(p, "www.", 4)) p += 4;
    else if (!strncmp(p, "m.", 2)) p += 2;
    if (strncmp(p, "twitch.tv/", 10)) return 0;
    p += 10;
    if (!*p || strchr(p, '/') || !strncmp(p, "videos", 6)) return 0;
    if (login) {
        size_t i = 0;
        for (; p[i] && p[i] != '?' && p[i] != '#' && i + 1 < n; i++) login[i] = (char)((p[i] >= 'A' && p[i] <= 'Z') ? p[i] + 32 : p[i]);
        login[i] = 0;
        for (size_t k = 0; k < i; k++)
            if (!((login[k] >= 'a' && login[k] <= 'z') || (login[k] >= '0' && login[k] <= '9') || login[k] == '_')) { login[0] = 0; break; }
    }
    return 1;
}

// youtube.com/live/<id>, youtube.com/@channel/live (also /channel/<id>/live, /c/<name>/live): live streams.
// streamlink plays YouTube's combined audio+video live streams, which start ~2.5 s sooner than the
// separate video and audio streams mpv gets through yt-dlp.
static int is_yt_live_url(const char *u)
{
    const char *h = strstr(u, "://");
    if (!h) return 0;
    h += 3;
    const char *path = strchr(h, '/');
    if (!path) return 0;
    size_t hl = (size_t)(path - h);
    if (!((hl == 11 && !strncmp(h, "youtube.com", 11)) || (hl == 15 && !strncmp(h, "www.youtube.com", 15)) ||
          (hl == 13 && !strncmp(h, "m.youtube.com", 13))))
        return 0;
    size_t pl = strcspn(path, "?#");
    if (pl > 6 && !strncmp(path, "/live/", 6)) return 1;
    if (pl >= 5 && !strncmp(path + pl - 5, "/live", 5) && pl > 5) return 1;
    if (pl >= 6 && !strncmp(path + pl - 6, "/live/", 6)) return 1;
    return 0;
}

static int have_streamlink(void)
{
#ifdef _WIN32
    char p[1100];
    os_exe_path(p, sizeof p, STREAMLINK_REL);
    return os_file_exists(p);
#else
    return os_file_exists("/usr/bin/streamlink") || os_file_exists("/usr/sbin/streamlink");
#endif
}

static int capturing(void);
static char cap_url[2048];      // the stream this player is sending (rtmp://.../live/<id>?key=...)
static uint64_t live_retry_at;  // a CS:TV stream that wasn't there yet: try it again then
static int live_retries;

static const char *load_note;   // second line of the loading screen instead of the link
static int opt_audio;           // this player picked "audio only"
static int load_retries;
static char ytdl_err[200];      // why yt-dlp refused the current link ("" = it didn't)
static double req_pos;
static uint64_t req_at;
static int pending_running = 1;   // the server's clock runs while we load: add the load time to the seek
static int load_running = 1;      // what the game said for the current load request

// hand a URL to mpv (the part shared by first loads, retries and playlist pages)
static void mpv_load(const char *load, const char *shown, double pos_s, int paused)
{
    if (pos_s > 0.5 && !cur_live) { pending_seek = pos_s; pending_running = load_running; }
    char host[200];
    const char *h = strstr(shown, "://");
    snprintf(host, sizeof host, "%s", h ? h + 3 : shown);
    set_screen(SCR_LOADING, "Loading", load_note ? load_note : host);
    mpv_set_property_string(mpv, "vid", "auto");   // a quality pick (vid=N / no) would otherwise stick to the next file
    mpv_set_property_string(mpv, "sid", "no");     // captions are picked once the video is open (apply_subs)
    shm->live = cur_live;
    mpv_set_option_string(mpv, "ytdl-format", opt_yt_format);
    const char *cmd[] = {"loadfile", load, NULL};
    mpv_command(mpv, cmd);
    double sp = cur_live ? 1.0 : cur_speed;
    mpv_set_property(mpv, "speed", MPV_FORMAT_DOUBLE, &sp);
    update_chat();
    int f = paused && !cur_live;
    mpv_set_property(mpv, "pause", MPV_FORMAT_FLAG, &f);
    loaded_at = now_ns();
    set_status(CSTV_LOADING, "loading %s%s", streamlink_running() ? "[live via streamlink] " : cur_live ? "[live] " : "", shown);
}

static void load_url(const char *url, double pos_s, int paused);

static void start_playback(const char *url, double pos_s, int paused)
{
    gave_up_at = 0;
    load_retries = 0;
    live_retries = 0;
    live_retry_at = 0;
    next_sent = want_next = 0;
    req_pos = pos_s;
    req_at = now_ns();
    load_url(url, pos_s, paused);
}

static void load_url(const char *url, double pos_s, int paused)
{
    kill_streamlink();
    snprintf(cur_url, sizeof cur_url, "%s", url);
    cur_paused = paused;
    cur_live = 0;
    mpv_set_property_string(mpv, "mute", "no");   // (only this player's own stream is shown silent)
    pending_seek = -1;
    shm->latest = CSTV_NONE;
    have_frame = 0;
    file_loaded = 0;
    video_ready = 0;
    load_gen++;
    seek_wait = 0;
    catching = 0;
    resync_pending = 0;
    stream_broken = 0;
    ytdl_err[0] = 0;
    sync_at = 0;   // positions from the previous link mean nothing for this one
    cur_login[0] = 0;
    cur_ytlive[0] = 0;
    char target[64];
    const char *load = url;
    int tw = is_twitch_channel_l(url, cur_login, sizeof cur_login), ytl = !tw && is_yt_live_url(url);
    if (!tw) cur_login[0] = 0;
    if ((tw || ytl) && have_streamlink()) {
        char sl[1100];
        const char *argv[10];
        int a = 0, fd = -1;
        argv[a++] = tool("streamlink", STREAMLINK_REL, sl, sizeof sl);
        argv[a++] = "--stdout";
        argv[a++] = "--loglevel";
        argv[a++] = "warning";
        // streamlink >= 8 skips Twitch's ad segments by itself (the old --twitch-disable-ads is gone)
        // --twitch-low-latency: ~3-4 s behind live with a steady buffer (tested: 0 stalls in 120 s
        // over the user's VPN, where normal mode stalled every 30-60 s)
        if (tw) argv[a++] = "--twitch-low-latency";
        argv[a++] = url;
        argv[a++] = opt_tw_quality;
        argv[a] = NULL;
        int started = os_spawn(argv, NULL, OS_OUT_PIPE, &fd, &streamlink_proc) == 0;
#ifdef _WIN32
        if (started && relay_start(fd, target, sizeof target)) { close(fd); kill_streamlink(); started = 0; }
#else
        if (started) snprintf(target, sizeof target, "fdclose://%d", fd);   // mpv closes it when it stops reading
#endif
        if (started) {
            load = target;
            cur_live = 1;
            if (ytl) {   // chat: the id is in /live/<id> links; for @channel/live the overlay looks it up
                char v[16], l[64];
                int ix;
                cur_ytid[0] = 0;
                if (yt_parse(url, v, l, &ix) && v[0]) snprintf(cur_ytid, sizeof cur_ytid, "%s", v);
                else snprintf(cur_ytlive, sizeof cur_ytlive, "%s", url);
            }
        }
    }
    static char rtsp[2100];
    if (!strncmp(url, "rtmp://", 7)) {   // a CS:TV stream: live, no seeking, pause = stop
        cur_live = 1;
        const char *path = strchr(url + 7, '/');
        // our own stream: shown like everyone sees it (from the relay), but silent: its sound is this game's
        // own, which the capture would take in again (an echo that builds up)
        if (path && capturing() && strstr(cap_url, path)) {
            mpv_set_property_string(mpv, "mute", "yes");
            logf_("live: this player's own stream (shown muted)");
        }
        // read over RTSP (TCP 8554 on the relay, same path): the relay's RTMP side drops the Opus sound that
        // streams have (Opus because the website plays them over WebRTC, which can't carry AAC)
        if (path && stream_host[0]) {
            snprintf(rtsp, sizeof rtsp, "rtsp://%s:8554%s", stream_host, path);
            load = rtsp;
        }
    }
    char vid[16], list[64], single[128];
    int index;
    pl_wait = 0;
    if (!cur_live) cur_ytid[0] = 0;
    int is_yt = !cur_live && yt_parse(url, vid, list, &index);
    if (is_yt) snprintf(cur_ytid, sizeof cur_ytid, "%s", vid);
    if (is_yt && list[0]) {
        if (strcmp(vid, cur_vid) || strcmp(list, cur_list)) { next_sent = 0; want_next = 0; pl_lookups = 0; }
        snprintf(cur_list, sizeof cur_list, "%s", list);
        snprintf(cur_vid, sizeof cur_vid, "%s", vid);
        cur_index = index;
        win_sent_vid[0] = 0;
        pthread_mutex_lock(&pl_mu);
        int busy = pl_busy;
        pthread_mutex_unlock(&pl_mu);
        // with a video in the link, the list is looked up once its picture is up (main loop)
        if (!vid[0] && !busy) playlist_lookup(url, list, list_start(list, index));
        if (!vid[0]) {   // a playlist page: play its first video once the list is known
            const char *cmd[] = {"stop", NULL};
            mpv_command(mpv, cmd);
            pl_wait = 1;
            pl_wait_pos = pos_s;
            pl_wait_at = now_ns();
            set_status(CSTV_LOADING, "loading playlist %s", list);
            set_screen(SCR_LOADING, "Loading", "a YouTube playlist");
            return;
        }
        // just this video: with the list attached, yt-dlp may walk hundreds of entries first
        snprintf(single, sizeof single, "https://www.youtube.com/watch?v=%s", vid);
        load = single;
    } else if (!cur_live) {
        cur_vid[0] = cur_list[0] = 0;
    }
    mpv_load(load, url, pos_s, paused);
}

// A playlist page's list arrived: start its first video (at the position the server has reached since).
static void pl_check_wait(void)
{
    if (!pl_wait) return;
    pthread_mutex_lock(&pl_mu);
    int busy = pl_busy, ok = !strcmp(pl_list, cur_list) && pl_n > 0;
    char first[16] = "";
    int start = pl_start;
    if (ok) snprintf(first, sizeof first, "%s", pl_ids[0]);
    pthread_mutex_unlock(&pl_mu);
    if (busy) return;
    pl_wait = 0;
    if (!ok) { set_status(CSTV_ERROR, "could not read the playlist"); set_screen(SCR_ERROR, "Couldn't play this", "could not read the playlist"); return; }
    snprintf(cur_vid, sizeof cur_vid, "%s", first);
    snprintf(cur_ytid, sizeof cur_ytid, "%s", first);
    cur_index = start;
    char single[128];
    snprintf(single, sizeof single, "https://www.youtube.com/watch?v=%s", first);
    double pos = pl_wait_pos + (cur_paused ? 0 : (now_ns() - pl_wait_at) / 1e9);
    mpv_load(single, cur_url, pos, cur_paused);
}

static void do_pause(int paused, double pos_s)
{
    if (!cur_url[0]) return;
    if (cur_live) {
        // live streams can't be paused and resumed in place: stop, and restart at the live edge
        if (paused && !cur_paused) {
            char keep[sizeof cur_url];
            snprintf(keep, sizeof keep, "%s", cur_url);
            const char *cmd[] = {"stop", NULL};
            mpv_command(mpv, cmd);
            kill_streamlink();
            snprintf(cur_url, sizeof cur_url, "%s", keep);
            cur_paused = 1;
            set_status(CSTV_PAUSED, "paused (live)");
            set_screen(SCR_PAUSED, "Paused", "it picks up live again when resumed");
        } else if (!paused && cur_paused) {
            char keep[sizeof cur_url];
            snprintf(keep, sizeof keep, "%s", cur_url);
            start_playback(keep, 0, 0);
        }
        return;
    }
    cur_paused = paused;
    // the position that comes with a resume is the server clock's, starting now
    if (paused || pos_s < 0) sync_at = 0;
    else { sync_pos = pos_s; sync_at = now_ns(); }
    if (!seek_wait) mpv_set_property(mpv, "pause", MPV_FORMAT_FLAG, &paused);
    if (pos_s >= 0 && fabs(shm->pos_s - pos_s) > 0.3) {
        char p[32];
        snprintf(p, sizeof p, "%.3f", pos_s);
        const char *cmd[] = {"seek", p, "absolute+exact", NULL};
        mpv_command(mpv, cmd);
    }
    set_status(paused ? CSTV_PAUSED : CSTV_PLAYING, paused ? "paused at %.1f s" : "playing from %.1f s", pos_s);
}

// Seeks hold the picture until enough is buffered, then play: one clean pause instead of
// play-stall-play. With report set, the game is told when it plays again (the server waits for that
// before running its clock, so nobody falls behind during the buffering).
static int seek_report;
static uint64_t seek_wait_at;
static int seek_running;        // the server's clock ran during the seek: catch up before playing
static double seek_target;

static void set_speed(double v)
{
    mpv_set_property(mpv, "speed", MPV_FORMAT_DOUBLE, &v);
}

// running: the server's clock keeps going meanwhile, so aim SEEK_LEAD s ahead of it and hold the picture
// there until the server arrives (fetching takes 1-5 s; landing early and waiting is exact, landing late isn't)
#define SEEK_LEAD 3.0
static void controlled_seek(double p, int report, int running)
{
    seek_target = p;
    seek_running = running;
    int yes = 1;
    mpv_set_property(mpv, "pause", MPV_FORMAT_FLAG, &yes);
    char ps[32];
    snprintf(ps, sizeof ps, "%.3f", running ? p + SEEK_LEAD * cur_speed : p);
    const char *sc[] = {"seek", ps, "absolute", NULL};
    mpv_command(mpv, sc);
    seek_wait = 1;
    seek_report = report;
    seek_wait_at = now_ns();
    if (catching) { catching = 0; set_speed(cur_speed); }
}

static void seek_tick(void)
{
    if (!seek_wait) return;
    int seeking = 1, idle = 0;
    double ahead = 0, waited = (now_ns() - seek_wait_at) / 1e9;
    mpv_get_property(mpv, "seeking", MPV_FORMAT_FLAG, &seeking);
    mpv_get_property(mpv, "demuxer-cache-duration", MPV_FORMAT_DOUBLE, &ahead);
    mpv_get_property(mpv, "demuxer-cache-idle", MPV_FORMAT_FLAG, &idle);
    if (getenv("CSTV_DEBUG")) logf_("seek wait %.1f s: seeking=%d ahead=%.2f idle=%d", waited, seeking, ahead, idle);
    if (waited > 10 && ahead < 0.1 && !idle) {   // nothing arrived: the links went bad, reload (main loop)
        stream_broken = 1;
        return;
    }
    if ((!seeking && waited > 0.3 && (ahead >= 1.5 || idle)) || waited > 10) {
        if (seek_running && !cur_paused) {   // where the server is now; usually already in the buffer = instant
            double at = 0, expected = seek_target + waited * cur_speed;
            mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &at);
            if (at > expected + 0.03 && at - expected < SEEK_LEAD * cur_speed + 1) return;   // early: hold until it gets here
            if (expected - at > 0.25 && expected < at + ahead - 0.5) {
                char ps[32];
                snprintf(ps, sizeof ps, "%.3f", expected + 0.05);
                const char *sc[] = {"seek", ps, "absolute+exact", NULL};
                mpv_command(mpv, sc);
                logf_("caught up %.1f s inside the buffer", expected - at);
            }
        }
        seek_wait = 0;
        int f = cur_paused;
        mpv_set_property(mpv, "pause", MPV_FORMAT_FLAG, &f);
        if (seek_report) master_send("started %ld\n", cur_rev);
        logf_("seek ready after %.1f s (%.1f s buffered)", waited, ahead);
    }
}

// Small differences (< 1.5 s) to the server's position are evened out by playing up to 5% faster or
// slower (pitch stays the same); bigger ones get one seek that holds until buffered, then lands exactly.
static void catchup_tick(void)
{
    if (!catching) return;
    if (!cur_url[0] || cur_live || cur_paused || seek_wait || shm->state != CSTV_PLAYING) {
        catching = 0;
        if (cur_url[0] && !cur_live) set_speed(cur_speed);
        return;
    }
    double expected = sync_pos + (now_ns() - sync_at) / 1e9 * cur_speed;
    double off = expected - shm->pos_s;   // > 0: we're behind
    if (fabs(off) < fmin(0.15, sync_tol / 3) || now_ns() - sync_at > 40000000000ull) {
        catching = 0;
        set_speed(cur_speed);
        logf_("in sync again (off by %.2f s)", off);
        return;
    }
    // <= 5% (6% when following tightly): barely audible, even on music
    double lim = sync_tol < 0.3 ? 0.06 : 0.05;
    double rate = 1 + fmax(-lim, fmin(lim, off / (sync_tol < 0.3 ? 4 : 10)));
    static double last_rate;
    if (fabs(rate - last_rate) > 0.01) { last_rate = rate; set_speed(cur_speed * rate); }
}

// YouTube & co. have every quality as its own track (ytdl_hook all_formats): pick the best video track
// at most max_h tall (0 = the best there is), preferring the codec already playing. -1 = none.
static int64_t pick_video_track_x(int max_h, const int64_t *excl, int nexcl);
static int64_t pick_video_track(int max_h) { return pick_video_track_x(max_h, NULL, 0); }
static int64_t pick_video_track_x(int max_h, const int64_t *excl, int nexcl)
{
    mpv_node tl;
    if (mpv_get_property(mpv, "track-list", MPV_FORMAT_NODE, &tl) < 0) return -1;
    char cur_codec[32] = "";
    int64_t best = -1, low = -1;
    int best_h = -1, best_rank = -1, low_h = 1 << 30;
    if (tl.format == MPV_FORMAT_NODE_ARRAY) {
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < tl.u.list->num; i++) {
                mpv_node *t = &tl.u.list->values[i];
                if (t->format != MPV_FORMAT_NODE_MAP) continue;
                const char *type = "", *codec = "", *title = "";
                int64_t id = -1, h = 0;
                int sel = 0, art = 0;
                for (int k = 0; k < t->u.list->num; k++) {
                    const char *key = t->u.list->keys[k];
                    mpv_node *v = &t->u.list->values[k];
                    if (!strcmp(key, "type") && v->format == MPV_FORMAT_STRING) type = v->u.string;
                    else if (!strcmp(key, "codec") && v->format == MPV_FORMAT_STRING) codec = v->u.string;
                    else if (!strcmp(key, "title") && v->format == MPV_FORMAT_STRING) title = v->u.string;
                    else if (!strcmp(key, "id") && v->format == MPV_FORMAT_INT64) id = v->u.int64;
                    else if (!strcmp(key, "demux-h") && v->format == MPV_FORMAT_INT64) h = v->u.int64;
                    else if (!strcmp(key, "selected") && v->format == MPV_FORMAT_FLAG) sel = v->u.flag;
                    else if (!strcmp(key, "albumart") && v->format == MPV_FORMAT_FLAG) art = v->u.flag;
                }
                if (strcmp(type, "video") || art) continue;
                int skip = 0;
                for (int e = 0; e < nexcl; e++) skip |= excl[e] == id;
                if (skip && pass == 1) continue;
                if (pass == 0) {
                    if (sel) snprintf(cur_codec, sizeof cur_codec, "%s", codec);
                    continue;
                }
                if (!h || !codec[0] || !strcmp(codec, "null")) continue;   // size or codec unknown
                if (strstr(title, "Premium") || strstr(title, "-sr ") || strstr(title, "AI-upscaled")) continue;   // see YT_FORMAT
                if (h < low_h) { low_h = (int)h; low = id; }
                if (max_h > 0 && h > max_h) continue;
                int rank = !strcmp(codec, cur_codec) ? 3 : !strcmp(codec, "vp9") ? 2 : !strcmp(codec, "av1") ? 1 : 0;
                if (strchr(title, '(')) rank += 10;   // "(720p)" = a regular DASH stream; HLS ones have no label
                if (h > best_h || (h == best_h && rank > best_rank)) { best = id; best_h = (int)h; best_rank = rank; }
            }
        }
    }
    mpv_free_node_contents(&tl);
    return best >= 0 ? best : low;   // nothing that small: the smallest there is
}

// A video track switch stalls playback while the new stream buffers (~2-3 s) and the server's clock
// keeps going: once it plays again, jump to where everyone else is (usually already buffered = instant).
static void resync_from_last(void)
{
    resync_pending = 0;
    if (!sync_at || !cur_url[0] || cur_paused || cur_live || seek_wait) return;
    double expected = sync_pos + (now_ns() - sync_at) / 1e9 * cur_speed, at = 0, ahead = 0;
    mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &at);
    mpv_get_property(mpv, "demuxer-cache-duration", MPV_FORMAT_DOUBLE, &ahead);
    double off = expected - at;
    if (fabs(off) < 0.25) return;
    if (off > 0 && off < ahead - 0.5) {
        char ps[32];
        snprintf(ps, sizeof ps, "%.3f", expected + 0.05);
        const char *sc[] = {"seek", ps, "absolute+exact", NULL};
        mpv_command(mpv, sc);
        logf_("back in step after the switch (%.1f s, inside the buffer)", off);
    } else {
        controlled_seek(expected, 0, 1);
        logf_("back in step after the switch (%.1f s)", off);
    }
}

// Pick English captions ourselves: mpv's "auto" can land on YouTube's live-style caption stream
// (hls_timedtext_playlist), which never shows anything. Hand-made English first, then the automatic
// English captions; "English from <other language>" translations are skipped. -1 = none.
static int64_t pick_sub_track(void)
{
    mpv_node tl;
    if (mpv_get_property(mpv, "track-list", MPV_FORMAT_NODE, &tl) < 0) return -1;
    int64_t best = -1;
    int best_score = 0;
    if (tl.format == MPV_FORMAT_NODE_ARRAY) {
        for (int i = 0; i < tl.u.list->num; i++) {
            mpv_node *t = &tl.u.list->values[i];
            if (t->format != MPV_FORMAT_NODE_MAP) continue;
            const char *type = "", *lang = "", *title = "", *file = "";
            int64_t id = -1;
            for (int k = 0; k < t->u.list->num; k++) {
                const char *key = t->u.list->keys[k];
                mpv_node *v = &t->u.list->values[k];
                if (v->format == MPV_FORMAT_STRING) {
                    if (!strcmp(key, "type")) type = v->u.string;
                    else if (!strcmp(key, "lang")) lang = v->u.string;
                    else if (!strcmp(key, "title")) title = v->u.string;
                    else if (!strcmp(key, "external-filename")) file = v->u.string;
                } else if (!strcmp(key, "id") && v->format == MPV_FORMAT_INT64) id = v->u.int64;
            }
            if (strcmp(type, "sub") || strstr(file, "hls_timedtext_playlist")) continue;
            int translated = strstr(title, " from ") != NULL, score = 0;
            if (!strcmp(lang, "en")) score = 4;
            else if ((!strncmp(lang, "en-", 3) || !strcmp(lang, "eng")) && !translated) score = 3;
            else if (strstr(title, "English") && !translated) score = 2;
            if (score > best_score) { best_score = score; best = id; }
        }
    }
    mpv_free_node_contents(&tl);
    return best;
}

static void apply_subs(int announce)
{
    if (!opt_subs) {
        mpv_set_property_string(mpv, "sid", "no");
        if (announce) toast("Captions off");
        return;
    }
    int64_t id = pick_sub_track();
    if (id < 0) {
        mpv_set_property_string(mpv, "sid", "no");
        if (announce) toast("No English captions for this video");
        return;
    }
    mpv_set_property(mpv, "sid", MPV_FORMAT_INT64, &id);
    if (announce) toast("Captions on");
}

static int switch_quality(const char *val)
{
    if (!strcmp(val, "audio")) {
        mpv_set_property_string(mpv, "vid", "no");
        set_screen(SCR_AUDIO, "Audio only", "pick a quality in !tvyt to bring the picture back");
        toast("Audio only");
        logf_("quality: audio only");
        return 1;
    }
    int autoq = !strcmp(val, "auto");
    int h = !strcmp(val, "source") ? 0 : autoq ? AUTO_MAX_H : atoi(val);
    int64_t id = pick_video_track(h);
    if (id < 0) return 0;
    switch_h = h;
    switch_tries = 0;
    switch_pos0 = shm->pos_s;
    if (screen_mode == SCR_AUDIO) screen_mode = SCR_VIDEO;
    mpv_set_property(mpv, "vid", MPV_FORMAT_INT64, &id);
    resync_pending = 1;
    resync_set_at = now_ns();
    if (autoq) toast("Quality: auto");
    else if (h) toast("Quality: %dp", h);
    else toast("Quality: best");
    logf_("quality: video track %lld (up to %dp), no reload", (long long)id, h);
    return 1;
}

// expected = where the server clock is now
static void check_drift(double expected)
{
    double t = shm->pos_s, off = expected - t;
    if (fabs(off) > fmax(1.5, sync_tol)) {
        controlled_seek(expected, 0, 1);
        logf_("resync: was %.1f s, server says %.1f s", t, expected);
    } else if (fabs(off) > sync_tol && !catching) {
        catching = 1;
        logf_("catching up: %.2f s %s", fabs(off), off > 0 ? "behind" : "ahead");
    }
}

// Between the server's syncs (every 15 s), check against where its clock must be by now: a buffering
// stall or a slow start is caught within a second instead of at the next sync. Only when following
// tightly (others are watching).
static void drift_tick(void)
{
    static uint64_t last;
    uint64_t now = now_ns();
    if (now - last < 1000000000ull) return;
    last = now;
    if (sync_tol >= 1.5 || catching || !sync_at || !cur_url[0] || cur_live || cur_paused || seek_wait || shm->state != CSTV_PLAYING)
        return;
    double expected = sync_pos + (now - sync_at) / 1e9 * cur_speed;
    if (cur_duration > 0 && expected > cur_duration - 1) return;
    check_drift(expected);
}

static void do_sync(double pos_s, int running)
{
    // keep the server's latest position even when we can't act on it now (used to aim reloads)
    sync_pos = pos_s;
    sync_at = running ? now_ns() : 0;   // a held clock (loading / seek) can't be extrapolated
    if (getenv("CSTV_DEBUG"))
        logf_("sync %.1f: url=%d live=%d paused=%d seek_wait=%d state=%d", pos_s, cur_url[0] != 0, cur_live, cur_paused, seek_wait, (int)shm->state);
    if (!cur_url[0] || cur_live || cur_paused || seek_wait || shm->state != CSTV_PLAYING) return;
    if (cur_duration > 0 && pos_s > cur_duration - 1) return;   // the video already ended; nothing to seek to
    if (!running) return;
    check_drift(pos_s);
}

// "ERROR: [youtube] ID: Private video. Sign in if you've been granted access to this video. Use --cookies..."
// -> a line for the error screen
static void keep_ytdl_error(const char *text)
{
    static const struct { const char *has, *say; } known[] = {
        {"Private video", "it's a private video"},
        {"confirm your age", "it's age-restricted (needs a signed-in account)"},
        {"not a bot", "YouTube wants a sign-in (bot check), try again later"},
        {"members-only", "it's for channel members only"},
        {"Join this channel", "it's for channel members only"},
        {"not made this video available in your country", "it's blocked in this country (VPN location?)"},
        {"Premieres in", "it hasn't premiered yet"},
        {"live event will begin", "the live stream hasn't started yet"},
        {"Unsupported URL", "this site isn't supported"},
    };
    for (size_t i = 0; i < sizeof known / sizeof *known; i++)
        if (strstr(text, known[i].has)) { snprintf(ytdl_err, sizeof ytdl_err, "%s", known[i].say); return; }
    const char *s = text + 7;   // past "ERROR: "
    if (*s == '[') {            // "[extractor] id: "
        const char *e = strstr(s, "]");
        const char *c = e ? strstr(e, ": ") : NULL;
        s = c ? c + 2 : (e ? e + 1 : s);
        while (*s == ' ') s++;
    }
    snprintf(ytdl_err, sizeof ytdl_err, "%s", s);
    char *cut = strstr(ytdl_err, " Use --");
    if (!cut) cut = strstr(ytdl_err, " See ");
    if (cut) *cut = 0;
    size_t n = strlen(ytdl_err);
    while (n && (ytdl_err[n - 1] == '\n' || ytdl_err[n - 1] == ' ' || ytdl_err[n - 1] == '.')) ytdl_err[--n] = 0;
    if (ytdl_err[0] >= 'A' && ytdl_err[0] <= 'Z' && (ytdl_err[1] < 'A' || ytdl_err[1] > 'Z')) ytdl_err[0] += 'a' - 'A';
}

// errors a fresh lookup can fix (network trouble); the rest (private, removed, ...) stay that way
static int ytdl_err_transient(void)
{
    return strstr(ytdl_err, "Unable to download") || strstr(ytdl_err, "timed out") || strstr(ytdl_err, "HTTP Error 5")
        || strstr(ytdl_err, "Temporary failure") || strstr(ytdl_err, "onnection");
}

static void handle_mpv_events(void)
{
    for (;;) {
        mpv_event *ev = mpv_wait_event(mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE) break;
        switch (ev->event_id) {
        case MPV_EVENT_PLAYBACK_RESTART:   // the newly opened video is playing (also after seeks)
            if (file_loaded) video_ready = 1;
            break;
        case MPV_EVENT_LOG_MESSAGE: {
            mpv_event_log_message *lm = ev->data;
            if (cur_url[0] && !cur_live && file_loaded && (strstr(lm->text, "403 Forbidden") || strstr(lm->text, "failed to load segment")))
                stream_broken = 1;
            if (!strcmp(lm->prefix, "ytdl_hook") && !strncmp(lm->text, "ERROR: ", 7) && !ytdl_err[0])
                keep_ytdl_error(lm->text);
#ifdef _WIN32
            // Linux sees mpv's warnings on stderr (the journal); on Windows that's an invisible console
            if (lm->log_level <= MPV_LOG_LEVEL_WARN || !strcmp(lm->prefix, "ytdl_hook")) {
                char line[1200];
                snprintf(line, sizeof line, "[%s] %s", lm->prefix, lm->text);
                os_log_line(line);
            }
#endif
            break;
        }
        case MPV_EVENT_FILE_LOADED: {
            logf_("opened after %.1f s", (now_ns() - loaded_at) / 1e9);
            file_loaded = 1;
            stream_broken = 0;   // anything flagged while the previous stream was being torn down
            double dur = 0;
            int has_dur = mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &dur) >= 0 && dur > 0;
            char *js = mpv_get_property_string(mpv, "user-data/mpv/ytdl/json-subprocess-result");
            if (js) {
                if (strstr(js, "\\\"is_live\\\": true") || strstr(js, "\"is_live\": true")) cur_live = 1;
                // youtube.com/@channel/live and similar links don't carry the video id: take it from yt-dlp's
                // output (mpv keeps it as a string inside a JSON object, so its quotes are escaped)
                if (!cur_ytid[0]) {
                    static const char esc[] = "{\\\"id\\\": \\\"", raw[] = "{\"id\": \"";
                    const char *idp = strstr(js, esc);
                    if (idp) idp += sizeof esc - 1;
                    else if (!strncmp(js, raw, sizeof raw - 1)) idp = js + sizeof raw - 1;
                    int ytx = strstr(js, "\\\"extractor\\\": \\\"youtube\\\"") || strstr(js, "\"extractor\": \"youtube\"");
                    if (idp && ytx) {
                        size_t i = 0;
                        for (; idp[i] && idp[i] != '"' && idp[i] != '\\' && i + 1 < sizeof cur_ytid; i++) cur_ytid[i] = idp[i];
                        cur_ytid[i] = 0;
                        if (!id_ok(cur_ytid)) cur_ytid[0] = 0;
                    }
                }
                mpv_free(js);
            }
            if (!has_dur) cur_live = 1;
            shm->live = cur_live;
            update_chat();
            if (pending_seek > 0 && !cur_live) {
                // the server's position was taken when it was sent; add the time loading took
                double p = pending_seek + (cur_paused || !pending_running ? 0 : (now_ns() - loaded_at) / 1e9);
                controlled_seek(p, 0, pending_running && !cur_paused);
            }
            if (opt_audio && !cur_live) mpv_set_property_string(mpv, "vid", "no");
            if (opt_subs && !cur_live) apply_subs(0);
            char *vid = mpv_get_property_string(mpv, "vid");
            if (vid) {
                if (!strcmp(vid, "no")) {   // audio only: no frames will come, so count it as started now
                    set_screen(SCR_AUDIO, "Audio only", "pick a quality in !tvyt to bring the picture back");
                    have_frame = 1;
                    frame_gen = load_gen;
                }
                mpv_free(vid);
            }
            pending_seek = -1;
            set_status(cur_paused ? CSTV_PAUSED : CSTV_PLAYING, "%s%s", cur_live ? "live: " : "", cur_url);
            break;
        }
        case MPV_EVENT_END_FILE: {
            mpv_event_end_file *ef = ev->data;
            if (ef->reason == MPV_END_FILE_REASON_ERROR) {
                if (cur_url[0] && !strncmp(cur_url, "rtmp://", 7) && live_retries < 20) {
                    // a CS:TV stream that isn't on the relay yet (or just dropped): wait for it, don't move on
                    live_retries++;
                    live_retry_at = now_ns() + 2000000000ull;
                    set_screen(SCR_LOADING, "Waiting for the stream", "it starts in a moment");
                    logf_("stream not there yet (%s), trying again in 2 s (%d/20)", mpv_error_string(ef->error), live_retries);
                } else if (cur_url[0] && !cur_live && !streamlink_running() && load_retries < 3 && (!ytdl_err[0] || ytdl_err_transient())) {
                    // YouTube now and then hands out a stream link that answers 403; a new lookup gets a working one
                    load_retries++;
                    logf_("could not play (%s), trying again (%d/3)", mpv_error_string(ef->error), load_retries);
                    char keep[sizeof cur_url];
                    snprintf(keep, sizeof keep, "%s", cur_url);
                    load_url(keep, req_pos + (cur_paused ? 0 : (now_ns() - req_at) / 1e9), cur_paused);
                } else {
                    set_status(CSTV_ERROR, "could not play: %s", ytdl_err[0] ? ytdl_err : mpv_error_string(ef->error));
                    const char *why = ytdl_err[0] ? ytdl_err : cur_live ? "the stream ended or is offline" : "the site didn't hand over a playable video";
                    set_screen(SCR_ERROR, "Couldn't play this", why);
                    if (cur_url[0] && !streamlink_running()) { want_next = 1; give_up(why); }   // move on (queue / playlist) instead of sitting on an error
                }
            }
            break;
        }
        case MPV_EVENT_PROPERTY_CHANGE: {
            mpv_event_property *pr = ev->data;
            if (!strcmp(pr->name, "time-pos") && pr->format == MPV_FORMAT_DOUBLE) shm->pos_s = *(double *)pr->data;
            if (!strcmp(pr->name, "duration")) cur_duration = pr->format == MPV_FORMAT_DOUBLE ? *(double *)pr->data : 0;
            if (!strcmp(pr->name, "eof-reached") && pr->format == MPV_FORMAT_FLAG && *(int *)pr->data && cur_url[0] && !cur_live) {
                if (cur_duration > 0 && shm->pos_s < cur_duration - 3) { if (file_loaded) stream_broken = 1; }   // "ended" early: the stream broke
                else want_next = 1;   // the video finished: tell the game (next playlist video, or the server's queue)
            }
            if (!strcmp(pr->name, "paused-for-cache") && pr->format == MPV_FORMAT_FLAG) {
                int on = *(int *)pr->data;
                if (on && !stall_start) stall_start = now_ns();
                else if (!on && stall_start) {
                    logf_("buffered for %.2f s (network couldn't keep up)", (now_ns() - stall_start) / 1e9);
                    stall_start = 0;
                }
            }
            break;
        }
        default:
            break;
        }
    }
}

// ------------------------------------------------------------------ base64url
static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

static int b64decode(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    int acc = 0, bits = 0;
    for (; *in && *in != '='; in++) {
        int v = b64val((unsigned char)*in);
        if (v < 0) return -1;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o + 1 >= outsz) return -1;
            out[o++] = (char)((acc >> bits) & 0xff);
        }
    }
    out[o] = 0;
    return (int)o;
}

// ------------------------------------------------------------------ per-player settings
static void apply_tol(long ms)   // tol <ms>: drift tolerance (see sync_tol)
{
    double t = ms / 1000.0;
    if (t >= 0.05 && t <= 60 && t != sync_tol) {
        sync_tol = t;
        logf_("sync tolerance %.2f s (%s)", t, t < 1.5 ? "others are watching too" : "watching alone");
        if (t >= 1.5 && catching) { catching = 0; set_speed(cur_speed); }
    }
}

static void apply_opt(const char *name, const char *val)
{
    if (!strcmp(name, "chat")) {
        opt_chat = atoi(val) != 0;
        update_chat();
    } else if (!strcmp(name, "subs")) {
        opt_subs = atoi(val) != 0;
        if (file_loaded && !cur_live) apply_subs(1);   // otherwise when the video opens
    } else if (!strcmp(name, "quality")) {
        // Twitch: a streamlink quality name; YouTube/other: a max height or "auto"
        char tw[128], yt[256];
        snprintf(yt, sizeof yt, "%s", opt_yt_format);   // audio only keeps the video format (picture comes back instantly)
        if (!strcmp(val, "source")) { snprintf(tw, sizeof tw, "best"); yt_format_for(0, yt, sizeof yt); }
        else if (!strcmp(val, "auto")) { snprintf(tw, sizeof tw, "%s", TW_AUTO); yt_format_for(AUTO_MAX_H, yt, sizeof yt); }
        else if (!strcmp(val, "audio")) { snprintf(tw, sizeof tw, "audio_only,worst"); }
        else {
            int h = atoi(val);
            if (h < 100 || h > 4320) return;
            snprintf(tw, sizeof tw, "%dp60,%dp30,%dp,best", h, h, h);
            yt_format_for(h, yt, sizeof yt);
        }
        int audio = !strcmp(val, "audio");
        int changed = strcmp(tw, opt_tw_quality) || strcmp(yt, opt_yt_format) || audio != opt_audio;
        snprintf(opt_tw_quality, sizeof opt_tw_quality, "%s", tw);
        snprintf(opt_yt_format, sizeof opt_yt_format, "%s", yt);
        opt_audio = audio;
        if (changed && cur_url[0]) {
            if (!cur_live && !streamlink_running() && file_loaded && switch_quality(val)) {
                // switched tracks in place
            } else {   // streamlink (Twitch / YouTube live): restart the stream in the new quality
                char keep[sizeof cur_url];
                snprintf(keep, sizeof keep, "%s", cur_url);
                double pos = cur_live ? 0 : shm->pos_s;
                load_note = "switching quality";
                start_playback(keep, pos, cur_paused);
                load_note = NULL;
            }
        }
    }
}

// ~/.config/cstv/prefs, key=value lines: the player's own settings (the master never sees them). The
// file may also set master=host:port and stream_cmd=<shell command, {url} = where to push> (replaces the
// in-game capture: a desktop capture of the player's choosing, desktop audio and all); the game can't
// change those two.
// hud: the HUD screen on/off; hud_x / hud_y / hud_w: its top left and width as fractions of the screen.
// opacity_screen / opacity_hud: placed screens (shared + personal) / the HUD screen, 10-100 %.
// universal: 1 = the video's sound at full volume wherever the player is (no fading with distance), like with
// the HUD screen on.
static const char *const pref_names[] = {"vol", "quality", "subs", "chat", "upnext", "hud", "hud_x", "hud_y", "hud_w", "opacity_screen", "opacity_hud",
                                         "chat_text", "universal"};
#define NPREF ((int)(sizeof pref_names / sizeof *pref_names))
static char pref_vals[NPREF][16] = {"100", "auto", "0", "0", "1", "0", "0.015", "0.32", "0.28", "100", "100", "normal", "0"};
static char pref_hub[512], pref_stream_cmd[1024];   // hub=wss://... (another hub), stream_cmd=...
static char prefs_path[1100];

static int pref_valid(const char *name, const char *v)
{
    if (!strcmp(name, "vol")) {
        char *e;
        long x = strtol(v, &e, 10);
        return *v && !*e && x >= 0 && x <= 100;
    }
    if (!strcmp(name, "chat_text")) {   // the Lobby page chat's text size
        static const char *const ok[] = {"smaller", "small", "normal", "large", "larger", "largest"};   // small = old normal
        for (size_t i = 0; i < sizeof ok / sizeof *ok; i++)
            if (!strcmp(v, ok[i])) return 1;
        return 0;
    }
    if (!strncmp(name, "opacity_", 8)) {
        char *e;
        long x = strtol(v, &e, 10);
        return *v && !*e && x >= 10 && x <= 100;
    }
    if (!strcmp(name, "quality")) {
        static const char *const q[] = {"auto", "source", "1440", "1080", "720", "480", "360"};
        for (size_t i = 0; i < sizeof q / sizeof *q; i++)
            if (!strcmp(v, q[i])) return 1;
        return 0;
    }
    if (!strncmp(name, "hud_", 4)) {
        char *e;
        double x = strtod(v, &e);
        return *v && !*e && strlen(v) < 16 && x >= 0 && x <= 1;
    }
    return !strcmp(v, "0") || !strcmp(v, "1");
}

static void prefs_load(void)
{
#ifdef _WIN32
    const char *ad = getenv("APPDATA");
    snprintf(prefs_path, sizeof prefs_path, "%s\\cstv\\prefs", ad ? ad : ".");
#else
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    if (x && *x) snprintf(prefs_path, sizeof prefs_path, "%s/cstv/prefs", x);
    else snprintf(prefs_path, sizeof prefs_path, "%s/.config/cstv/prefs", h ? h : ".");
#endif
    FILE *f = os_fopen(prefs_path, "r");
    if (!f) return;
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *v = eq + 1;
        if (!strcmp(line, "hub")) snprintf(pref_hub, sizeof pref_hub, "%s", v);
        else if (!strcmp(line, "stream_cmd")) snprintf(pref_stream_cmd, sizeof pref_stream_cmd, "%s", v);
        else
            for (int i = 0; i < NPREF; i++)
                if (!strcmp(line, pref_names[i]) && pref_valid(line, v)) snprintf(pref_vals[i], sizeof pref_vals[i], "%s", v);
    }
    fclose(f);
}

static void mkdirs_for(const char *path)
{
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", path);
    for (char *p = dir + 1; *p; p++) {   // create the folders on the way (~/.config may not exist yet)
        if (*p != '/' && *p != '\\') continue;
        char c = *p;
        *p = 0;
        os_mkdir(dir);
        *p = c;
    }
}

static void prefs_save(void)
{
    char tmp[1110];
    mkdirs_for(prefs_path);
    snprintf(tmp, sizeof tmp, "%s.new", prefs_path);
    FILE *f = os_fopen(tmp, "w");
    if (!f) { logf_("cannot write %s", tmp); return; }
    for (int i = 0; i < NPREF; i++) fprintf(f, "%s=%s\n", pref_names[i], pref_vals[i]);
    if (pref_hub[0]) fprintf(f, "hub=%s\n", pref_hub);
    if (pref_stream_cmd[0]) fprintf(f, "stream_cmd=%s\n", pref_stream_cmd);
    if (fclose(f)) { logf_("cannot write %s", tmp); return; }
#ifdef _WIN32
    remove(prefs_path);   // rename doesn't replace there
#endif
    if (rename(tmp, prefs_path)) logf_("cannot replace %s", prefs_path);
}

static void prefs_send(void)
{
    for (int i = 0; i < NPREF; i++) game_send("pref %s %s\n", pref_names[i], pref_vals[i]);
}

static void set_pref(const char *name, const char *v)
{
    for (int i = 0; i < NPREF; i++) {
        if (strcmp(name, pref_names[i]) || !pref_valid(name, v)) continue;
        if (strcmp(pref_vals[i], v)) {
            snprintf(pref_vals[i], sizeof pref_vals[i], "%s", v);
            prefs_save();
            if (!strcmp(name, "quality") || !strcmp(name, "subs") || !strcmp(name, "chat")) apply_opt(name, v);
        }
        game_send("pref %s %s\n", name, v);
        return;
    }
}

// ------------------------------------------------------------------ personal screens
// ~/.config/cstv/screens (next to prefs): the player's own screen per "<server>/<map>", one
// "<key> <scale> <float> <pos xyz> <normal xyz> <right xyz> <up xyz>" line each. Only this player sees
// them; the master never hears of them. The game asks for the one where it is (`pscreen <key>`) and
// sets or removes it; every pscreen line gets the current state back.
#define MAX_PSCREENS 1000
static struct { char key[136], geo[200]; } pscreens[MAX_PSCREENS];
static int npscreens;
static char screens_path[1100];

static int pscreen_key_ok(const char *k)
{
    size_t n = strlen(k);
    if (n < 3 || n > 130 || !strchr(k, '/')) return 0;
    for (; *k; k++)
        if (!isalnum((unsigned char)*k) && !strchr(".:[]_-/", *k)) return 0;
    return 1;
}

// "<scale> <float> <12 numbers>" written out again the same way, or 0 if it isn't one
static int pscreen_geo(const char *in, char *out, size_t cap)
{
    double v[14];
    const char *p = in;
    for (int i = 0; i < 14; i++) {
        char *e;
        v[i] = strtod(p, &e);
        if (e == p || !isfinite(v[i])) return 0;
        p = e;
    }
    while (*p == ' ' || *p == '\r') p++;
    if (*p || v[0] < 0.05 || v[0] > 4 || (v[1] != 0 && v[1] != 1)) return 0;
    for (int i = 2; i < 14; i++)
        if (fabs(v[i]) > (i < 5 ? 65536 : 1.01)) return 0;
    int n = snprintf(out, cap, "%g %d", v[0], (int)v[1]);
    for (int i = 2; i < 14 && n > 0 && (size_t)n < cap; i++) n += snprintf(out + n, cap - n, i < 5 ? " %.1f" : " %.4f", v[i]);
    return n > 0 && (size_t)n < cap;
}

static void pscreens_load(void)
{
    size_t n = strlen(prefs_path);   // ".../cstv/prefs" -> ".../cstv/screens"
    snprintf(screens_path, sizeof screens_path, "%.*sscreens", (int)(n >= 5 ? n - 5 : 0), prefs_path);
    FILE *f = os_fopen(screens_path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f) && npscreens < MAX_PSCREENS) {
        line[strcspn(line, "\r\n")] = 0;
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        if (pscreen_key_ok(line) && pscreen_geo(sp + 1, pscreens[npscreens].geo, sizeof pscreens[npscreens].geo)) {
            snprintf(pscreens[npscreens].key, sizeof pscreens[npscreens].key, "%s", line);
            npscreens++;
        }
    }
    fclose(f);
}

static void pscreens_save(void)
{
    char tmp[1110];
    mkdirs_for(screens_path);
    snprintf(tmp, sizeof tmp, "%s.new", screens_path);
    FILE *f = os_fopen(tmp, "w");
    if (!f) { logf_("cannot write %s", tmp); return; }
    for (int i = 0; i < npscreens; i++) fprintf(f, "%s %s\n", pscreens[i].key, pscreens[i].geo);
    if (fclose(f)) { logf_("cannot write %s", tmp); return; }
#ifdef _WIN32
    remove(screens_path);
#endif
    if (rename(tmp, screens_path)) logf_("cannot replace %s", screens_path);
}

// rest: "" = just tell, "none" = remove, else the geometry to keep
static void pscreen_cmd(const char *key, const char *rest)
{
    if (!pscreen_key_ok(key)) return;
    int i = 0;
    while (i < npscreens && strcmp(pscreens[i].key, key)) i++;
    if (i == npscreens) i = -1;
    if (!strcmp(rest, "none")) {
        if (i >= 0) {
            pscreens[i] = pscreens[--npscreens];
            pscreens_save();
        }
        i = -1;
    } else if (*rest) {
        char geo[sizeof pscreens[0].geo];
        if (!pscreen_geo(rest, geo, sizeof geo)) return;
        if (i < 0 && npscreens < MAX_PSCREENS) {
            i = npscreens++;
            snprintf(pscreens[i].key, sizeof pscreens[i].key, "%s", key);
        }
        if (i < 0) { logf_("%s is full", screens_path); return; }
        snprintf(pscreens[i].geo, sizeof pscreens[i].geo, "%s", geo);
        pscreens_save();
    }
    game_send("pscreen %s %s\n", key, i >= 0 ? pscreens[i].geo : "none");
}

// ------------------------------------------------------------------ title lookups
static void start_title_job(const char *uid, const char *url_b64)
{
    char url[2048];
    struct title_job *j;
    const char *d = uid;
    while (*d >= '0' && *d <= '9') d++;
    if (!*uid || *d || strlen(uid) > 20 || b64decode(url_b64, url, sizeof url) < 0 || !url_syntax_ok(url)) return;
    if (!(j = calloc(1, sizeof *j))) return;
    snprintf(j->url, sizeof j->url, "%s", url);
    snprintf(j->uid, sizeof j->uid, "%s", uid);
    pthread_t t;
    if (pthread_create(&t, NULL, title_thread, j) == 0) pthread_detach(t);
    else free(j);
}

// ------------------------------------------------------------------ YouTube search for the game's menu
struct search_job { char id[24]; char q[300]; };
static atomic_int searches;

static void *search_thread(void *arg)
{
    struct search_job *j = arg;
    const size_t cap = 256 * 1024;
    char *out = malloc(cap), yt[1100], term[320];
    size_t len = 0;
    int rc = -1;
    snprintf(term, sizeof term, "ytsearch15:%s", j->q);
    // \x1f between the fields: titles can hold anything else
    const char *argv[] = {tool("yt-dlp", YTDLP_REL, yt, sizeof yt), "--no-cache-dir", "--no-warnings", "--flat-playlist",
                          "--print", "%(url)s\x1f%(title)s\x1f%(channel)s\x1f%(duration)s", "--", term, NULL};
    os_proc pr;
    int fd;
    if (out && os_spawn(argv, NULL, OS_OUT_PIPE, &fd, &pr) == 0) {
        int r;
        while (len + 1 < cap && (r = (int)read(fd, out + len, (unsigned)(cap - 1 - len))) > 0) len += (size_t)r;
        close(fd);
        rc = os_proc_wait(&pr);
    }
    int n = 0;
    pthread_mutex_lock(&pl_mu);   // game_send from a thread: keep lines whole
    if (out) {
        out[len] = 0;
        char *save = NULL;
        for (char *line = strtok_r(out, "\n", &save); line && n < 15; line = strtok_r(NULL, "\n", &save)) {
            char *f[4] = {line, NULL, NULL, NULL};
            for (int k = 1; k < 4; k++) {
                f[k] = f[k - 1] ? strchr(f[k - 1], '\x1f') : NULL;
                if (f[k]) *f[k]++ = 0;
            }
            if (!f[3] || !url_syntax_ok(f[0])) continue;
            char bu[2800], bt[400], bc[200], dur[16] = "-";
            if (!strcmp(f[2], "NA")) f[2] = "";
            b64encode(f[0], bu, sizeof bu);
            b64encode(strcmp(f[1], "NA") ? f[1] : "", bt, sizeof bt);
            b64encode(f[2], bc, sizeof bc);
            if (strcmp(f[3], "NA") && atof(f[3]) > 0) snprintf(dur, sizeof dur, "%ld", (long)atof(f[3]));
            game_send("result %s %d %s %s %s %s\n", j->id, n, bu, bt[0] ? bt : "-", bc[0] ? bc : "-", dur);
            n++;
        }
    }
    if (n || rc == 0) {
        game_send("results_end %s %d\n", j->id, n);
    } else {
        char be[64];
        b64encode("search failed", be, sizeof be);
        game_send("results_end %s %s\n", j->id, be);
    }
    pthread_mutex_unlock(&pl_mu);
    logf_("search %s: %d results", j->id, n);
    free(out);
    free(j);
    searches--;
    return NULL;
}

static void start_search(const char *id, const char *q_b64)
{
    char q[300];
    if (!id_ok(id) || strlen(id) > 20 || b64decode(q_b64, q, sizeof q) <= 0) return;
    for (char *p = q; *p; p++)
        if ((unsigned char)*p < ' ') *p = ' ';
    if (searches >= 2) {   // someone typing fast: don't pile up yt-dlp processes
        char be[64];
        b64encode("busy, try again", be, sizeof be);
        game_send("results_end %s %s\n", id, be);
        return;
    }
    struct search_job *j = calloc(1, sizeof *j);
    if (!j) return;
    snprintf(j->id, sizeof j->id, "%s", id);
    snprintf(j->q, sizeof j->q, "%s", q);
    searches++;
    pthread_t t;
    if (pthread_create(&t, NULL, search_thread, j) == 0) pthread_detach(t);
    else { searches--; free(j); }
}

// ------------------------------------------------------------------ the CS:TV hub (cstv.xyz)
// One connection while a game is connected (and has said who it is). cstv_link.py (Python, standard library
// only) holds the WebSocket to the hub and passes lines both ways on its stdin / stdout: it prints "#open"
// once connected and ends with the connection ("#closed <why>"); we start a new one 1 s .. 30 s later. It
// also keeps the sign-in token to itself. Everything the hub says goes to the game as "srv <line>"; cstvd
// itself answers started / ended / title. (daf's version: plain TCP to his master; the lines are the same.)
// The link's output is read on a thread (Windows can't poll pipes) and handed over through the wake pipe.
enum { M_OFF, M_CONNECTING, M_UP };
static int m_state;
static os_proc m_proc;
static int m_in = -1;                // the link's stdin
static pthread_mutex_t m_mu = PTHREAD_MUTEX_INITIALIZER;   // m_in, m_state, m_rx*: title lookups send from threads
static char *m_rx;                   // complete and partial lines from the link, not handled yet
static size_t m_rxlen;
static int m_rx_eof;                 // the link's stdout ended
static unsigned m_gen;               // which link the reader thread's data belongs to
static int m_broken;                 // a write failed (seen by a thread): the main loop closes
static char m_why[200];              // the link's "#closed" reason
static uint64_t m_next_try, m_backoff = 1000000000ull, m_last_ping, m_last_rx, m_since;
static int m_welcomed;
static double m_rtt = -1;            // smoothed round trip in ms, -1 = not measured yet
static char ident_id[24], ident_name[200];   // from the game's ident
static char where_line[600];                 // the game's last "where ..."
static char lobby_code[16];
static char lobby_pass[100] = "-";   // b64, as the player gave it: the rejoin after a reconnect needs it
static long master_rev;

static void cap_stop(void);
static int capturing(void);
static void stream_state(const char *st, const char *text);
static uint64_t stream_asked;

struct m_reader_arg { int fd; unsigned gen; };

static void *m_reader(void *arg)
{
    struct m_reader_arg a = *(struct m_reader_arg *)arg;
    free(arg);
    char buf[8192];
    for (;;) {
        int r = (int)read(a.fd, buf, sizeof buf);
        pthread_mutex_lock(&m_mu);
        if (a.gen != m_gen) { pthread_mutex_unlock(&m_mu); break; }   // a newer link took over
        if (r <= 0) {
            m_rx_eof = 1;
        } else if (m_rxlen + r < (4u << 20)) {   // a stuck main loop: drop rather than grow forever
            char *nb = realloc(m_rx, m_rxlen + r + 1);
            if (nb) {
                m_rx = nb;
                memcpy(m_rx + m_rxlen, buf, r);
                m_rxlen += r;
                m_rx[m_rxlen] = 0;
            }
        }
        pthread_mutex_unlock(&m_mu);
        os_wake();
        if (r <= 0) break;
    }
    close(a.fd);
    return NULL;
}

static void master_send(const char *fmt, ...)
{
    char buf[1024], *p = buf;
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof buf && (p = malloc((size_t)n + 1))) vsnprintf(p, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    if (n <= 0 || !p) return;
    pthread_mutex_lock(&m_mu);
    if (m_state != M_UP || m_in < 0) logf_("not connected to the hub, dropped: %.*s", n - 1, p);
    else if (write(m_in, p, (size_t)n) != n) m_broken = 1;
    pthread_mutex_unlock(&m_mu);
    if (p != buf) free(p);
}

static void m_close(const char *why)
{
    pthread_mutex_lock(&m_mu);
    if (m_in >= 0) close(m_in);   // the link ends when its stdin does
    m_in = -1;
    m_state = M_OFF;
    m_gen++;                       // the old reader's data is no longer wanted
    free(m_rx);
    m_rx = NULL;
    m_rxlen = 0;
    m_rx_eof = 0;
    m_broken = 0;
    pthread_mutex_unlock(&m_mu);
    os_proc_kill(&m_proc, 300);
    if (m_welcomed) game_send("master 0\n");
    m_welcomed = 0;
    stream_host[0] = 0;
    if (capturing() || stream_asked) {
        cap_stop();
        stream_asked = 0;
        stream_state("error", "lost the connection to the CS:TV server");
    }
    if (why) {
        m_next_try = now_ns() + m_backoff;
        logf_("hub %s: %s, again in %llu s", hub_url, m_why[0] ? m_why : why, (unsigned long long)(m_backoff / 1000000000ull));
        m_backoff = m_backoff * 2 > 30000000000ull ? 30000000000ull : m_backoff * 2;
    }
    m_why[0] = 0;
}

static void m_connected(void)
{
    pthread_mutex_lock(&m_mu);
    m_state = M_UP;
    pthread_mutex_unlock(&m_mu);
    m_last_rx = now_ns();
    m_last_ping = 0;
    logf_("connected to the hub %s", hub_url);
    master_send("hello 2 %s %s\n", ident_id, ident_name);
    if (where_line[0]) master_send("%s\n", where_line);
    if (lobby_code[0]) master_send("lobby join %s %s\n", lobby_code, lobby_pass);
}

static void m_start(void)
{
    char script[1100], py[1100];
    os_exe_path(script, sizeof script, "cstv_link.py");
    const char *argv[] = {tool("python3", PY_REL, py, sizeof py), script, hub_url, NULL};
    int in = -1, out = -1;
    m_since = now_ns();
    if (os_spawn(argv, &in, OS_OUT_PIPE, &out, &m_proc)) { m_close("could not start cstv_link.py"); return; }
    struct m_reader_arg *a = malloc(sizeof *a);
    pthread_mutex_lock(&m_mu);
    m_in = in;
    m_state = M_CONNECTING;
    unsigned gen = m_gen;
    pthread_mutex_unlock(&m_mu);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (!a || (a->fd = out, a->gen = gen, pthread_create(&t, &at, m_reader, a))) {
        free(a);
        close(out);
        m_close("no reader thread");
    }
    pthread_attr_destroy(&at);
}

static void master_line(char *line);

// the link's lines: "#open", "#closed <why>", the hub's lines
static void m_drain(void)
{
    for (;;) {
        char line[1 << 16];
        int eof = 0, got = 0;
        pthread_mutex_lock(&m_mu);
        char *nl = m_rx ? strchr(m_rx, '\n') : NULL;
        if (nl) {
            size_t n = (size_t)(nl - m_rx);
            if (n < sizeof line) {
                memcpy(line, m_rx, n);
                line[n] = 0;
                got = 1;
            }
            m_rxlen -= n + 1;
            memmove(m_rx, nl + 1, m_rxlen + 1);
        } else {
            eof = m_rx_eof;
        }
        pthread_mutex_unlock(&m_mu);
        if (!nl) {
            if (eof && m_state != M_OFF) m_close("the link ended");
            return;
        }
        if (!got) { logf_("hub line too long, dropped"); continue; }
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[--len] = 0;   // Windows Python text mode
        if (!strcmp(line, "#open")) {
            if (m_state == M_CONNECTING) m_connected();
        } else if (!strncmp(line, "#closed", 7)) {
            snprintf(m_why, sizeof m_why, "%s", line[7] ? line + 8 : "closed");
        } else if (m_state == M_UP && len) {
            m_last_rx = now_ns();
            master_line(line);
        }
        if (m_state == M_OFF) return;   // closed while handling a line
    }
}

// called every loop: connect while a game is connected, keep the connection alive
static void m_tick(void)
{
    uint64_t now = now_ns();
    if (m_broken) m_close("connection lost");
    if (game_fd == SOCK_BAD || !ident_id[0]) {
        if (m_state != M_OFF) m_close(NULL);
        m_backoff = 1000000000ull;
        m_next_try = 0;
        return;
    }
    if (m_state != M_OFF) {
        m_drain();
        now = now_ns();   // m_drain may have just stamped m_last_rx / m_since later than the first reading
    }
    if (m_state == M_OFF && now >= m_next_try) m_start();
    else if (m_state == M_CONNECTING && now - m_since > 20000000000ull) m_close("connect timed out");
    else if (m_state == M_UP) {
        if (now - m_last_rx > 25000000000ull) m_close("no answer for 25 s");
        else if (now - m_last_ping > 5000000000ull) {
            m_last_ping = now;
            master_send("ping %llu\n", (unsigned long long)(now / 1000000));
        }
    }
}

static void stream_go(const char *url);

// The renderer's answers to the Lobby page's pickers: pick <name>, gifpick <id>, gifmore <query> <offset>.
static void overlay_poll(void)
{
    static char buf[4096];
    static size_t len;
    if (overlay_out < 0) return;
    ssize_t r;
#ifdef _WIN32
    DWORD avail;   // a pipe read would block: only read what's there
    while (PeekNamedPipe((HANDLE)_get_osfhandle(overlay_out), NULL, 0, NULL, &avail, NULL) && avail
           && (r = read(overlay_out, buf + len, avail < sizeof buf - 1 - len ? avail : sizeof buf - 1 - len)) > 0) {
#else
    while ((r = read(overlay_out, buf + len, sizeof buf - 1 - len)) > 0) {
#endif
        len += (size_t)r;
        buf[len] = 0;
        char *nl;
        while ((nl = strchr(buf, '\n'))) {
            *nl = 0;
            char verb[16] = "", a1[600] = "";
            long off = 0;
            int n = sscanf(buf, "%15s %599s %ld", verb, a1, &off);
            if (n >= 2 && line_safe(a1)) {
                if (!strcmp(verb, "pick")) game_send("lchat_pick %s\n", a1);
                else if (!strcmp(verb, "gifpick")) game_send("lchat_gifpick %s\n", a1);
                else if (n >= 3 && !strcmp(verb, "gifmore") && m_state == M_UP) master_send("gifs %s %ld\n", a1, off);
            }
            size_t used = (size_t)(nl + 1 - buf);
            memmove(buf, nl + 1, len - used + 1);
            len -= used;
        }
        if (len >= sizeof buf - 1) len = 0;   // a line too long for us: drop it
    }
}

static void master_line(char *line)
{
    char verb[16] = "", a1[2100] = "";
    if (sscanf(line, "%15s %2099s", verb, a1) < 1) return;
    if (!strcmp(verb, "state")) {
        // the clock as it was when sent: while it runs, it's half a round trip further by now
        long rev, paused, pos, speed, seek_id, running;
        if (sscanf(line, "state %ld %ld %ld %ld %ld %ld", &rev, &paused, &pos, &speed, &seek_id, &running) == 6 && running == 1 && !paused && m_rtt > 0) {
            pos += (long)(m_rtt / 2 * speed / 100);
            game_send("srv state %ld %ld %ld %ld %ld %ld\n", rev, paused, pos, speed, seek_id, running);
        } else {
            game_send("srv %s\n", line);
        }
        return;
    }
    game_send("srv %s\n", line);
    if (!strncmp(verb, "lchat", 5) || !strcmp(verb, "gifs")) {   // the Lobby page's chat and GIF picker
        lchat_remember(line);
        overlay_line("hub", line);
    }
    if (!strcmp(verb, "welcome")) {
        m_welcomed = 1;
        m_backoff = 1000000000ull;
        game_send("master 1\n");
    } else if (!strcmp(verb, "pong")) {
        double rtt = (double)(now_ns() / 1000000) - atof(a1);
        if (rtt >= 0 && rtt < 60000) m_rtt = m_rtt < 0 ? rtt : m_rtt * 0.8 + rtt * 0.2;
    } else if (!strcmp(verb, "room")) {   // room <srv|lobby> <name> ...: rejoin the lobby after a reconnect
        char name[64] = "";
        sscanf(line, "%*s %*s %63s", name);
        if (!strcmp(a1, "lobby") && strlen(name) < sizeof lobby_code && id_ok(name)) snprintf(lobby_code, sizeof lobby_code, "%s", name);
        else lobby_code[0] = 0;
    } else if (!strcmp(verb, "streamhost")) {   // where the hub relays streams: rtmp:// links from there are ours
        snprintf(stream_host, sizeof stream_host, "%s", strcmp(a1, "-") ? a1 : "");
        for (char *c = stream_host; *c; c++) *c = (char)tolower((unsigned char)*c);
    } else if (!strcmp(verb, "url")) {
        master_rev = atol(a1);
    } else if (!strcmp(verb, "tol")) {
        apply_tol(atol(a1));
    } else if (!strcmp(verb, "skip")) {
        if (cur_url[0]) { next_sent = 0; want_next = 1; }
    } else if (!strcmp(verb, "title")) {
        char ub[2100] = "";
        if (sscanf(line, "%*s %*s %2099s", ub) == 1) start_title_job(a1, ub);
    } else if (!strcmp(verb, "upnext")) {
        char url[2048];
        if (b64decode(a1, url, sizeof url) > 0 && url_syntax_ok(url)) prefetch(url);
    } else if (!strcmp(verb, "stream_go")) {
        char url[2048];
        stream_asked = 0;
        if (b64decode(a1, url, sizeof url) > 0) stream_go(url);
    } else if (!strcmp(verb, "stream_end")) {
        stream_asked = 0;
        cap_stop();
        stream_state("idle", "the stream ended");
    }
}

// "m <line>" from the game: only what a player may say to the hub (started / ended / title / hello and
// stream are cstvd's own; auth / token stay inside cstv_link.py)
static int m_allowed(const char *l)
{
    static const char *const ok[] = {"where", "place", "remove", "link", "queue", "unqueue", "playnow", "clearqueue",
                                     "next", "prev", "pause", "seek", "speed", "loop",
                                     "login", "logout", "status", "chat", "lobbies", "gifs", "chatgif"};   // cstv.xyz: sign-in, lobby chat, lists
    static const char *const lobby[] = {"create", "join", "leave", "kick", "ban", "unban", "owner", "perm", "name", "password",
                                        "set", "delete"};
    char v[16] = "", sub[16] = "";
    sscanf(l, "%15s %15s", v, sub);
    for (size_t i = 0; i < sizeof ok / sizeof *ok; i++)
        if (!strcmp(v, ok[i])) return 1;
    if (!strcmp(v, "lobby"))
        for (size_t i = 0; i < sizeof lobby / sizeof *lobby; i++)
            if (!strcmp(sub, lobby[i])) return 1;
    return 0;
}

// ------------------------------------------------------------------ streaming this player's game to the room
// "stream start" -> master answers stream_go <rtmp url> -> cstvd creates the capture buffer and tells the
// game `capture 1` -> the game fills it with its own picture and sound mix, a writer thread feeds that to
// ffmpeg at a fixed 60 fps -> after ~2 s of pushing it's "live". Only the game's own sound goes out, never
// the desktop's (Discord, notifications, our own video). stream_cmd= in the prefs replaces all of it with
// the player's own command (a desktop capture, with whatever audio it grabs).
static char stream_st[16] = "idle", stream_txt[200];
static uint64_t cap_at;     // capture started (`capture 1` sent)
static int cap_live;
#ifndef _WIN32
#define CAP_FPS 60
static pid_t cap_pid;
static uint64_t enc_at;     // encoder started
static struct cstv_cap_shm *cap;
static char cap_err[200];
static int cap_vfd = -1, cap_afd = -1, cap_efd = -1;   // ffmpeg's video and audio input, its stderr
static int cap_rate, cap_ch;
static pthread_t cap_thr;
static atomic_int cap_run;
enum { ENC_X264, ENC_VAAPI, ENC_NVENC };
static atomic_int hw_enc = -1;   // which encoder streams use (ENC_*); -1 = not probed yet
static int enc_probing;

// Every 1/60 s: that tick's worth of the game's sound, and a picture (the newest game frame, the previous one
// again if there's none). Both ffmpeg inputs are timed by count, so for sync the two must keep the same number of
// ticks. The sound always goes out: ffmpeg holds back the video while its sound is behind (its scheduler keeps
// the streams within ~0.1 s), so holding the sound back whenever the video wasn't taken deadlocks it -- the
// first versions dropped ticks whole and a stream sent ~5 frames, then nothing (2026-10-18). The video owes a
// frame per tick and pays it back as soon as ffmpeg takes them; only when it is CAP_LAG frames behind (the
// encoder can't keep up; ffmpeg is then waiting on the video, not the sound) are ticks dropped whole.
#define CAP_LAG CAP_FPS
static void *cap_writer(void *arg)
{
    (void)arg;
    const size_t fb = CSTV_CAP_FRAME_BYTES, tick_max = ((size_t)cap_rate / CAP_FPS + 1) * cap_ch;
    const size_t acap = (size_t)cap_rate * cap_ch * 2;   // bytes: at most 1 s of sound waiting for ffmpeg
    uint8_t *vbuf = calloc(1, fb), *abuf = malloc(acap);
    int16_t *tick = malloc(tick_max * 2);
    size_t voff = fb, abeg = 0, aend = 0;   // bytes of the pending frame written; pending sound = abuf[abeg, aend)
    uint64_t seq = 0, rd = __atomic_load_n(&cap->audio_written, __ATOMIC_ACQUIRE), n = 0, k = 0, dropped = 0, frames = 0;
    int owed = 0;   // frames the video is behind the ticks
    uint64_t base = now_ns();
    // the game's audio comes in callback-sized bursts: keep ~80 ms in hand, never fall more than 250 ms behind
    const uint64_t cushion = cap_rate / 12, most = cap_rate / 4;
    int primed = 0;
    while (atomic_load(&cap_run) && vbuf && abuf && tick) {
        uint64_t now = now_ns(), next = base + k * 1000000000ull / CAP_FPS;
        if (now >= next) {
            if (now - next > 200000000ull) { base = now; k = 0; }   // we were stalled: those ticks are gone
            k++;
            size_t need = (size_t)((n + 1) * cap_rate / CAP_FPS - n * cap_rate / CAP_FPS);
            n++;
            uint64_t w = __atomic_load_n(&cap->audio_written, __ATOMIC_ACQUIRE);
            int fmt_ok = cap->audio_rate == (uint32_t)cap_rate && cap->audio_channels == (uint32_t)cap_ch;
            if (!fmt_ok || w < rd) { rd = w; primed = 0; }
            if (w - rd > most) rd = w - cushion;   // also covers falling more than the ring behind
            if (!primed && w - rd >= cushion) primed = 1;
            size_t got = 0;
            if (primed) {
                got = w - rd < need ? (size_t)(w - rd) : need;
                for (size_t i = 0; i < got; i++, rd++)
                    memcpy(tick + i * cap_ch, cap->audio + (rd % CSTV_CAP_AUDIO_FRAMES) * cap_ch, cap_ch * 2);
                if (got < need) primed = 0;   // ran dry (the game stalled): silence until the cushion is back
            }
            memset(tick + got * cap_ch, 0, (need - got) * cap_ch * 2);
            if (owed >= CAP_LAG) {
                dropped++;   // the tick's sound read from the game and thrown away, so it stays current
            } else {
                size_t len = need * cap_ch * 2;
                if (aend + len > acap) {   // make room: move what's pending to the front, else lose the oldest
                    memmove(abuf, abuf + abeg, aend - abeg);
                    aend -= abeg;
                    abeg = 0;
                    if (aend + len > acap) { abeg = aend + len - acap; memmove(abuf, abuf + abeg, aend - abeg); aend -= abeg; abeg = 0; }
                }
                memcpy(abuf + aend, tick, len);
                aend += len;
                owed++;
            }
        }
        if (voff >= fb && owed > 0) {   // the next picture: the newest game frame, or the last one again
            uint64_t s = __atomic_load_n(&cap->seq, __ATOMIC_ACQUIRE);
            uint32_t l = __atomic_load_n(&cap->latest, __ATOMIC_ACQUIRE);
            if (l < 3 && s != seq) {
                __atomic_store_n(&cap->reading, l, __ATOMIC_RELEASE);
                // still the latest once marked: the game can't be writing it
                if (__atomic_load_n(&cap->latest, __ATOMIC_ACQUIRE) == l) {
                    memcpy(vbuf, cap->buf[l], fb);
                    seq = s;
                }
                __atomic_store_n(&cap->reading, CSTV_NONE, __ATOMIC_RELEASE);
            }
            voff = 0;
            owed--;
            frames++;
        }
        struct pollfd pf[2];
        int np = 0;
        if (voff < fb) pf[np++] = (struct pollfd){cap_vfd, POLLOUT, 0};
        if (abeg < aend) pf[np++] = (struct pollfd){cap_afd, POLLOUT, 0};
        now = now_ns();
        next = base + k * 1000000000ull / CAP_FPS;
        uint64_t wait = next > now ? next - now : 0;
        struct timespec ts = {(time_t)(wait / 1000000000ull), (long)(wait % 1000000000ull)};
        if (ppoll(pf, np, &ts, NULL) < 0 && errno != EINTR) break;
        for (int i = 0; i < np; i++) {
            if (pf[i].revents & (POLLERR | POLLHUP)) goto out;   // ffmpeg is gone
            if (!(pf[i].revents & POLLOUT)) continue;
            int vid = pf[i].fd == cap_vfd;
            ssize_t r = vid ? write(cap_vfd, vbuf + voff, fb - voff) : write(cap_afd, abuf + abeg, aend - abeg);
            if (r < 0 && errno != EAGAIN && errno != EINTR) goto out;
            if (r > 0) *(vid ? &voff : &abeg) += (size_t)r;
        }
        if (abeg == aend) abeg = aend = 0;
    }
out:
    logf_("capture: %llu ticks, %llu frames written, %llu dropped (video behind)", (unsigned long long)n,
          (unsigned long long)frames, (unsigned long long)dropped);
    free(vbuf);
    free(abuf);
    free(tick);
    return NULL;
}

// The encoder that works here, best first: NVENC (NVIDIA, takes the BGRA frames as they are), VA-API (AMD /
// Intel), else libx264 on the CPU. Each is tried for real (an encoder can be listed and still not work).
static void *enc_probe(void *arg)
{
    (void)arg;
    FILE *p = popen("unset LD_PRELOAD LD_LIBRARY_PATH; "
                    "if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q h264_nvenc && "
                    "timeout 6 ffmpeg -nostdin -hide_banner -loglevel error -f lavfi -i color=c=black:s=1920x1080:r=60 "
                    "-frames:v 3 -pix_fmt bgra -c:v h264_nvenc -f null - </dev/null >/dev/null 2>&1; then echo nvenc; "
                    "elif [ -e /dev/dri/renderD128 ] && ffmpeg -hide_banner -encoders 2>/dev/null | grep -q h264_vaapi && "
                    "timeout 6 ffmpeg -nostdin -hide_banner -loglevel error -vaapi_device /dev/dri/renderD128 "
                    "-f lavfi -i color=c=black:s=1920x1080:r=60 -frames:v 3 -vf format=nv12,hwupload -c:v h264_vaapi "
                    "-f null - </dev/null >/dev/null 2>&1; then echo vaapi; fi", "r");
    char b[16] = "";
    if (p) {
        if (!fgets(b, sizeof b, p)) b[0] = 0;
        pclose(p);
    }
    int e = !strncmp(b, "nvenc", 5) ? ENC_NVENC : !strncmp(b, "vaapi", 5) ? ENC_VAAPI : ENC_X264;
    atomic_store(&hw_enc, e);
    logf_("streams encode with %s", e == ENC_NVENC ? "h264_nvenc" : e == ENC_VAAPI ? "h264_vaapi" : "libx264 (no GPU encoder works)");
    return NULL;
}

// own process group (a stream_cmd shell takes its children with it), gone with cstvd, no game preload.
// in: stdin (-1 = /dev/null); fd3: becomes fd 3 (-1 = none); err: stderr (-1 = ours)
static pid_t cap_spawn(const char *const *argv, int in, int fd3, int err)
{
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        prctl(PR_SET_PDEATHSIG, SIGTERM);   // cstvd dying (even by SIGKILL) ends the capture
        if (getppid() != parent) _exit(0);
        // the game's preload must not end up in a host program
        unsetenv("LD_PRELOAD");
        unsetenv("LD_LIBRARY_PATH");
        int dn = open("/dev/null", O_RDWR);
        dup2(in >= 0 ? in : dn, 0);
        dup2(dn, 1);
        if (err >= 0) dup2(err, 2);
        if (fd3 == 3) fcntl(3, F_SETFD, 0);   // dup2 onto itself would keep CLOEXEC
        else if (fd3 >= 0) dup2(fd3, 3);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (pid > 0) setpgid(pid, pid);
    return pid;
}

static void cap_read_err(void)
{
    static char line[512];
    static size_t len;
    char b[4096];
    for (;;) {
        ssize_t r = read(cap_efd, b, sizeof b);
        if (r <= 0) {
            if (r == 0 && len) { line[len] = 0; logf_("ffmpeg: %s", line); snprintf(cap_err, sizeof cap_err, "%.199s", line); len = 0; }
            return;
        }
        for (ssize_t i = 0; i < r; i++) {
            if (b[i] != '\n' && b[i] != '\r') {
                if (len < sizeof line - 1) line[len++] = b[i];
            } else if (len) {
                line[len] = 0;
                logf_("ffmpeg: %s", line);
                snprintf(cap_err, sizeof cap_err, "%.199s", line);
                len = 0;
            }
        }
    }
}

static int cap_start_encoder(void)
{
    int vp[2], ap[2], ep[2];
    if (pipe2(vp, O_CLOEXEC)) return -1;
    if (pipe2(ap, O_CLOEXEC)) { close(vp[0]); close(vp[1]); return -1; }
    if (pipe2(ep, O_CLOEXEC)) { close(vp[0]); close(vp[1]); close(ap[0]); close(ap[1]); return -1; }
    fcntl(vp[1], F_SETPIPE_SZ, 1 << 20);   // best effort: fewer wakeups per 3.6 MB frame
    cap_rate = cap->audio_rate >= 8000 && cap->audio_rate <= 192000 ? (int)cap->audio_rate : 48000;
    cap_ch = cap->audio_channels == 1 || cap->audio_channels == 2 ? (int)cap->audio_channels : 2;
    char rate[16], ch[4], size[16];
    snprintf(rate, sizeof rate, "%d", cap_rate);
    snprintf(ch, sizeof ch, "%d", cap_ch);
    snprintf(size, sizeof size, "%dx%d", CSTV_CAP_W, CSTV_CAP_H);
    int enc = atomic_load(&hw_enc);
    char fps[8];
    snprintf(fps, sizeof fps, "%d", CAP_FPS);
    const char *argv[80];
    int a = 0;
    static const char *const head[] = {"ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error"};
    for (size_t i = 0; i < sizeof head / sizeof *head; i++) argv[a++] = head[i];
    if (enc == ENC_VAAPI) { argv[a++] = "-vaapi_device"; argv[a++] = "/dev/dri/renderD128"; }
    const char *const in[] = {"-thread_queue_size", "8", "-f", "rawvideo", "-pix_fmt", "bgra", "-video_size", size,
                              "-framerate", fps, "-i", "pipe:0",
                              "-thread_queue_size", "512", "-f", "s16le", "-ar", rate, "-ac", ch, "-i", "pipe:3",
                              "-map", "0:v", "-map", "1:a"};
    for (size_t i = 0; i < sizeof in / sizeof *in; i++) argv[a++] = in[i];
    // No B-frames anywhere: the website plays streams over WebRTC, which has none. 8 Mbit/s for 1080p60.
    static const char *const nvenc[] = {"-c:v", "h264_nvenc", "-preset", "p4", "-tune", "ll", "-rc", "cbr", "-profile:v", "high",
                                        "-b:v", "8000k", "-maxrate", "8000k", "-bufsize", "8000k", NULL};
    static const char *const vaapi[] = {"-vf", "format=nv12,hwupload", "-c:v", "h264_vaapi", "-b:v", "8000k", "-maxrate", "8000k",
                                        "-bufsize", "8000k", NULL};
    static const char *const x264[] = {"-c:v", "libx264", "-preset", "veryfast", "-tune", "zerolatency", "-pix_fmt", "yuv420p",
                                       "-b:v", "6000k", "-maxrate", "6000k", "-bufsize", "6000k", NULL};
    for (const char *const *e = enc == ENC_NVENC ? nvenc : enc == ENC_VAAPI ? vaapi : x264; *e; e++) argv[a++] = *e;
    // Opus sound (in enhanced RTMP): WebRTC can't carry AAC. A keyframe every second: a website viewer that lost a
    // packet waits for the next one (the relay can't ask an RTMP publisher for one), and new viewers start sooner
    static const char *const out[] = {"-bf", "0", "-g", "60", "-c:a", "libopus", "-b:a", "128k", "-ar", "48000", "-f", "flv"};
    for (size_t i = 0; i < sizeof out / sizeof *out; i++) argv[a++] = out[i];
    argv[a++] = cap_url;
    argv[a] = NULL;
    pid_t pid = cap_spawn(argv, vp[0], ap[0], ep[1]);
    close(vp[0]);
    close(ap[0]);
    close(ep[1]);
    if (pid < 0) { close(vp[1]); close(ap[1]); close(ep[0]); return -1; }
    fcntl(vp[1], F_SETFL, O_NONBLOCK);
    fcntl(ap[1], F_SETFL, O_NONBLOCK);
    fcntl(ep[0], F_SETFL, O_NONBLOCK);
    cap_pid = pid;
    cap_vfd = vp[1];
    cap_afd = ap[1];
    cap_efd = ep[0];
    enc_at = now_ns();
    atomic_store(&cap_run, 1);
    if (pthread_create(&cap_thr, NULL, cap_writer, NULL)) atomic_store(&cap_run, 0);   // ffmpeg then ends on EOF at stop
    logf_("encoder started (%s, %dx%d %d fps, audio %d Hz x%d), pushing to %.*s...",
          enc == ENC_NVENC ? "h264_nvenc" : enc == ENC_VAAPI ? "h264_vaapi" : "libx264", CSTV_CAP_W, CSTV_CAP_H, CAP_FPS, cap_rate, cap_ch,
          (int)strcspn(cap_url, "?"), cap_url);
    return 0;
}
#endif

static int capturing(void)
{
#ifdef _WIN32
    return 0;
#else
    return cap_pid > 0 || cap;
#endif
}

static void stream_send(void)
{
    char b[300];
    b64encode(stream_txt, b, sizeof b);
    game_send("stream_state %s %s\n", stream_st, b[0] ? b : "-");
}

static void stream_state(const char *st, const char *text)
{
    snprintf(stream_st, sizeof stream_st, "%s", st);
    snprintf(stream_txt, sizeof stream_txt, "%s", text ? text : "");
    stream_send();
    logf_("stream: %s%s%s", st, stream_txt[0] ? ", " : "", stream_txt);
}

static void cap_stop(void)
{
#ifndef _WIN32
    if (cap) {
        __atomic_store_n(&cap->active, 0, __ATOMIC_RELEASE);
        game_send("capture 0\n");
    }
    if (atomic_load(&cap_run)) {
        atomic_store(&cap_run, 0);
        pthread_join(cap_thr, NULL);
    }
    // EOF on its inputs: ffmpeg finishes the flv
    if (cap_vfd >= 0) { close(cap_vfd); cap_vfd = -1; }
    if (cap_afd >= 0) { close(cap_afd); cap_afd = -1; }
    if (cap_pid > 0) {
        kill(-cap_pid, SIGTERM);
        int gone = 0;
        for (int i = 0; i < 50 && !gone; i++) {   // ffmpeg finishes the flv cleanly in well under 1 s
            gone = waitpid(cap_pid, NULL, WNOHANG) == cap_pid;
            if (!gone) usleep(20000);
        }
        kill(-cap_pid, SIGKILL);
        if (!gone) waitpid(cap_pid, NULL, 0);
        cap_pid = 0;
        logf_("capture stopped");
    }
    if (cap_efd >= 0) { close(cap_efd); cap_efd = -1; }
    if (cap) {
        os_shm_close(cap, sizeof *cap);
        unlink(CSTV_CAP_PATH);
        cap = NULL;
    }
#endif
    cap_live = 0;
}

static void cap_fail(const char *why)
{
    cap_stop();
    master_send("stream stop\n");
    stream_state("error", why);
}

static void stream_go(const char *url)
{
    if (strncmp(url, "rtmp://", 7) || !url_syntax_ok(url)) {
        stream_state("error", "the server sent a bad stream address");
        master_send("stream stop\n");
        return;
    }
    if (capturing()) return;
#ifdef _WIN32
    stream_state("error", "streaming isn't available on Windows yet");
    master_send("stream stop\n");
#else
    cap_at = now_ns();
    cap_live = 0;
    cap_err[0] = 0;
    if (pref_stream_cmd[0]) {
        char script[1400];
        size_t o = 0;
        for (const char *p = pref_stream_cmd; *p && o + 8 < sizeof script; ) {
            if (!strncmp(p, "{url}", 5)) { memcpy(script + o, "\"$1\"", 4); o += 4; p += 5; }
            else script[o++] = *p++;
        }
        script[o] = 0;
        const char *argv[] = {"/bin/sh", "-c", script, "sh", url, NULL};
        pid_t pid = cap_spawn(argv, -1, -1, -1);
        if (pid < 0) { cap_fail("couldn't start the capture"); return; }
        cap_pid = pid;
        enc_at = cap_at;
        logf_("capture started (stream_cmd), pushing to %.*s...", (int)strcspn(url, "?"), url);
        stream_state("starting", "starting the capture");
        return;
    }
    cap = os_shm_create(CSTV_CAP_PATH, sizeof *cap);
    if (!cap) {
        logf_("cannot create %s: %s", CSTV_CAP_PATH, strerror(errno));
        cap_fail("couldn't set up the capture");
        return;
    }
    memset(cap, 0, offsetof(struct cstv_cap_shm, buf));
    cap->magic = CSTV_CAP_MAGIC;
    cap->version = 2;
    cap->w = CSTV_CAP_W;
    cap->h = CSTV_CAP_H;
    cap->latest = CSTV_NONE;
    cap->reading = CSTV_NONE;
    __atomic_store_n(&cap->active, 1, __ATOMIC_RELEASE);
    snprintf(cap_url, sizeof cap_url, "%s", url);
    if (atomic_load(&hw_enc) < 0 && !enc_probing) {
        pthread_t t;
        enc_probing = 1;
        if (pthread_create(&t, NULL, enc_probe, NULL)) atomic_store(&hw_enc, ENC_X264);
        else pthread_detach(t);
    }
    game_send("capture 1\n");
    stream_state("starting", "waiting for the game");
#endif
}

static void stream_tick(void)
{
    uint64_t now = now_ns();
#ifndef _WIN32
    if (cap_efd >= 0) cap_read_err();
    uint64_t hb = cap ? __atomic_load_n(&cap->game_heartbeat_ns, __ATOMIC_ACQUIRE) : 0;
    if (cap && hb < cap_at && now - cap_at > 5000000000ull) {
        cap_fail("the game isn't sending frames (restart the game after updating CS:TV)");
        return;
    }
    // the encoder needs the game's audio format: wait up to 1 s for it (and for the encoder probe)
    if (cap && cap_pid <= 0 && atomic_load(&hw_enc) >= 0 && ((cap->audio_rate && cap->audio_channels) || now - cap_at > 1000000000ull)) {
        if (cap_start_encoder()) { cap_fail("couldn't start the encoder"); return; }
    }
    if (cap_pid > 0) {
        int st;
        if (waitpid(cap_pid, &st, WNOHANG) == cap_pid) {
            kill(-cap_pid, SIGKILL);
            cap_pid = 0;
            logf_("capture exited (%d)", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
            if (cap_efd >= 0) cap_read_err();
            char why[sizeof stream_txt];
            const char *e = cap_err;
            if (*e == '[' && strstr(e, "] ")) e = strstr(e, "] ") + 2;   // "[out#0/flv @ 0x...] "
            if (*e) snprintf(why, sizeof why, "the encoder stopped: %.170s", e);
            else snprintf(why, sizeof why, "%s", cap_live ? "the capture stopped" : "the capture didn't start (see the cstvd log)");
            cap_fail(why);
        } else if (!cap_live && now > enc_at + 2000000000ull && (!cap || hb >= cap_at)) {
            cap_live = 1;
            master_send("stream live\n");
            stream_state("live", NULL);
        }
    }
#endif
    if (stream_asked && now - stream_asked > 10000000000ull) {
        stream_asked = 0;
        master_send("stream stop\n");
        stream_state("error", "the CS:TV server didn't answer");
    }
}

static void stream_cmd(const char *what)
{
    if (!strcmp(what, "start")) {
        if (capturing() || stream_asked) return;
        if (!m_welcomed) { stream_state("error", "not connected to the CS:TV server"); return; }
        master_send("stream start\n");
        stream_asked = now_ns();
        stream_state("starting", "asking the server");
    } else if (!strcmp(what, "stop")) {
        int was = capturing() || stream_asked;
        cap_stop();
        stream_asked = 0;
        if (was) master_send("stream stop\n");
        stream_state("idle", NULL);
    }
}

// ------------------------------------------------------------------ load requests: link check first
// Names checked recently are remembered (most links are YouTube): only a new name waits for DNS, and
// that happens in a thread; the video loads when the answer is in.
struct load_req { char url[2048], host[256]; double pos; int paused, running, ok; long rev; uint64_t at; unsigned gen; };
static pthread_mutex_t chk_mu = PTHREAD_MUTEX_INITIALIZER;
static struct load_req *chk_done;
static unsigned chk_gen;
static struct { char host[256]; uint64_t until; } host_cache[16];

static void chk_cancel(void)
{
    pthread_mutex_lock(&chk_mu);
    chk_gen++;
    pthread_mutex_unlock(&chk_mu);
}

static int host_cached(const char *h)
{
    for (size_t i = 0; i < sizeof host_cache / sizeof *host_cache; i++)
        if (!strcmp(host_cache[i].host, h) && now_ns() < host_cache[i].until) return 1;
    return 0;
}

static void host_remember(const char *h)
{
    size_t old = 0;
    for (size_t i = 1; i < sizeof host_cache / sizeof *host_cache; i++)
        if (host_cache[i].until < host_cache[old].until) old = i;
    snprintf(host_cache[old].host, sizeof host_cache[old].host, "%s", h);
    host_cache[old].until = now_ns() + 600000000000ull;
}

static void load_now(const struct load_req *r)
{
    cur_rev = r->rev;
    load_running = r->running;   // 0 = the master waits for the video to start before counting
    double pos = r->pos + (r->running && !r->paused ? (now_ns() - r->at) / 1e9 * cur_speed : 0);
    start_playback(r->url, pos, r->paused);
    load_running = 1;
}

static void refuse_link(const char *url, const char *why)
{
    if (cur_url[0]) stop_playback("link refused");
    set_status(CSTV_ERROR, "rejected url %.200s: %s", url, why);
    set_screen(SCR_ERROR, "Couldn't play this", why);
}

static void *chk_thread(void *arg)
{
    struct load_req *r = arg;
    r->ok = host_global(r->host, 0) == 1;
    pthread_mutex_lock(&chk_mu);
    if (r->gen == chk_gen) {
        free(chk_done);
        chk_done = r;
        r = NULL;
    }
    pthread_mutex_unlock(&chk_mu);
    free(r);
    os_wake();
    return NULL;
}

static void request_load(const char *url, double pos, int paused, int running)
{
    chk_cancel();
    if (!url_syntax_ok(url)) { refuse_link(url, "links must be http(s)"); return; }
    struct load_req r;
    memset(&r, 0, sizeof r);
    snprintf(r.url, sizeof r.url, "%s", url);
    r.pos = pos;
    r.paused = paused;
    r.running = running;
    r.rev = master_rev;
    r.at = now_ns();
    if (!strncmp(url, "rtmp://", 7)) { load_now(&r); return; }
    url_host(url, r.host, sizeof r.host);
    int g = host_global(r.host, 1);
    if (g == 1 || (g == -1 && host_cached(r.host))) { load_now(&r); return; }
    if (g == 0) { refuse_link(url, "it points to a private address"); return; }
    struct load_req *h = malloc(sizeof *h);
    if (!h) return;
    *h = r;
    pthread_mutex_lock(&chk_mu);
    h->gen = chk_gen;
    pthread_mutex_unlock(&chk_mu);
    pthread_t t;
    if (pthread_create(&t, NULL, chk_thread, h) == 0) pthread_detach(t);
    else free(h);
}

static void chk_poll(void)
{
    pthread_mutex_lock(&chk_mu);
    struct load_req *r = chk_done;
    chk_done = NULL;
    pthread_mutex_unlock(&chk_mu);
    if (!r) return;
    if (r->ok) {
        host_remember(r->host);
        load_now(r);
    } else {
        refuse_link(r->url, "its address isn't public (or doesn't resolve)");
    }
    free(r);
}

// ------------------------------------------------------------------ commands from the game
static uint64_t last_ping;

static void handle_line(char *line)
{
    char cmd[16] = {0}, a[2100] = {0};
    long b = 0, c = 0, d = 1;
    int n = sscanf(line, "%15s %2099s %ld %ld %ld", cmd, a, &b, &c, &d);
    if (n < 1) return;
    last_ping = now_ns();
    if (!strcmp(cmd, "ping")) return;
    if (!strcmp(cmd, "m") && n >= 2) {   // m <line>: for the master
        const char *body = line + 2;
        if (strlen(body) > 3000 || !m_allowed(body)) { logf_("not passed to the master: %.80s", body); return; }
        if (!strncmp(body, "where ", 6) && strlen(body) < sizeof where_line) snprintf(where_line, sizeof where_line, "%s", body);
        char sub[16] = "", pw[100] = "-";
        // join <code> <pw>, create <name> <pw>, password <pw>
        if (sscanf(body, "lobby %15s %*s %99s", sub, pw) >= 1 && (!strcmp(sub, "join") || !strcmp(sub, "create")))
            snprintf(lobby_pass, sizeof lobby_pass, "%s", pw);
        else if (sscanf(body, "lobby password %99s", pw) == 1)
            snprintf(lobby_pass, sizeof lobby_pass, "%s", pw);
        if (m_state == M_UP) master_send("%s\n", body);
    } else if (!strcmp(cmd, "lchat") && n >= 2) {   // lchat <size|show|mode|scroll|click|search|draft ...>: the Lobby page's chat
        overlay_line("lchat_ui", line + 6);
    } else if (!strcmp(cmd, "xchat") && n >= 2) {   // xchat 0|1: the window's Live tab shows the stream's chat
        int v = atoi(a) != 0;
        if (v != chat_gui) {
            chat_gui = v;
            update_chat();
        }
    } else if (!strcmp(cmd, "ident") && n >= 2) {   // ident <steamid64> <name b64>
        char name[200] = "";
        sscanf(line, "%*s %*s %199s", name);
        const char *p = a;
        while (*p >= '0' && *p <= '9') p++;
        if (*p || strlen(a) >= sizeof ident_id) snprintf(a, sizeof a, "0");
        if (!id_ok(name) && strcmp(name, "-")) snprintf(name, sizeof name, "-");
        int changed = strcmp(a, ident_id) || strcmp(name, ident_name);
        snprintf(ident_id, sizeof ident_id, "%s", a);
        snprintf(ident_name, sizeof ident_name, "%s", name);
        if (changed && m_state != M_OFF) { m_close(NULL); m_next_try = 0; }   // say hello again as the new player
    } else if (!strcmp(cmd, "search") && n >= 2) {   // search <id> <query b64>
        char q[2100] = "";
        if (sscanf(line, "%*s %*s %2099s", q) == 1) start_search(a, q);
    } else if (!strcmp(cmd, "pref") && n >= 2) {   // pref <name> <value>
        char v[64] = "";
        if (sscanf(line, "%*s %*s %63s", v) == 1) set_pref(a, v);
    } else if (!strcmp(cmd, "pscreen") && n >= 2) {   // pscreen <server>/<map> [<geometry>|none]
        const char *p = line + 7;
        while (*p == ' ') p++;
        p += strlen(a);
        while (*p == ' ') p++;
        pscreen_cmd(a, p);
    } else if (!strcmp(cmd, "stream") && n >= 2) {   // stream start|stop
        stream_cmd(a);
    } else if (!strcmp(cmd, "load") && n >= 2) {
        char url[2048];
        if (b64decode(a, url, sizeof url) < 0) { set_status(CSTV_ERROR, "rejected url"); return; }
        request_load(url, n >= 3 ? b / 1000.0 : 0, n >= 4 ? (int)c : 0, n >= 5 ? d != 0 : 1);
    } else if (!strcmp(cmd, "pause") && n >= 2) {
        do_pause(atoi(a) != 0, n >= 3 ? b / 1000.0 : -1);
    } else if (!strcmp(cmd, "sync") && n >= 2) {
        do_sync(atol(a) / 1000.0, n >= 3 ? b != 0 : 1);   // sync <pos_ms> [running]
    } else if (!strcmp(cmd, "vol") && n >= 2) {
        double v = atof(a);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        mpv_set_property(mpv, "volume", MPV_FORMAT_DOUBLE, &v);
    } else if (!strcmp(cmd, "stop")) {
        chk_cancel();
        stop_playback("game asked");
        if (gave_up_at && now_ns() - gave_up_at < 10000000000ull) set_screen(SCR_ERROR, "Couldn't play this", gave_up_why);
        gave_up_at = 0;
    } else if (!strcmp(cmd, "speed") && n >= 2) {
        double v = atof(a);
        if (v >= 0.25 && v <= 4) {
            cur_speed = v;
            sync_at = 0;   // the clock was rebased; the sync that follows has the new base
            if (!cur_live && !catching) mpv_set_property(mpv, "speed", MPV_FORMAT_DOUBLE, &cur_speed);
        }
    } else if (!strcmp(cmd, "tol") && n >= 2) {
        apply_tol(atol(a));
    } else if (!strcmp(cmd, "seek") && n >= 2) {   // someone seeked: go there exactly
        double p = atol(a) / 1000.0;
        sync_at = 0;   // positions from before the seek mean nothing now
        if (p < 0) p = 0;
        if (cur_url[0] && !cur_live) {
            if (file_loaded) {
                controlled_seek(p, 1, 0);   // the server holds its clock until we report back
                next_sent = 0;   // seeking back from the end: it can finish again
                logf_("seek to %.1f s", p);
            } else {
                pending_seek = p;
                pending_running = 0;
            }
        }
    } else if (!strcmp(cmd, "title") && n >= 2) {   // title <uid> <url b64url>: look up a queued link's title
        char ub[2100] = {0};
        if (sscanf(line, "%*s %*s %2099s", ub) == 1) start_title_job(a, ub);
    } else if (!strcmp(cmd, "prefetch") && n >= 2) {   // prefetch <url b64url>: what plays next
        char url[2048];
        if (b64decode(a, url, sizeof url) >= 0 && url_syntax_ok(url)) prefetch(url);
    } else if (!strcmp(cmd, "simbreak")) {   // testing: act as if YouTube refused a chunk
        stream_broken = 1;
    } else if (!strcmp(cmd, "dbg") && n >= 2) {   // dbg <mpv property>: log its value (for debugging)
        char *v = mpv_get_property_string(mpv, a);
        logf_("dbg %s = %.300s", a, v ? v : "(unavailable)");
        mpv_free(v);
    } else if (!strcmp(cmd, "skip")) {
        if (cur_url[0]) { next_sent = 0; want_next = 1; }
    } else if (!strcmp(cmd, "info")) {
        if (cur_login[0]) overlay_send("{\"cmd\":\"info\",\"login\":\"%s\"}\n", cur_login);
    } else if (!strcmp(cmd, "opt") && n >= 2) {   // opt <name> <value>: per-player settings
        char name[16] = {0}, val[64] = {0};
        if (sscanf(line, "%*s %15s %63s", name, val) == 2) apply_opt(name, val);
    }
}

int main(void)
{
    os_net_init();
    setlocale(LC_NUMERIC, "C");   // libmpv requires the C numeric locale
    dxt1_fast_init();
#ifdef HAVE_ENC_AVX2
    __builtin_cpu_init();
    use_avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2");
#endif
    os_init_paths();
    prefs_load();
    pscreens_load();
    // hub: CSTV_HUB_URL, else hub= in the prefs, else wss://cstv.xyz/game
    const char *hu = getenv("CSTV_HUB_URL");
    if (!hu || !*hu) hu = pref_hub;
    if (*hu && (!strncmp(hu, "wss://", 6) || !strncmp(hu, "ws://", 5)) && !strpbrk(hu, " \t\"")) snprintf(hub_url, sizeof hub_url, "%s", hu);
#ifdef _WIN32
    os_log_open();
    // what we start finds the bundled yt-dlp + deno next to us, and keeps runtime files in %TEMP%\cstv
    {
        wchar_t *dir = os_wide(os_exe_dir), *run = os_wide(os_run_dir);
        static wchar_t old[32768], path[32768 + 1100];
        if (!GetEnvironmentVariableW(L"PATH", old, 32768)) old[0] = 0;
        if (dir) { _snwprintf(path, sizeof path / sizeof *path - 1, L"%ls;%ls", dir, old); SetEnvironmentVariableW(L"PATH", path); }
        if (run) SetEnvironmentVariableW(L"CSTV_DIR", run);
        free(dir);
        free(run);
    }
#endif
    if (open_shm()) return 1;
    if (os_wake_init()) return 1;
    if (init_mpv()) { logf_("mpv init failed"); return 1; }
    apply_opt("quality", pref_vals[1]);
    apply_opt("subs", pref_vals[2]);
    apply_opt("chat", pref_vals[3]);

    sock_t ls = os_ctl_listen(logf_);
    if (ls == SOCK_BAD) return 1;
    set_status(CSTV_IDLE, "ready (streamlink %s)", have_streamlink() ? "found" : "missing: Twitch channels show ads");

    sock_t cl = SOCK_BAD;
    char buf[8192];
    size_t blen = 0;
    // Started by cstv-launch along with the game: quit when there's no game to serve (the launcher stops
    // us when the game exits; this covers the launcher being killed). Grace: 180 s for the game to start
    // and connect, 15 s after it disconnects (it reconnects within 2 s when we restart).
    int with_game = getenv("CSTV_QUIT_WITHOUT_GAME") != NULL;
    uint64_t alone_since = now_ns(), alone_limit = 180000000000ull;
    for (;;) {
        struct pollfd pf[3] = {{ls, POLLIN, 0}, {os_wake_rd, POLLIN, 0}, {cl, POLLIN, 0}};
        int npf = cl != SOCK_BAD ? 3 : 2;
        poll(pf, npf, 200);
        shm->heartbeat_ns = now_ns();
        if (with_game && cl == SOCK_BAD && now_ns() - alone_since > alone_limit) {
            logf_("no game connected for %llu s, quitting", (unsigned long long)(alone_limit / 1000000000ull));
            if (cur_url[0]) stop_playback("quitting");
            kill_streamlink();
            cap_stop();
            _exit(0);
        }
        if (pf[1].revents) {
            os_wake_drain();
            handle_mpv_events();
        }
        if (pf[0].revents & POLLIN) {
            sock_t c = os_accept(ls);
            if (c != SOCK_BAD) {
                if (cl != SOCK_BAD) sock_close(cl);   // newest game connection wins
                cl = c;
                game_fd = c;
                game_send("chat %d\n", chat_state);
                if (chat_avail >= 0) game_send("chatavail %d\n", chat_avail);
                game_send("master %d\n", m_welcomed);
                prefs_send();
                stream_send();
                blen = 0;
                last_ping = now_ns();
                logf_("game connected");
            }
        }
        if (cl != SOCK_BAD && (pf[2].revents & (POLLIN | POLLHUP | POLLERR))) {
            int r = (int)recv(cl, buf + blen, (int)(sizeof buf - 1 - blen), 0);
            if (r <= 0) {
                sock_close(cl);
                cl = SOCK_BAD;
                game_fd = SOCK_BAD;
                if (capturing() || stream_asked) stream_cmd("stop");
                ident_id[0] = where_line[0] = 0;   // a new connection says who and where it is again
                chk_cancel();
                if (cur_url[0]) stop_playback("game disconnected");
                logf_("game disconnected");
                chat_gui = 0;   // its window is gone
                alone_since = now_ns();
                alone_limit = 15000000000ull;
            } else {
                blen += r;
                buf[blen] = 0;
                char *s = buf, *nl;
                while ((nl = strchr(s, '\n'))) {
                    *nl = 0;
                    handle_line(s);
                    s = nl + 1;
                }
                blen = strlen(s);
                memmove(buf, s, blen + 1);
                if (blen > sizeof buf / 2) blen = 0;   // garbage, drop it
            }
        }
        if (cl != SOCK_BAD && cur_url[0] && now_ns() - last_ping > 3000000000ull) stop_playback("no ping from the game for 3 s");
        m_tick();
        stream_tick();
        overlay_poll();
        if (live_retry_at && now_ns() >= live_retry_at && !strncmp(cur_url, "rtmp://", 7)) {
            live_retry_at = 0;
            char keep[sizeof cur_url];
            snprintf(keep, sizeof keep, "%s", cur_url);
            load_url(keep, 0, 0);
        }
        chk_poll();
        pl_check_wait();
        static int started_gen = -1;
        if (have_frame && frame_gen == load_gen && started_gen != load_gen && cur_url[0] && !seek_wait) {
            started_gen = load_gen;
            logf_("first frame after %.1f s", (now_ns() - loaded_at) / 1e9);
            master_send("started %ld\n", cur_rev);   // the master starts its clock now (not when the link was typed)
            update_chat();               // video first, then chat
        }
        // playlist: make sure the list covers the current video, then tell the server what's next
        if (have_frame && cur_list[0] && cur_vid[0] && strcmp(win_sent_vid, cur_vid)) {
            pthread_mutex_lock(&pl_mu);
            int busy = pl_busy;
            pthread_mutex_unlock(&pl_mu);
            if (!busy) {
                int found = pl_find(cur_vid);
                if (found >= 0) snprintf(win_sent_vid, sizeof win_sent_vid, "%s", cur_vid);
                else if (pl_lookups < 2) playlist_lookup(cur_url, cur_list, list_start(cur_list, cur_index));
                else {   // give up for this video (e.g. a private playlist: anonymously we only get this one video)
                    snprintf(win_sent_vid, sizeof win_sent_vid, "%s", cur_vid);
                    logf_("playlist %s: can't see what comes after this video (private playlist?)", cur_list);
                }
            }
        }
        seek_tick();
        catchup_tick();
        drift_tick();
        // waiting on data for 6 s straight (YouTube throttled or dropped the stream): treat it as broken
        if (stall_start && file_loaded && !cur_live && !seek_wait && now_ns() - stall_start > 6000000000ull) {
            logf_("no data for 6 s");
            stall_start = 0;
            stream_broken = 1;
        }
        if (stream_broken && file_loaded && cur_url[0] && !cur_live && !streamlink_running()) {
            stream_broken = 0;
            if (strcmp(refresh_url, cur_url)) { refresh_count = 0; snprintf(refresh_url, sizeof refresh_url, "%s", cur_url); }
            if (now_ns() - last_refresh > 3000000000ull) {
                if (refresh_count < 3) {
                    refresh_count++;
                    last_refresh = now_ns();
                    double target;
                    int running;
                    if (sync_at && !cur_paused) { target = sync_pos + (now_ns() - sync_at) / 1e9 * cur_speed; running = 1; }
                    else if (seek_wait) {   // the seek that never arrived (plus the time it waited, if the clock ran)
                        running = seek_running;
                        target = seek_target + (running ? (now_ns() - seek_wait_at) / 1e9 * cur_speed : 0);
                    }
                    else { target = shm->pos_s; running = !cur_paused; }
                    logf_("YouTube refused part of the video: reloading at %.1f s (%d/3)", target, refresh_count);
                    char keep[sizeof cur_url];
                    snprintf(keep, sizeof keep, "%s", cur_url);
                    load_note = "reconnecting";
                    load_running = running;
                    int paused = cur_paused;
                    start_playback(keep, target, paused);
                    load_note = NULL;
                    load_running = 1;
                } else {
                    logf_("the video keeps failing: moving on");
                    set_status(CSTV_ERROR, "could not play: the site kept refusing the video");
                    set_screen(SCR_ERROR, "Couldn't play this", "the site kept refusing the video");
                    give_up("the site kept refusing the video");
                    want_next = 1;
                }
            }
        }
        // a quality switch whose new stream hasn't delivered in 4 s: try another stream of that height
        if (resync_pending && now_ns() - resync_set_at > 4000000000ull && shm->pos_s - switch_pos0 < 0.3 && switch_tries < 3) {
            int64_t cur = -1;
            mpv_get_property(mpv, "vid", MPV_FORMAT_INT64, &cur);
            switch_tried[switch_tries++] = cur;
            int64_t alt = pick_video_track_x(switch_h, switch_tried, switch_tries);
            if (alt >= 0 && alt != cur) {
                mpv_set_property(mpv, "vid", MPV_FORMAT_INT64, &alt);
                logf_("quality: track %lld didn't start, trying %lld", (long long)cur, (long long)alt);
                resync_set_at = now_ns();
                stall_start = 0;   // the watchdog would reload everything; give the other stream its chance
            }
        }
        if (resync_pending) {   // after a quality switch: once the video moves normally again for 0.5 s
            static double last_pos;
            static uint64_t last_t;
            double at = shm->pos_s;
            uint64_t now = now_ns();
            if (now - resync_set_at < 300000000ull || !last_t || now - last_t > 1000000000ull) { last_pos = at; last_t = now; }
            else if (now - last_t >= 500000000ull) {
                if (at - last_pos >= 0.4 * cur_speed || now - resync_set_at > 10000000000ull) { resync_from_last(); last_t = 0; }
                else { last_pos = at; last_t = now; }
            }
        }
        if (want_next) send_next();
        if (streamlink_running() && !os_proc_running(&streamlink_proc)) {
            if (cur_url[0] && !cur_paused) {
                set_status(CSTV_ERROR, "streamlink exited (channel offline?)");
                if (!have_frame) set_screen(SCR_ERROR, "Couldn't play this", "the channel looks offline");
                else set_screen(SCR_ERROR, "Stream ended", "the channel went offline");
            }
        }
    }
}
