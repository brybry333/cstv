// In-game capture for "Start stream": the game's own picture and sound into /tmp/cstv/capture
// (struct cstv_cap_shm), which cstvd encodes. No desktop capture, so nothing else on the screen or the
// speakers gets in, and the CS:TV window is left out of the picture.
//
// Video, renderer-agnostic (togl and DXVK both run shaderapidx9's code, which only uses D3D9 calls):
//   frame N, first MatSystemTopPanel paint (the HUD is drawn, the CS:TV window and the console/menu
//   popups aren't yet): CopyRenderTargetToTextureEx the back buffer into our 1920x1080 render target
//   (StretchRect, D3DTEXF_LINEAR; letter-/pillarboxed, black bars, for screens that aren't 16:9).
//   frame N+1, FRAME_START: ReadPixels that render target into a free shm buffer, publish it.
// Both run on the render thread (queued through the material system's call queue, like the uploads in
// cstv_client.cpp; directly with mat_queue_mode 0). ReadPixels is GetRenderTargetData + LockRect, which
// waits for the GPU to finish everything submitted before it. Issued at the start of the next frame's
// queue, that is the previous frame, which the GPU normally has finished by then; a deeper ring of render
// targets wouldn't help, the wait is for everything queued, not for the one copy. (The queued context's own
// ReadPixels would stall the main thread on the render thread, hence the job.) At most 60 copies a second.
//
// Audio: engine.so opens its output with SDL_OpenAudioDevice(NULL, 0, {44100 Hz, AUDIO_S16LSB, 2 ch,
// 1024 samples, callback}, &obtained, 0). It's an ordinary dynamic import (engine.so is dlopen'ed with
// RTLD_NOW, no RTLD_DEEPBIND), so the definition below, exported through cstv.ver from the preload, is
// what the engine calls. It wraps the callback: the original mixes, then while capturing the mix goes
// into the shm ring as interleaved s16.
//
// Slots used (CS:S 64-bit Linux binaries, checked 2026-10-18; numbers from the SDK headers via
// tools/vtidx.cpp-style PMF arithmetic, then the code in each slot read with tools/slots.py + objdump):
//   IMaterialSystem (CMaterialSystem)  31 GetBackBufferDimensions (-> shader device),
//     85 CreateNamedRenderTargetTextureEx (texture manager create + IncrementReferenceCount; outside a
//     Begin/EndRenderTargetAllocation block it calls EndRenderTargetAllocation itself, which only flushes
//     unless mat_requires_rt_alloc_first, default 0; Ex2 is the one that refuses late allocation),
//     98 GetRenderContext (thread-local context, the hardware one on the render thread)
//   IMatRenderContext (CMatRenderContext)  12 ClearBuffers, 13 ReadPixels (-> IShaderAPI 158),
//     73 ClearColor4ub, 106 PushRenderTargetAndViewport(ITexture *), 109 PopRenderTargetAndViewport,
//     111 CopyRenderTargetToTextureEx (-> ITextureInternal 42 -> IShaderAPI 197), 147 GetCallQueue
//   IShaderAPI 197 CopyRenderTargetToTextureEx: GetRenderTarget, GetSurfaceLevel(0), StretchRect(LINEAR);
//   158 ReadPixels: own render target of the same size -> CreateOffscreenPlainSurface(SYSTEMMEM) +
//   GetRenderTargetData, LockRect(READONLY), ConvertImageFormat (a row memcpy when the formats match),
//   top row first. togl flips y in its vertex shaders ("vTempPos.y = -vTempPos.y") so render targets
//   hold the top row first there too.
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dlfcn.h>

#include "materialsystem/imaterialsystem.h"
#include "materialsystem/itexture.h"
#include "tier1/callqueue.h"
#include "tier1/functors.h"

#include "cstv_proto.h"
#include "cstv_capture.h"
#include "cstv_platform.h"

#define CAP_VERSION 2   // what cstvd puts in cstv_cap_shm.version (2: 1920x1080, 60 a second)

static void say(const char *fmt, ...)
{
    typedef void (*ConMsg_t)(const char *fmt, ...);
    static ConMsg_t con = (ConMsg_t)plat_conmsg();
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (con) con("[cstv] %s\n", buf);
}

