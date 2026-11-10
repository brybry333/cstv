// Fast DXT1 (BC1) block encoder for video frames, several times faster than stb_dxt at close quality:
//  - nearly flat blocks: the block's average colour, made from two 565 colours and their 2/3 mix
//    (stb_dxt's "optimal single colour" trick); plain 565 rounding would show bands in dark gradients
//  - other blocks: the bounding box of the colours, the box diagonal that matches how they lean
//    (I. Castano), ends pulled in by 1/16 (J.M.P. van Waveren, "Real-Time DXT Compression"), nearest of
//    the 4 palette colours, then one least-squares pass on the two end colours (as stb_dxt's refine).
// Measured on 1080p photos: 35-47 dB like stb_dxt (gradients slightly better), 5-19 ms vs 24-38 ms per frame.
// Call dxt1_fast_init() once before use.
#pragma once
#include <stddef.h>
#include <stdint.h>

// always inlined: cstvd compiles its encoder loop for AVX2 and for any x86-64 CPU (picked at runtime),
// and each copy must get its own build of these, not a call into the plain one
#if defined(__GNUC__)
#define DXT1F_INLINE static inline __attribute__((always_inline))
#else
#define DXT1F_INLINE static inline
#endif

static uint8_t dxt1f_om5[256][2], dxt1f_om6[256][2];   // value -> (c0 part, c1 part) whose 2/3 mix is closest

DXT1F_INLINE int dxt1f_e5(int v) { return v << 3 | v >> 2; }
DXT1F_INLINE int dxt1f_e6(int v) { return v << 2 | v >> 4; }

static void dxt1_fast_init(void)
{
    for (int v = 0; v < 256; v++) {
        int best5 = 1 << 30, best6 = 1 << 30;
        for (int a = 0; a < 64; a++)
            for (int b = 0; b < 64; b++) {
                if (a < 32 && b < 32) {
                    int m = (2 * dxt1f_e5(a) + dxt1f_e5(b)) / 3, err = (m - v) * (m - v) * 64 + (a > b ? a - b : b - a);
                    if (err < best5) { best5 = err; dxt1f_om5[v][0] = (uint8_t)a; dxt1f_om5[v][1] = (uint8_t)b; }
                }
                int m = (2 * dxt1f_e6(a) + dxt1f_e6(b)) / 3, err = (m - v) * (m - v) * 64 + (a > b ? a - b : b - a);
                if (err < best6) { best6 = err; dxt1f_om6[v][0] = (uint8_t)a; dxt1f_om6[v][1] = (uint8_t)b; }
            }
    }
}

DXT1F_INLINE int dxt1f_clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
DXT1F_INLINE unsigned dxt1f_565(int r, int g, int b)
{
    return (unsigned)(((dxt1f_clamp(r) * 31 + 127) / 255) << 11 | ((dxt1f_clamp(g) * 63 + 127) / 255) << 5 | ((dxt1f_clamp(b) * 31 + 127) / 255));
}

// nearest of the 4 palette colours for every pixel; c0 > c1 (4-colour mode)
DXT1F_INLINE uint32_t dxt1f_indices(int px[16][3], unsigned c0, unsigned c1)
{
    int pal[4][3];
    pal[0][0] = dxt1f_e5(c0 >> 11); pal[0][1] = dxt1f_e6(c0 >> 5 & 63); pal[0][2] = dxt1f_e5(c0 & 31);
    pal[1][0] = dxt1f_e5(c1 >> 11); pal[1][1] = dxt1f_e6(c1 >> 5 & 63); pal[1][2] = dxt1f_e5(c1 & 31);
    for (int c = 0; c < 3; c++) {
        pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3;
        pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3;
    }
    uint32_t idx = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < 4; k++) {
            int dr = px[i][0] - pal[k][0], dg = px[i][1] - pal[k][1], db = px[i][2] - pal[k][2];
            int d = dr * dr + dg * dg + db * db;
            if (d < bd) { bd = d; best = k; }
        }
        idx |= (uint32_t)best << (2 * i);
    }
    return idx;
}

DXT1F_INLINE void dxt1f_put(uint8_t *dst, unsigned c0, unsigned c1, uint32_t idx)
{
    dst[0] = (uint8_t)c0; dst[1] = (uint8_t)(c0 >> 8);
    dst[2] = (uint8_t)c1; dst[3] = (uint8_t)(c1 >> 8);
    dst[4] = (uint8_t)idx; dst[5] = (uint8_t)(idx >> 8); dst[6] = (uint8_t)(idx >> 16); dst[7] = (uint8_t)(idx >> 24);
}

