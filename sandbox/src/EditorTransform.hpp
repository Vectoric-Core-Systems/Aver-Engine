#pragma once
// Composing a 4x4 world matrix from a position, an Euler rotation in degrees and a scale.
//
// ONE IMPLEMENTATION, TWO EDITORS. The actor editor composes designer-file placements this way and
// the graph editor's Viewport tab composes COMP-record components the same way, into the same
// PreviewDraw::world. Two copies of this arithmetic would be two chances to disagree about what
// "yaw 90" means, and the actor editor's own localAxisWorldDir comment already records what that
// costs at a smaller scale -- a gizmo drawn along an axis the mesh does not actually turn about.
//
// HEADER-ONLY AND CORE-ONLY, so tests/editor's GraphEditorLoadSaveTest -- which compiles
// GraphEditor.cpp with no ImGui, no preview module and no link beyond Formats/Core/Platform --
// still builds. That is the same trade EditorEuler.hpp argues for its own contents; see
// aver/world/LevelTransform.hpp for the fuller version of the argument.
//
// NOT MERGED WITH quatFromEulerDeg, deliberately. That function composes the identical Rz*Ry*Rx
// order, but takes its angles as (roll, pitch, yaw) while everything on this path -- Aver.Scene's
// Rot, ActorBuilder.Place, and the .ocgraph COMP record's own `rot=` -- states them as (yaw, pitch,
// roll). Unifying them would mean silently reversing one caller's argument order, which is the
// single most dangerous edit possible in rotation code.
#include "aver/core/Types.hpp"

#include <cmath>

namespace aver::editor {

// Builds a row-major, ROW-VECTOR world matrix (the layout PreviewDraw::world documents) from a
// position in centimetres, a rotation in DEGREES as yaw/pitch/roll, and a per-axis scale.
// Rotation is applied Z (yaw) then Y (pitch) then X (roll) -- the order the framework applies them.
inline void composeEditorTransform(const f32 pos[3], const f32 rotDeg[3], const f32 scale[3],
                                    f32 out[16]) {
    constexpr f32 kPi = 3.14159265358979f;
    const f32 y = rotDeg[0] * kPi / 180.0f, p = rotDeg[1] * kPi / 180.0f, r = rotDeg[2] * kPi / 180.0f;
    const f32 cy = std::cos(y), sy = std::sin(y);
    const f32 cp = std::cos(p), sp = std::sin(p);
    const f32 cr = std::cos(r), sr = std::sin(r);

    const f32 m00 = cy * cp,  m01 = sy * cp,  m02 = -sp;
    const f32 m10 = cy * sp * sr - sy * cr, m11 = sy * sp * sr + cy * cr, m12 = cp * sr;
    const f32 m20 = cy * sp * cr + sy * sr, m21 = sy * sp * cr - cy * sr, m22 = cp * cr;

    out[0]  = m00 * scale[0]; out[1]  = m01 * scale[0]; out[2]  = m02 * scale[0]; out[3]  = 0.0f;
    out[4]  = m10 * scale[1]; out[5]  = m11 * scale[1]; out[6]  = m12 * scale[1]; out[7]  = 0.0f;
    out[8]  = m20 * scale[2]; out[9]  = m21 * scale[2]; out[10] = m22 * scale[2]; out[11] = 0.0f;
    out[12] = pos[0];         out[13] = pos[1];         out[14] = pos[2];         out[15] = 1.0f;
}

// out = a * b, row-vector convention -- so a CHILD's local matrix times its PARENT's world matrix is
// the child's world matrix, and never the other way round. `out` must not alias `a` or `b`.
inline void multiplyEditorTransform(const f32 a[16], const f32 b[16], f32 out[16]) {
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            out[row * 4 + col] = a[row * 4 + 0] * b[0 * 4 + col] + a[row * 4 + 1] * b[1 * 4 + col] +
                                  a[row * 4 + 2] * b[2 * 4 + col] + a[row * 4 + 3] * b[3 * 4 + col];
}

} // namespace aver::editor