static uint64_t now_ns()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static IMaterialSystem *g_ms;
static ITexture *g_rt;
static cstv_cap_shm *g_map;                    // our mapping, until nothing can be using it any more
static std::atomic<cstv_cap_shm *> g_live;     // where the jobs and the audio callback write; null = not capturing
static std::atomic<int> g_jobs, g_audio_busy;  // queued jobs not run yet / audio callbacks inside the mapping
static bool g_start_pending, g_armed, g_copied;   // armed: first top-panel paint of this frame still to come
static double g_next;                          // earliest time of the next copy
static uint64_t g_frame_ns;                    // render thread: when the copy that's in g_rt was made

// render thread totals, printed when the capture stops
static std::atomic<uint64_t> g_reads, g_read_ns, g_read_max_ns, g_main_ns;

// ---------------------------------------------------------------- video (render thread)
static void copy_frame(const Rect_t &dst)
{
    if (!g_live.load()) return;
    CMatRenderContextPtr ctx(g_ms);
    if (dst.width != CSTV_CAP_W || dst.height != CSTV_CAP_H) {
        ctx->PushRenderTargetAndViewport(g_rt);
        ctx->ClearColor4ub(0, 0, 0, 255);
        ctx->ClearBuffers(true, false, false);
        ctx->PopRenderTargetAndViewport();
    }
    Rect_t d = dst;
    ctx->CopyRenderTargetToTextureEx(g_rt, 0, nullptr, &d);   // null source: the whole back buffer
    g_frame_ns = now_ns();
}

static void read_frame()
{
    cstv_cap_shm *s = g_live.load();
    if (!s) return;
    uint64_t t0 = now_ns();
    uint32_t latest = __atomic_load_n(&s->latest, __ATOMIC_ACQUIRE), reading = __atomic_load_n(&s->reading, __ATOMIC_ACQUIRE);
    uint32_t i = 0;
    while (i == latest || i == reading) i++;
    CMatRenderContextPtr ctx(g_ms);
    ctx->PushRenderTargetAndViewport(g_rt);
    ctx->ReadPixels(0, 0, CSTV_CAP_W, CSTV_CAP_H, s->buf[i], IMAGE_FORMAT_BGRA8888);
    ctx->PopRenderTargetAndViewport();
    s->frame_ns = g_frame_ns;
    __atomic_store_n(&s->latest, i, __ATOMIC_RELEASE);
    __atomic_add_fetch(&s->seq, 1, __ATOMIC_RELEASE);
    uint64_t t1 = now_ns();
    s->game_heartbeat_ns = t1;
    g_reads++;
    g_read_ns += t1 - t0;
    if (t1 - t0 > g_read_max_ns) g_read_max_ns = t1 - t0;
}

class CapJob : public CFunctor {
    std::atomic<int> m_refs{1};
    bool m_read;
    Rect_t m_dst;
public:
    CapJob(bool read, const Rect_t &dst) : m_read(read), m_dst(dst) {}
    int AddRef() override { return ++m_refs; }
    int Release() override { int r = --m_refs; if (!r) delete this; return r; }
    void operator()() override
    {
        if (m_read) read_frame();
        else copy_frame(m_dst);
        g_jobs--;
    }
};

static void run(bool read, const Rect_t &dst)
{
    uint64_t t0 = now_ns();
    g_jobs++;
    CapJob *job = new CapJob(read, dst);
    {
        CMatRenderContextPtr ctx(g_ms);
        if (ICallQueue *q = ctx->GetCallQueue()) q->QueueFunctor(job);   // takes its own reference
        else (*job)();
    }
    job->Release();
    g_main_ns += now_ns() - t0;
}

// ---------------------------------------------------------------- control (main thread)
static bool create_rt()
{
    // RT_SIZE_LITERAL: the default size modes would shrink it to fit a smaller back buffer
    ITexture *t = g_ms->CreateNamedRenderTargetTextureEx("_rt_cstv_capture", CSTV_CAP_W, CSTV_CAP_H, RT_SIZE_LITERAL,
        IMAGE_FORMAT_BGRA8888, MATERIAL_RT_DEPTH_NONE, TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD | TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT, 0);
    if (!t || t->GetActualWidth() != CSTV_CAP_W || t->GetActualHeight() != CSTV_CAP_H) {
        say("could not create the capture render target; streaming from the game won't work");
        return false;
    }
    g_rt = t;   // created with a reference of ours: it stays for the rest of the game
    return true;
}

