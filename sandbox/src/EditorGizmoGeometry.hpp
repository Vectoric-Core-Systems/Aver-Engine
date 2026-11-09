// Gizmo colours and the line/mesh builders for the grid, move/rotate/scale gizmos, the player-start capsule and arrow, and pick geometry.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

// Gizmo colours: X red, Y green, Z blue, yellow highlight (UE5 palette).
static const Vec3 kAxisDir[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
static const Vec3 kAxisCol[3] = {{202.0f/255.0f, 38.0f/255.0f, 0.0f}, {103.0f/255.0f, 169.0f/255.0f, 0.0f},
                                  {44.0f/255.0f, 126.0f/255.0f, 237.0f/255.0f}};
static const Vec3 kAxisHi = {1.0f, 1.0f, 0.0f};

using rhi::primitives::appendBox;
using rhi::primitives::appendSphere;
// Ground quad in XY plane with half-extent s.
static inline void appendGround(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx, f32 s) {
    const u32 b = static_cast<u32>(v.size());
    v.push_back({-s,-s,0,0,0,1,-0.5f,-0.5f}); v.push_back({s,-s,0,0,0,1,0.5f,-0.5f});
    v.push_back({s,s,0,0,0,1,0.5f,0.5f}); v.push_back({-s,s,0,0,0,1,-0.5f,0.5f});
    idx.push_back(b); idx.push_back(b+1); idx.push_back(b+2); idx.push_back(b); idx.push_back(b+2); idx.push_back(b+3);
}

using rhi::primitives::appendBoxYaw;
using rhi::primitives::appendCylinderZ;


// Placeholder quadcopter from appendBoxYaw/appendCylinderZ.
static inline void appendDrone(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx) {
    // Central body: squarish, flatter than wide.
    constexpr f32 kBodyHX = 0.26f, kBodyHY = 0.26f, kBodyHZ = 0.15f;
    appendBoxYaw(v, idx, 0, 0, 0, kBodyHX, kBodyHY, kBodyHZ, 0.0f);

    // Four arms with motor pods and rotors.
    constexpr f32 kArmAngleDeg[4] = {45.0f, 135.0f, 225.0f, 315.0f};
    constexpr f32 kArmRInner = 0.34f;  // just inside the body's own corner (0.26*sqrt2 = 0.368) -- no seam
    constexpr f32 kArmROuter = 0.80f;  // hub distance from the drone's centre
    constexpr f32 kArmHalfLen = (kArmROuter - kArmRInner) * 0.5f;
    constexpr f32 kArmCenterR = (kArmROuter + kArmRInner) * 0.5f;
    constexpr f32 kArmHalfWidth = 0.045f, kArmHalfThick = 0.032f;
    constexpr f32 kHubRadius = 0.11f, kHubHalfHeight = 0.05f, kHubCenterZ = 0.08f;
    constexpr f32 kDiscRadius = 0.20f, kDiscHalfHeight = 0.014f, kDiscCenterZ = 0.14f;
    for (f32 deg : kArmAngleDeg) {
        const f32 rad = deg * kDegToRad;
        const f32 armX = kArmCenterR * std::cos(rad), armY = kArmCenterR * std::sin(rad);
        appendBoxYaw(v, idx, armX, armY, 0.0f, kArmHalfLen, kArmHalfWidth, kArmHalfThick, deg);

        const f32 hubX = kArmROuter * std::cos(rad), hubY = kArmROuter * std::sin(rad);
        appendCylinderZ(v, idx, hubX, hubY, kHubCenterZ, kHubRadius, kHubHalfHeight, 8);
        // Static rotor disc placeholder.
        appendCylinderZ(v, idx, hubX, hubY, kDiscCenterZ, kDiscRadius, kDiscHalfHeight, 10);
    }

    // Landing skids and support struts.
    constexpr f32 kSkidHalfLen = 0.30f, kSkidHalfWidth = 0.02f, kSkidHalfThick = 0.018f;
    constexpr f32 kSkidY = 0.20f, kSkidZ = -0.20f;
    appendBoxYaw(v, idx, 0.0f,  kSkidY, kSkidZ, kSkidHalfLen, kSkidHalfWidth, kSkidHalfThick, 0.0f);
    appendBoxYaw(v, idx, 0.0f, -kSkidY, kSkidZ, kSkidHalfLen, kSkidHalfWidth, kSkidHalfThick, 0.0f);

    constexpr f32 kStrutHalfX = 0.02f, kStrutHalfY = 0.02f, kStrutHalfZ = 0.016f;
    // Midpoint between body and skid.
    constexpr f32 kStrutX = 0.16f, kStrutZ = -0.166f;
    for (f32 sx : {-kStrutX, kStrutX})
        for (f32 sy : {-kSkidY, kSkidY})
            appendBoxYaw(v, idx, sx, sy, kStrutZ, kStrutHalfX, kStrutHalfY, kStrutHalfZ, 0.0f);
}

// Euler<->quaternion conversion (in EditorEuler.hpp).
using aver::editor::quatFromEulerDeg;
using aver::editor::eulerDegFromQuat;

// Snap to grid (step).
static inline f32 snapf(f32 v, f32 step) { return editor::snapValue(v, step); }

// Transform point (row-vector matrix).
static inline Vec3 xformPoint(const Mat4& m, const Vec3& p) {
    return { p.x*m.m[0][0]+p.y*m.m[1][0]+p.z*m.m[2][0]+m.m[3][0],
             p.x*m.m[0][1]+p.y*m.m[1][1]+p.z*m.m[2][1]+m.m[3][1],
             p.x*m.m[0][2]+p.y*m.m[1][2]+p.z*m.m[2][2]+m.m[3][2] };
}
// Transform direction (no translation).
static inline Vec3 xformVec(const Mat4& m, const Vec3& v) {
    return { v.x*m.m[0][0]+v.y*m.m[1][0]+v.z*m.m[2][0],
             v.x*m.m[0][1]+v.y*m.m[1][1]+v.z*m.m[2][1],
             v.x*m.m[0][2]+v.y*m.m[1][2]+v.z*m.m[2][2] };
}
// Ray-AABB intersection, entry distance to tHit.
static inline bool rayAabb(const Vec3& o, const Vec3& d, const Vec3& mn, const Vec3& mx, f32& tHit) {
    f32 tmin = 0.0f, tmax = 1e30f;
    for (int a = 0; a < 3; ++a) {
        const f32 od = (&o.x)[a], dd = (&d.x)[a], lo = (&mn.x)[a], hi = (&mx.x)[a];
        if (std::fabs(dd) < 1e-8f) { if (od < lo || od > hi) return false; }
        else {
            f32 t1 = (lo - od) / dd, t2 = (hi - od) / dd;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::fmax(tmin, t1); tmax = std::fmin(tmax, t2);
            if (tmin > tmax) return false;
        }
    }
    tHit = tmin; return true;
}
// MeshVertex to PickGeometry (built-in meshes only).
static inline aver::editor::PickGeometry buildPickGeometry(const std::vector<rhi::MeshVertex>& v,
                                                      const std::vector<u32>& idx) {
    aver::editor::PickGeometry g;
    g.positions.reserve(v.size() * 3);
    g.normals.reserve(v.size() * 3);
    for (const rhi::MeshVertex& mv : v) {
        g.positions.push_back(mv.px); g.positions.push_back(mv.py); g.positions.push_back(mv.pz);
        g.normals.push_back(mv.nx);   g.normals.push_back(mv.ny);   g.normals.push_back(mv.nz);
    }
    g.indices = idx;
    return g;
}
// Floor grid with axis markers.
static inline void buildGrid(std::vector<rhi::LineVertex>& v, f32 ext, f32 step) {
    const f32 g = 0.26f;
    const f32 lift = step * 0.02f;
    for (f32 x = -ext; x <= ext + step * 0.001f; x += step) {
        v.push_back({x, -ext, lift, g, g, g}); v.push_back({x, ext, lift, g, g, g});
    }
    for (f32 y = -ext; y <= ext + step * 0.001f; y += step) {
        v.push_back({-ext, y, lift, g, g, g}); v.push_back({ext, y, lift, g, g, g});
    }
    const f32 axisLift = lift * 1.5f;
    v.push_back({0,0,axisLift, 0.80f,0.25f,0.25f}); v.push_back({ext,0,axisLift, 0.80f,0.25f,0.25f});
    v.push_back({0,0,axisLift, 0.28f,0.72f,0.30f}); v.push_back({0,ext,axisLift, 0.28f,0.72f,0.30f});
}

// Add coloured line segment.
static inline void gzLine(std::vector<rhi::LineVertex>& v, const Vec3& a, const Vec3& b, const Vec3& c) {
    v.push_back({a.x,a.y,a.z, c.x,c.y,c.z}); v.push_back({b.x,b.y,b.z, c.x,c.y,c.z});
}
// Translate arrow for axis a.
static inline std::vector<rhi::LineVertex> buildMoveAxis(int a, const Vec3& c) {
    std::vector<rhi::LineVertex> v;
    const Vec3 A = kAxisDir[a], P = kAxisDir[(a+1)%3], Q = kAxisDir[(a+2)%3];
    gzLine(v, {0,0,0}, A, c);
    const Vec3 tip = A, base = A * 0.80f;
    for (int k = 0; k < 4; ++k) { f32 t = k * (kPi * 0.5f); Vec3 r = P*(std::cos(t)*0.07f) + Q*(std::sin(t)*0.07f); gzLine(v, base+r, tip, c); }
    return v;
}
// Rotation ring perpendicular to axis a.
static inline std::vector<rhi::LineVertex> buildRotRing(int a, const Vec3& c) {
    std::vector<rhi::LineVertex> v;
    const Vec3 P = kAxisDir[(a+1)%3], Q = kAxisDir[(a+2)%3];
    const int N = 64; Vec3 prev{};
    for (int k = 0; k <= N; ++k) { f32 t = k * (kTwoPi / N); Vec3 p = P*std::cos(t) + Q*std::sin(t); if (k > 0) gzLine(v, prev, p, c); prev = p; }
    return v;
}
// Scale handle for axis a (shaft with box tip).
static inline std::vector<rhi::LineVertex> buildScaleAxis(int a, const Vec3& c) {
    std::vector<rhi::LineVertex> v;
    const Vec3 A = kAxisDir[a], P = kAxisDir[(a+1)%3], Q = kAxisDir[(a+2)%3];
    gzLine(v, {0,0,0}, A * 0.86f, c);
    const Vec3 ctr = A * 0.93f; const f32 h = 0.07f;
    Vec3 cor[8]; int i = 0;
    for (int sx = -1; sx <= 1; sx += 2) for (int sy = -1; sy <= 1; sy += 2) for (int sz = -1; sz <= 1; sz += 2)
        cor[i++] = ctr + A*(h*sx) + P*(h*sy) + Q*(h*sz);
    const int e[12][2] = {{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
    for (auto& pr : e) gzLine(v, cor[pr[0]], cor[pr[1]], c);
    return v;
}

// Player Start capsule wire (UE style: rings, sides, pole arcs).
static inline std::vector<rhi::LineVertex> buildCapsuleWire(f32 radius, f32 halfHeight, const Vec3& c) {
    std::vector<rhi::LineVertex> v;
    const f32 tall = halfHeight * 2.0f;              // pole-to-pole height
    const f32 zLo = radius, zHi = tall - radius;      // hemisphere seam heights
    constexpr int kSeg = 24;
    for (const f32 z : {zLo, zHi}) {
        Vec3 prev{};
        for (int k = 0; k <= kSeg; ++k) {
            const f32 t = k * (kTwoPi / kSeg);
            const Vec3 p{radius * std::cos(t), radius * std::sin(t), z};
            if (k > 0) gzLine(v, prev, p, c);
            prev = p;
        }
    }
    for (int k = 0; k < 4; ++k) {
        const f32 t = k * (kPi * 0.5f);
        const f32 sx = radius * std::cos(t), sy = radius * std::sin(t);
        gzLine(v, {sx, sy, zLo}, {sx, sy, zHi}, c);
    }
    // Half circles: 180-degree arcs through each dome.
    constexpr int kHalfSeg = kSeg / 2;
    const f32 seamZ[2] = {zLo, zHi};
    const f32 sign[2]  = {-1.0f, 1.0f};   // bottom dome curves toward z=0, top toward z=tall
    for (int cap = 0; cap < 2; ++cap) {
        for (int plane = 0; plane < 2; ++plane) {   // 0 = XZ, 1 = YZ
            Vec3 prev{};
            for (int k = 0; k <= kHalfSeg; ++k) {
                const f32 t = k * (kPi / kHalfSeg);
                const f32 rx = radius * std::cos(t);
                const f32 rz = seamZ[cap] + sign[cap] * radius * std::sin(t);
                const Vec3 p = plane == 0 ? Vec3{rx, 0.0f, rz} : Vec3{0.0f, rx, rz};
                if (k > 0) gzLine(v, prev, p, c);
                prev = p;
            }
        }
    }
    return v;
}
// Player Start facing arrow (local +X).
static inline std::vector<rhi::LineVertex> buildPlayerStartArrow(f32 centerZ, f32 length, const Vec3& c) {
    std::vector<rhi::LineVertex> v;
    const Vec3 base{0.0f, 0.0f, centerZ}, tip{length, 0.0f, centerZ};
    gzLine(v, base, tip, c);
    const f32 headLen = length * 0.28f, headWidth = length * 0.16f;
    const Vec3 back{tip.x - headLen, 0.0f, centerZ};
    gzLine(v, tip, {back.x, headWidth, centerZ}, c);
    gzLine(v, tip, {back.x, -headWidth, centerZ}, c);
    return v;
}
