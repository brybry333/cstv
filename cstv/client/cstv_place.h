// Screen placement maths: orientation on a surface or in the air, and fitting the screen around
// nearby geometry. No SDK types, so it can be tested offline with a fake trace.
#pragma once
#include <algorithm>
#include <cmath>

struct V3 {
    float x, y, z;
    V3 operator+(const V3 &o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(const V3 &o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(float k) const { return {x * k, y * k, z * k}; }
    V3 operator-() const { return {-x, -y, -z}; }
};
inline float dot(const V3 &a, const V3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(const V3 &a, const V3 &b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float len(const V3 &a) { return sqrtf(dot(a, a)); }
inline V3 norm(const V3 &a)
{
    float l = len(a);
    return l > 1e-6f ? a * (1 / l) : V3{0, 0, 0};
}

// Size at scale 1 and the limits shared by every kind of world screen.
const float PLACE_W = 512, PLACE_H = 288, PLACE_SCALE_MIN = 0.1f, PLACE_SCALE_MAX = 4;

struct Placement {
    V3 pos, normal, right, up;   // normal = the side viewers see; cross(right, up) == normal
    float scale;
    bool floating;
};

// Source angles in degrees: pitch positive looks down.
inline V3 view_dir(float pitch, float yaw)
{
    float p = pitch * (float)M_PI / 180, y = yaw * (float)M_PI / 180;
    return {cosf(p) * cosf(y), cosf(p) * sinf(y), -sinf(p)};
}

// Flat on a surface with normal n: on walls the picture stays upright; on a floor or ceiling its
// top points the way the player's view's top does (away on a floor, back toward them on a ceiling),
// so it reads the right way round from where it was placed.
inline void surface_axes(const V3 &n, float yaw, V3 &right, V3 &up)
{
    float y = yaw * (float)M_PI / 180;
    V3 h = {cosf(y), sinf(y), 0};
    V3 want = fabsf(n.z) > 0.7f ? (n.z > 0 ? h : -h) : V3{0, 0, 1};
    up = norm(want - n * dot(want, n));
    right = norm(cross(up, n));
}

// Floating in front of the player, facing them: upright unless they aim well up or down.
inline void float_axes(float pitch, float yaw, V3 &normal, V3 &right, V3 &up)
{
    float p = pitch - 360 * truncf(pitch / 360);   // fmodf(pitch, 360), which is a glibc 2.38 symbol now (Steam runtime: 2.31)
    if (p > 180) p -= 360;
    if (p < -180) p += 360;
    V3 face = view_dir(fabsf(p) < 20 ? 0 : p, yaw);
    float y = yaw * (float)M_PI / 180;
    right = {sinf(y), -cosf(y), 0};
    up = cross(right, face);
    normal = -face;
}

// Fit rule: only the centre has to be on the aimed surface (a wall screen) or in the air (floating);
// the edges may hang over open space, so a screen fits on a pillar. Geometry the screen would cut into
// (a floor, a ceiling, an adjacent wall) pushes it along its plane away from that; if that isn't
// possible it gets smaller. False = it doesn't fit at min_scale (p is then where it got to, at
// min_scale).
//
// trace(a, b): the free fraction of the segment a -> b (1 = nothing in the way, 0 = a is in solid).
// The test plane is 4 units in front of the surface (p.pos is already 2 off it), so the surface
// itself is never hit; rays go from the centre to the edges and corners.
template <class Trace>
bool fit_screen(Placement &p, float min_scale, const Trace &trace)
{
    // targets in half-extents: 4 corners, 4 edge middles, 8 points between them
    static const float pts[][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}, {1, 0}, {-1, 0}, {0, 1}, {0, -1},
                                   {1, .5f}, {1, -.5f}, {-1, .5f}, {-1, -.5f}, {.5f, 1}, {-.5f, 1}, {.5f, -1}, {-.5f, -1}};
    const float margin = 1;
    int slid_r = 0, slid_u = 0;   // which way each axis has been pushed so far
    for (int step = 0; step < 40; step++) {
        float hw = PLACE_W / 2 * p.scale, hh = PLACE_H / 2 * p.scale;
        V3 o = p.floating ? p.pos : p.pos + p.normal * 2;
        float f[16];
        bool any = false, middle = false;
        for (int i = 0; i < 16; i++) {
            f[i] = trace(o, o + p.right * (pts[i][0] * hw) + p.up * (pts[i][1] * hh));
            if (f[i] >= 1) continue;
            any = true;
            if (pts[i][0] == 0 || pts[i][1] == 0) middle = true;
        }
        if (!any) return true;
        // How far the +right, -right, +up and -up edges have to come in to leave each hit point
        // outside, taking the edge that moves least for its size. A ray to an edge middle says for sure
        // which edge is in the way (a floor below, a wall beside); the slanted rays can blame the wrong
        // one (a floor seen by a ray toward the right edge), so they only count when no middle ray hit:
        // after the push they're traced again.
        float need[4] = {0, 0, 0, 0};
        for (int i = 0; i < 16; i++) {
            float kr = pts[i][0], ku = pts[i][1];
            if (f[i] >= 1 || (middle && kr != 0 && ku != 0)) continue;
            float mr = kr != 0 ? hw - f[i] * fabsf(kr) * hw : 1e9f, mu = ku != 0 ? hh - f[i] * fabsf(ku) * hh : 1e9f;
            int e = mr / hw <= mu / hh ? (kr > 0 ? 0 : 1) : (ku > 0 ? 2 : 3);
            need[e] = std::max(need[e], (e < 2 ? mr : mu) + margin);
        }
        float sr = need[1] - need[0], su = need[3] - need[2];   // the push, + = toward +right / +up
        bool both_r = need[0] > 0 && need[1] > 0, both_u = need[2] > 0 && need[3] > 0;
        // pushed back the way it came: the gap is too small for this size
        bool back = (sr > 0 && slid_r < 0) || (sr < 0 && slid_r > 0) || (su > 0 && slid_u < 0) || (su < 0 && slid_u > 0);
        if (!both_r && !both_u && !back) {
            V3 np = p.pos + p.right * sr + p.up * su;
            V3 no = p.floating ? np : np + p.normal * 2;
            bool ok = trace(o, no) >= 1;
            // the centre must still be on a surface at the same depth (not pushed off a pillar's edge)
            if (ok && !p.floating) {
                V3 s = np - p.normal * 2;
                float fc = trace(s + p.normal * 6, s - p.normal * 6);
                ok = fc > 0.45f && fc < 0.55f;
            }
            if (ok) {
                p.pos = np;
                if (sr) slid_r = sr > 0 ? 1 : -1;
                if (su) slid_u = su > 0 ? 1 : -1;
                continue;
            }
        }
        // smaller: straight to what the free space allows when both sides of an axis are blocked
        float k = 0.85f;
        if (both_r) k = std::min(k, (2 * hw - need[0] - need[1]) / (2 * hw) * 0.97f);
        if (both_u) k = std::min(k, (2 * hh - need[2] - need[3]) / (2 * hh) * 0.97f);
        k = std::max(k, 0.3f);
        if (p.scale <= min_scale) return false;
        p.scale = std::max(min_scale, p.scale * k);
    }
    return false;
}