// Unmap once no queued job and no audio callback can still be using the mapping.
static void release_map()
{
    if (g_map && !g_live.load() && !g_jobs.load() && !g_audio_busy.load()) {
        plat_unmap(g_map, sizeof *g_map);
        g_map = nullptr;
    }
}

static void try_start()
{
    if (g_map) return;   // the last capture's mapping is still in use
    g_start_pending = false;
    if (!g_ms) g_ms = (IMaterialSystem *)plat_interface("materialsystem", MATERIAL_SYSTEM_INTERFACE_VERSION);
    if (!g_ms || (!g_rt && !create_rt())) return;
    cstv_cap_shm *s = (cstv_cap_shm *)plat_map(CSTV_CAP_PATH, sizeof(cstv_cap_shm));
    if (!s) { say("cannot open %s", CSTV_CAP_PATH); return; }
    if (s->magic != CSTV_CAP_MAGIC || s->version != CAP_VERSION || s->w != CSTV_CAP_W || s->h != CSTV_CAP_H) {
        say("cstvd's capture buffer has an unexpected format; rebuild cstvd");
        plat_unmap(s, sizeof *s);
        return;
    }
    g_map = s;
    g_next = 0;
    g_copied = g_armed = false;
    g_reads = g_read_ns = g_read_max_ns = g_main_ns = 0;
    g_live.store(s);
    say("capturing the game for the stream");
}

void capture_start()
{
    capture_stop();
    g_start_pending = true;   // done at the next FRAME_START, outside any drawing
}

void capture_stop()
{
    g_start_pending = false;
    if (!g_live.load()) return;
    g_live.store(nullptr);
    g_copied = g_armed = false;
    uint64_t n = g_reads.load();
    if (n)
        say("capture stopped: %llu frames; readback avg %.2f ms, max %.2f ms (render thread); main thread %.1f us/frame",
            (unsigned long long)n, g_read_ns.load() / 1e6 / n, g_read_max_ns.load() / 1e6, g_main_ns.load() / 1e3 / n);
    release_map();
}

void capture_frame()
{
    if (g_map) release_map();
    if (g_start_pending) try_start();
    cstv_cap_shm *s = g_live.load(std::memory_order_relaxed);
    if (!s) return;
    if (!__atomic_load_n(&s->active, __ATOMIC_ACQUIRE)) { capture_stop(); return; }
    if (g_copied) {
        g_copied = false;
        run(true, Rect_t());
    }
    g_armed = true;
}

// The engine paints the top panel twice a frame in game (HUD, then the UI popups) and the CS:TV window is
// drawn after each, so only the first paint after FRAME_START is taken.
void capture_paint()
{
    if (!g_armed) return;
    g_armed = false;
    double t = plat_now();
    if (t < g_next || g_copied) return;
    g_next = std::max(g_next + 1.0 / 60, t + 0.012);   // 60 a second on average, never closer than 12 ms
    int bw = 0, bh = 0;
    g_ms->GetBackBufferDimensions(bw, bh);
    if (bw <= 0 || bh <= 0) return;
    Rect_t d = {0, 0, CSTV_CAP_W, CSTV_CAP_H};
    if ((int64_t)bw * CSTV_CAP_H > (int64_t)bh * CSTV_CAP_W) d.height = (int)((int64_t)CSTV_CAP_W * bh / bw) & ~1;
    else d.width = (int)((int64_t)CSTV_CAP_H * bw / bh) & ~1;
    d.x = (CSTV_CAP_W - d.width) / 2;
    d.y = (CSTV_CAP_H - d.height) / 2;
    run(false, d);
    g_copied = true;
}

// ---------------------------------------------------------------- audio (SDL's audio thread)
struct AudioSpec {   // SDL_AudioSpec, SDL2 ABI
    int freq;
    uint16_t format;
    uint8_t channels, silence;
    uint16_t samples, padding;
    uint32_t size;
    void (*callback)(void *userdata, uint8_t *stream, int len);
    void *userdata;
};
enum { AUDIO_S16_LE = 0x8010, AUDIO_F32_LE = 0x8120 };

