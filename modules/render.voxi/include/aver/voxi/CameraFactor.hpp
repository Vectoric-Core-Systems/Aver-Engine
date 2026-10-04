// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
#pragma once
#include "aver/core/Types.hpp"

#include <cmath>

// CameraFactor -- recovers the SEPARATE worldToView and viewToClip matrices of a lookAtLH *
// perspectiveLH camera from the engine's combined viewProj.
//
// WHY THIS EXISTS. IDevice::camera() and every per-frame consumer in this renderer (curViewProj_,
// prevViewProj_) only ever carry the TWO matrices already multiplied together -- view * proj, row-
// major, row-vector (RHI.hpp: "row-major, row-vector viewProj = view*proj"). Consumers that
// reconstruct view-space positions (formerly NVIDIA NRD, now any denoiser or reprojection pass)
// want the two SEPARATELY -- they decompose viewToClip to recover the projection frustum and the
// scene's handedness, and rebuild every pixel's view-space position from viewToClip alone before
// rotating by worldToView. Handing them identity for worldToView and the combined viewProj for
// viewToClip is silently wrong. This header is the fix: given only the combined matrix, it
// reconstructs the two such a consumer wants, dependency-light (aver/core/Types.hpp only, no Mat4)
// so it can be exercised without pulling in the RHI or the renderer.
//
// A CLOSED-FORM RECOVERY, NOT A GENERAL DECOMPOSITION. This does not attempt to factor an arbitrary
// 4x4 into "a view-like part" and "a projection-like part" -- that problem is underdetermined
// without knowing the projection has no shear and the view is a rigid rotation. It instead re-
// derives the SPECIFIC algebra of Mat4::lookAtLH composed with Mat4::perspectiveLH (see the two
// statics in aver/core/Math.hpp) and inverts exactly that. Every assumption the derivation leans
// on -- an orthonormal, proper (non-mirrored) rotation for the view; a projection with no shear
// outside the two off-centre terms cx/cy; a finite, non-degenerate scale -- is verified numerically
// before anything is returned, and factor() REFUSES (returns false, writes nothing) the moment any
// of them does not hold to the stated tolerance, rather than returning its best guess. See
// factor()'s own comment for exactly what is checked and why each check is the one that catches a
// specific failure mode (orthographic input, a mirrored camera, a non-finite input, a matrix that
// merely happens to be some other transform entirely).
//
// ALL ARITHMETIC IN DOUBLE. The 16 inputs are f32 (that is what IDevice::camera hands back and what
// every existing consumer here already stores), but the subtractions below -- col0 minus its
// projection onto f, to isolate the basis vector's own contribution -- are exactly the computation
// that loses relative precision fastest in single precision: two f32 values of comparable magnitude
// (the projection component can be nearly as large as the column itself near the poles of a
// near-vertical look direction) are subtracted to leave a much smaller remainder, which is then
// normalised. Doing that step in double keeps the mantissa bits single precision would have already
// discarded, at zero cost to the CALLER's precision -- the inputs and outputs are f32 regardless,
// this only protects the arithmetic in between.
namespace aver::voxi {

class CameraFactor {
public:
    // vp: the combined viewProj, row-major and row-vector -- vp[r*4+c] is the same element
    // Mat4::m[r][c] would be, exactly the layout IDevice::camera()/setCamera() already use and the
    // one curViewProj_/prevViewProj_ already store. Assumed to be V*P for V = Mat4::lookAtLH(...)
    // and P = Mat4::perspectiveLH(...) (both this engine's own conventions; see the two statics in
    // aver/core/Math.hpp) -- ANY other 4x4, including a proper camera built some other way, is
    // correctly rejected rather than silently misread as one of these.
    //
    // outWorldToView / outViewToClip: written in the SAME row-major layout as vp itself (so
    // outWorldToView[r*4+c] == V.m[r][c] and likewise for P), which is what lets a caller memcpy
    // either straight into a column-major/column-vector consumer exactly the way the combined viewProj
    // already is (a row-major row-vector array and a column-major column-vector array of the SAME
    // transform are the identical sequence of 16 numbers).
    //
    // Returns false, and leaves outWorldToView/outViewToClip UNTOUCHED (not identity, not a best
    // guess -- untouched), when vp does not verifiably have the assumed structure. See factor()'s
    // body for the full list of checks; a caller must treat false as "this frame's camera cannot be
    // trusted" and act accordingly (a caller skips the dependent dispatch that frame
    // rather than ever falling through to a stale or synthesised encoding).
    static bool factor(const f32 vp[16], f32 outWorldToView[16], f32 outViewToClip[16]) {
        // ---- 0. every input must be finite. A NaN/Inf anywhere poisons every derived quantity, and
        // every check below assumes it is comparing real numbers -- run this first so nothing past
        // this point has to guard against it again.
        for (int i = 0; i < 16; ++i)
            if (!std::isfinite(static_cast<double>(vp[i]))) return false;

        const double m[16] = {
            vp[0],  vp[1],  vp[2],  vp[3],
            vp[4],  vp[5],  vp[6],  vp[7],
            vp[8],  vp[9],  vp[10], vp[11],
            vp[12], vp[13], vp[14], vp[15],
        };
        // m[r*4+c] == VP row r, column c (the task's "VP[r][c]").

        // ---- 1. f = the view's forward axis, read straight off column 3 of rows 0-2 ----
        // VP[i][3] = f_i for i = 0,1,2 (P's column 3 is (0,0,1,0), so V's own column 3 -- always 0
        // for lookAtLH -- contributes nothing and only the f_i * 1 term from P.m[2][3] survives).
        // lookAtLH's f is unit length by construction, so a genuine input has |f| == 1 to float
        // precision; an ORTHOGRAPHIC projection has NO such column (P.m[2][3] = 0 there, so this
        // whole column reads 0 regardless of V), and a viewProj uniformly scaled by k != 1 reads
        // |f| == k -- both are exactly what this check exists to catch, not a special case bolted on
        // for either.
        const double fx = m[3], fy = m[7], fz = m[11];
        const double fLen2 = fx * fx + fy * fy + fz * fz;
        const double fLen = std::sqrt(fLen2);
        constexpr double kUnitTol = 1e-3;
        if (!std::isfinite(fLen) || std::fabs(fLen - 1.0) > kUnitTol) return false;

        // ---- 2. s (right) and u (up), the other two rows of V, recovered from columns 0 and 1 ----
        // VP[i][0] = a*s_i + cx*f_i, so subtracting the f component the off-centre term cx contributes
        // (cx = dot(col0, f), since f is unit length) leaves exactly a*s -- normalise for s, and the
        // subtracted length back out for the scale a. Column 1 / u / b / cy is the identical
        // computation. This is the subtraction the class comment says to do in double.
        const double col0x = m[0], col0y = m[4], col0z = m[8];
        const double col1x = m[1], col1y = m[5], col1z = m[9];
        const double col2x = m[2], col2y = m[6], col2z = m[10];

        const double cx = col0x * fx + col0y * fy + col0z * fz;
        const double sx = col0x - cx * fx, sy = col0y - cx * fy, sz = col0z - cx * fz;
        const double a = std::sqrt(sx * sx + sy * sy + sz * sz);

        const double cy = col1x * fx + col1y * fy + col1z * fz;
        const double ux = col1x - cy * fx, uy = col1y - cy * fy, uz = col1z - cy * fz;
        const double b = std::sqrt(ux * ux + uy * uy + uz * uz);

        // a, b MUST be positive and finite: lookAtLH's s and u are unit vectors, so perspectiveLH's
        // xScale/yScale survive whole in a/b. A zero or negative scale is not a camera this engine
        // ever produces (perspectiveLH's yScale = 1/tan(fovY/2) is positive for any fovY in (0, pi)),
        // and dividing by either below would be undefined for zero, silently wrong for negative.
        if (!(a > 0.0) || !std::isfinite(a) || !(b > 0.0) || !std::isfinite(b)) return false;
        const double s0 = sx / a, s1 = sy / a, s2 = sz / a;
        const double u0 = ux / b, u1 = uy / b, u2 = uz / b;
        const double f0 = fx, f1 = fy, f2 = fz;   // already unit length to kUnitTol

        // ---- 3. s, u, f must be mutually orthogonal -- lookAtLH's basis always is; anything that
        // came from a differently-shaped view matrix generally is not, and this is the check that
        // would notice.
        constexpr double kOrthoTol = 1e-3;
        const double dSU = s0 * u0 + s1 * u1 + s2 * u2;
        const double dSF = s0 * f0 + s1 * f1 + s2 * f2;
        const double dUF = u0 * f0 + u1 * f1 + u2 * f2;
        if (std::fabs(dSU) > kOrthoTol || std::fabs(dSF) > kOrthoTol || std::fabs(dUF) > kOrthoTol)
            return false;

        // ---- 4. handedness -- lookAtLH's s = cross(up, f) and u = cross(f, s) always compose into
        // a PROPER (determinant +1) basis: cross(s, u) == f. A mirrored camera -- any view matrix
        // built from a reflection, which no path in this engine constructs but a caller could still
        // pass in -- has cross(s, u) == -f instead, and this is the one check that tells the two
        // apart; the orthogonality test above cannot, since a mirror is still orthonormal. Compared
        // against a mid-point threshold (0.5, not bare > 0) precisely so a proper basis (~+1.0) and a
        // mirrored one (~-1.0) are separated by a wide margin and float noise near either extreme
        // cannot flip the verdict.
        const double crossX = s1 * u2 - s2 * u1;
        const double crossY = s2 * u0 - s0 * u2;
        const double crossZ = s0 * u1 - s1 * u0;
        const double handedness = crossX * f0 + crossY * f1 + crossZ * f2;
        if (!(handedness > 0.5)) return false;

        // ---- 5. column 2 must be PARALLEL to f (col2 == c*f exactly for a shear-free projection) --
        // VP[i][2] = c*f_i has no s or u component at all, unlike columns 0/1's off-centre terms, so
        // this checks a residual rather than subtracting one out. Relative to |c| (floored so a c
        // near zero cannot make an ordinary residual look enormous by comparison) because the
        // absolute size of an acceptable residual scales with c the same way it does with a/b above.
        const double c = col2x * f0 + col2y * f1 + col2z * f2;
        const double resX = col2x - c * f0, resY = col2y - c * f1, resZ = col2z - c * f2;
        const double residual = std::sqrt(resX * resX + resY * resY + resZ * resZ);
        constexpr double kParallelTol = 1e-3;
        constexpr double kCFloor = 1e-6;
        if (!std::isfinite(c) || residual > kParallelTol * std::fmax(std::fabs(c), kCFloor))
            return false;

        // ---- 6. row 3: translation and the projection's constant term ----
        // VP[3][3] = tz (V's own translation row has a 1 in this slot, and P's column 3 is
        // (0,0,1,0), so only tz*1 from P.m[2][3] contributes -- the same reason f above read
        // straight off column 3). tx/ty invert the two off-centre terms the same way s/u did above;
        // d is whatever perspectiveLH's fixed (0,0,d,0) row leaves once c*tz is subtracted out.
        const double tz = m[15];
        const double tx = (m[12] - cx * tz) / a;
        const double ty = (m[13] - cy * tz) / b;
        const double d  = m[14] - c * tz;
        if (!std::isfinite(tz) || !std::isfinite(tx) || !std::isfinite(ty) || !std::isfinite(d))
            return false;

        // ---- 7. the ultimate check: recompose V*P from everything recovered above and demand it
        // reproduces the input. Every check up to here verified a NECESSARY property of a
        // lookAtLH*perspectiveLH product; this is the SUFFICIENT one -- nothing above proves the
        // recovered matrices are the unique pair that multiplies back to vp, only this does.
        // Elementwise, absolute-plus-relative: an element near zero (most of the off-diagonal terms)
        // needs an absolute floor, and an element scaled by a large translation (this engine's own
        // 200000 cm far plane, or an eye position in the tens of thousands of centimetres) needs a
        // tolerance that grows with it rather than one pinned to a small scene's numbers.
        double recomposed[16];
        // Row i (0,1,2): recomposed[i*4+j] = s_i*P.col_j-row0-component ... -- rather than a generic
        // 4x4 multiply, this writes the exact six formulas the class comment derives, which is also
        // a second, independent statement of them (the caller has already checked they match the
        // hand-derivation in the header comment; this is the arithmetic that has to agree with it).
        const double sArr[3] = {s0, s1, s2};
        const double uArr[3] = {u0, u1, u2};
        const double fArr[3] = {f0, f1, f2};
        for (int i = 0; i < 3; ++i) {
            recomposed[i * 4 + 0] = a * sArr[i] + cx * fArr[i];
            recomposed[i * 4 + 1] = b * uArr[i] + cy * fArr[i];
            recomposed[i * 4 + 2] = c * fArr[i];
            recomposed[i * 4 + 3] = fArr[i];
        }
        recomposed[12] = a * tx + cx * tz;
        recomposed[13] = b * ty + cy * tz;
        recomposed[14] = c * tz + d;
        recomposed[15] = tz;

        constexpr double kRecomposeAbsTol = 1e-3;
        constexpr double kRecomposeRelTol = 1e-3;
        for (int i = 0; i < 16; ++i) {
            const double tol = kRecomposeAbsTol + kRecomposeRelTol * std::fabs(m[i]);
            if (!std::isfinite(recomposed[i]) || std::fabs(recomposed[i] - m[i]) > tol) return false;
        }

        // ---- every check passed: write both outputs, row-major, same layout as vp itself ----
        outWorldToView[0]  = static_cast<f32>(s0); outWorldToView[1]  = static_cast<f32>(u0);
        outWorldToView[2]  = static_cast<f32>(f0); outWorldToView[3]  = 0.0f;
        outWorldToView[4]  = static_cast<f32>(s1); outWorldToView[5]  = static_cast<f32>(u1);
        outWorldToView[6]  = static_cast<f32>(f1); outWorldToView[7]  = 0.0f;
        outWorldToView[8]  = static_cast<f32>(s2); outWorldToView[9]  = static_cast<f32>(u2);
        outWorldToView[10] = static_cast<f32>(f2); outWorldToView[11] = 0.0f;
        outWorldToView[12] = static_cast<f32>(tx); outWorldToView[13] = static_cast<f32>(ty);
        outWorldToView[14] = static_cast<f32>(tz); outWorldToView[15] = 1.0f;

        outViewToClip[0]  = static_cast<f32>(a);  outViewToClip[1]  = 0.0f;
        outViewToClip[2]  = 0.0f;                 outViewToClip[3]  = 0.0f;
        outViewToClip[4]  = 0.0f;                 outViewToClip[5]  = static_cast<f32>(b);
        outViewToClip[6]  = 0.0f;                 outViewToClip[7]  = 0.0f;
        outViewToClip[8]  = static_cast<f32>(cx); outViewToClip[9]  = static_cast<f32>(cy);
        outViewToClip[10] = static_cast<f32>(c);  outViewToClip[11] = 1.0f;
        outViewToClip[12] = 0.0f;                 outViewToClip[13] = 0.0f;
        outViewToClip[14] = static_cast<f32>(d);  outViewToClip[15] = 0.0f;
        return true;
    }
};

} // namespace aver::voxi