// src: 4x4 pixels, 4 bytes each (R, G, B, x), rows `stride` bytes apart; dst: 8 bytes
DXT1F_INLINE void dxt1_fast_block(uint8_t *dst, const uint8_t *src, size_t stride)
{
    int px[16][3];
    int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0}, sum[3] = {0, 0, 0};
    for (int y = 0; y < 4; y++) {
        const uint8_t *row = src + y * stride;
        for (int x = 0; x < 4; x++)
            for (int c = 0; c < 3; c++) {
                int v = row[x * 4 + c];
                px[y * 4 + x][c] = v;
                sum[c] += v;
                if (v < mn[c]) mn[c] = v;
                if (v > mx[c]) mx[c] = v;
            }
    }
    int range = mx[0] - mn[0];
    if (mx[1] - mn[1] > range) range = mx[1] - mn[1];
    if (mx[2] - mn[2] > range) range = mx[2] - mn[2];
    if (range <= 6) {   // nearly flat: one colour, as exact as DXT1 can make it
        int r = (sum[0] + 8) >> 4, g = (sum[1] + 8) >> 4, b = (sum[2] + 8) >> 4;
        unsigned c0 = (unsigned)(dxt1f_om5[r][0] << 11 | dxt1f_om6[g][0] << 5 | dxt1f_om5[b][0]);
        unsigned c1 = (unsigned)(dxt1f_om5[r][1] << 11 | dxt1f_om6[g][1] << 5 | dxt1f_om5[b][1]);
        unsigned i = 0xaaaaaaaau;                         // every pixel = index 2 (2/3 c0 + 1/3 c1)
        if (c0 < c1) { unsigned t = c0; c0 = c1; c1 = t; i = 0xffffffffu; }   // then it's index 3
        else if (c0 == c1) i = 0;
        dxt1f_put(dst, c0, c1, i);
        return;
    }
    int cov_rb = 0, cov_gb = 0;
    for (int i = 0; i < 16; i++) {
        int r = 2 * px[i][0] - mn[0] - mx[0], g = 2 * px[i][1] - mn[1] - mx[1], b = 2 * px[i][2] - mn[2] - mx[2];
        cov_rb += r * b;
        cov_gb += g * b;
    }
    int hi[3] = {mx[0], mx[1], mx[2]}, lo[3] = {mn[0], mn[1], mn[2]};
    if (cov_rb < 0) { int t = hi[0]; hi[0] = lo[0]; lo[0] = t; }
    if (cov_gb < 0) { int t = hi[1]; hi[1] = lo[1]; lo[1] = t; }
    for (int c = 0; c < 3; c++) {
        int d = (hi[c] - lo[c]) / 16;
        hi[c] -= d;
        lo[c] += d;
    }
    unsigned c0 = dxt1f_565(hi[0], hi[1], hi[2]), c1 = dxt1f_565(lo[0], lo[1], lo[2]);
    if (c0 < c1) { unsigned t = c0; c0 = c1; c1 = t; }
    if (c0 == c1) { dxt1f_put(dst, c0, c1, 0); return; }
    uint32_t idx = dxt1f_indices(px, c0, c1);

    // least squares: pixel ~ w*A + (1-w)*B with w = 1, 0, 2/3, 1/3 for indices 0..3 (in thirds: 3, 0, 2, 1)
    static const int w3[4] = {3, 0, 2, 1};
    int aa = 0, bb = 0, ab = 0, at[3] = {0, 0, 0}, bt[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) {
        int w = w3[idx >> (2 * i) & 3], v = 3 - w;
        aa += w * w; bb += v * v; ab += w * v;
        for (int c = 0; c < 3; c++) { at[c] += w * px[i][c]; bt[c] += v * px[i][c]; }
    }
    int det = aa * bb - ab * ab;
    if (det != 0) {
        int A[3], B[3];
        for (int c = 0; c < 3; c++) {   // the weights are in thirds: x3 on both sides cancels one 3
            A[c] = (int)((3LL * (at[c] * bb - bt[c] * ab) + det / 2) / det);
            B[c] = (int)((3LL * (bt[c] * aa - at[c] * ab) + det / 2) / det);
        }
        unsigned n0 = dxt1f_565(A[0], A[1], A[2]), n1 = dxt1f_565(B[0], B[1], B[2]);
        if (n0 < n1) { unsigned t = n0; n0 = n1; n1 = t; }
        if (n0 != n1) { c0 = n0; c1 = n1; idx = dxt1f_indices(px, c0, c1); }   // (keeping the better of the two: same quality, 20% slower)
    }
    dxt1f_put(dst, c0, c1, idx);
}