struct Tap {
    void (*cb)(void *, uint8_t *, int);
    void *ud;
    int freq, channels;
    uint16_t format;
};
static Tap g_taps[16];        // one per device the engine opened (snd_restart opens another)
static int g_ntaps;
static std::atomic<Tap *> g_cur;   // only the newest device writes, so the ring has one writer

static void write_audio(cstv_cap_shm *s, const Tap *t, const uint8_t *in, int len)
{
    int ch = t->channels, f32 = t->format == AUDIO_F32_LE;
    if (ch < 1 || ch > 2 || (!f32 && t->format != AUDIO_S16_LE)) return;
    if (s->audio_rate != (uint32_t)t->freq || s->audio_channels != (uint32_t)ch) {
        s->audio_rate = t->freq;
        s->audio_channels = ch;
    }
    uint64_t w = s->audio_written;
    int n = len / ((f32 ? 4 : 2) * ch);
    for (int i = 0; i < n; i++) {
        int16_t *d = s->audio + ((w + i) & (CSTV_CAP_AUDIO_FRAMES - 1)) * ch;
        for (int c = 0; c < ch; c++) {
            if (f32) {
                float v;
                memcpy(&v, in + ((size_t)i * ch + c) * 4, 4);
                v = std::max(-1.f, std::min(1.f, v));
                d[c] = (int16_t)(v * 32767.f);
            } else {
                memcpy(&d[c], in + ((size_t)i * ch + c) * 2, 2);
            }
        }
    }
    __atomic_store_n(&s->audio_written, w + n, __ATOMIC_RELEASE);
    s->game_heartbeat_ns = now_ns();
}

static void tap_cb(void *ud, uint8_t *stream, int len)
{
    Tap *t = (Tap *)ud;
    t->cb(t->ud, stream, len);
    if (!g_live.load(std::memory_order_relaxed) || t != g_cur.load(std::memory_order_relaxed)) return;
    g_audio_busy++;   // before loading the pointer: capture_stop clears it, then waits for this to drop
    cstv_cap_shm *s = g_live.load();
    if (s && __atomic_load_n(&s->active, __ATOMIC_ACQUIRE)) write_audio(s, t, stream, len);
    g_audio_busy--;
}

typedef uint32_t (*OpenAudioDevice_t)(const char *, int, const AudioSpec *, AudioSpec *, int);

static OpenAudioDevice_t real_open()
{
    void *f = dlsym(RTLD_NEXT, "SDL_OpenAudioDevice");
    if (!f) {   // SDL loaded only in the local scope of a dlopen'ed module
        void *h = dlopen("libSDL2-2.0.so.0", RTLD_NOLOAD | RTLD_LAZY);
        if (h) f = dlsym(h, "SDL_OpenAudioDevice");
    }
    return (OpenAudioDevice_t)f;
}

extern "C" __attribute__((visibility("default")))
uint32_t SDL_OpenAudioDevice(const char *device, int iscapture, const AudioSpec *desired, AudioSpec *obtained, int allowed_changes)
{
    static OpenAudioDevice_t real = real_open();
    static bool tap = plat_in_game() && !plat_disabled();
    if (!real) return 0;
    if (!tap || iscapture || !desired || !desired->callback || g_ntaps == (int)(sizeof g_taps / sizeof *g_taps))
        return real(device, iscapture, desired, obtained, allowed_changes);
    Tap *t = &g_taps[g_ntaps];
    t->cb = desired->callback;
    t->ud = desired->userdata;
    AudioSpec want = *desired;
    want.callback = tap_cb;
    want.userdata = t;
    uint32_t dev = real(device, iscapture, &want, obtained, allowed_changes);
    if (!dev) return 0;
    // the callback gets the obtained format (without an obtained spec SDL converts from the desired one);
    // the device starts paused, so nothing calls tap_cb before this is filled in
    const AudioSpec *fmt = obtained ? obtained : desired;
    t->freq = fmt->freq;
    t->channels = fmt->channels;
    t->format = fmt->format;
    if (obtained) {
        obtained->callback = desired->callback;
        obtained->userdata = desired->userdata;
    }
    g_ntaps++;
    g_cur.store(t);
    return dev;
}
