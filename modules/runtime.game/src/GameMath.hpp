// Small geometry and transform helpers, copied from SandboxApp.cpp.
//
// COPIED AND NOT PROMOTED TO A SHARED MODULE, deliberately. These are file-static in
// SandboxApp.cpp; hoisting them into Aver.Core or Aver.RHI would edit the editor's translation
// unit, and the whole discipline of this lift is that sandbox/src/SandboxApp.cpp is not touched, so
// the editor cannot regress and the 18x9 gate oracle stays a valid before-picture. Two copies of
// forty lines of cube vertices is the cheaper half of that trade for now. De-duplication is the
// later slice, and it is proved by the gates being bit-identical rather than by inspection.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/rhi/RHI.hpp"

#include <cmath>
#include <vector>

namespace aver::game {

// Appends an axis-aligned box centred at (cx,cy,cz) with half-extent h.
inline void appendBox(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx, f32 cx, f32 cy, f32 cz, f32 h) {
    const f32 p[8][3] = {{-h,-h,-h},{h,-h,-h},{h,h,-h},{-h,h,-h},{-h,-h,h},{h,-h,h},{h,h,h},{-h,h,h}};
    struct Face { f32 n[3]; int c[4]; };
    const Face faces[6] = {{{1,0,0},{1,2,6,5}},{{-1,0,0},{0,4,7,3}},{{0,1,0},{3,7,6,2}},
                           {{0,-1,0},{0,1,5,4}},{{0,0,1},{4,5,6,7}},{{0,0,-1},{0,3,2,1}}};
    const f32 quadUV[4][2] = {{0,0},{1,0},{1,1},{0,1}};
    for (const Face& f : faces) {
        const u32 b = static_cast<u32>(v.size());
        for (int k = 0; k < 4; ++k) {
            const f32* c = p[f.c[k]];
            v.push_back({cx+c[0],cy+c[1],cz+c[2],f.n[0],f.n[1],f.n[2],quadUV[k][0],quadUV[k][1]});
        }
        idx.push_back(b); idx.push_back(b+1); idx.push_back(b+2);
        idx.push_back(b); idx.push_back(b+2); idx.push_back(b+3);
    }
}

// Appends a UV sphere of radius r with +Z as the pole.
inline void appendSphere(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx, f32 r, u32 rings, u32 sectors) {
    const u32 base = static_cast<u32>(v.size());
    for (u32 ring = 0; ring <= rings; ++ring) {
        const f32 phi = kPi * (static_cast<f32>(ring) / static_cast<f32>(rings));
        const f32 z = std::cos(phi), rad = std::sin(phi);
        for (u32 sec = 0; sec <= sectors; ++sec) {
            const f32 theta = 2.0f * kPi * (static_cast<f32>(sec) / static_cast<f32>(sectors));
            const f32 nx = rad * std::cos(theta), ny = rad * std::sin(theta), nz = z;
            v.push_back({nx * r, ny * r, nz * r, nx, ny, nz,
                         static_cast<f32>(sec) / static_cast<f32>(sectors),
                         static_cast<f32>(ring) / static_cast<f32>(rings)});
        }
    }
    const u32 stride = sectors + 1;
    for (u32 ring = 0; ring < rings; ++ring) {
        for (u32 sec = 0; sec < sectors; ++sec) {
            const u32 a = base + ring * stride + sec, b = a + stride;
            idx.push_back(a); idx.push_back(b); idx.push_back(a + 1);
            idx.push_back(a + 1); idx.push_back(b); idx.push_back(b + 1);
        }
    }
}

} // namespace aver::game
