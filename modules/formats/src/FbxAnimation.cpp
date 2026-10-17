// FBX animation: skeletal clips and object clips, from ufbx_bake_anim. See docs/plans/wave9/FBX_IMPORT.md 2.6.
//
// SKELETAL. One OcTrack per (bone, channel), like glTF, straight from the baked keys: translation,
// rotation (quaternions, already unrolled) and scale, in the engine frame. A channel that is constant
// and equal to the bone's rest value emits nothing, so the pose sampler leaves that bone at rest. The
// keys of a topmost bone are premultiplied by the static world matrix of the non-bone parent, so they
// live in the same model space as the rest pose they replace.
//
// OBJECT. The motion of a non-bone node becomes a kOcAnimObject clip holding A(t) = W(t) * Q0^-1, the
// rule GltfImport.cpp importObjectAnimations documents (W the node's world, Q0 the rotation*scale baked
// into its mesh copy), sampled with ufbx_evaluate_transform.
#include "FbxInternal.hpp"

#if AVER_HAVE_UFBX

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace aver::fmt::fbx {
namespace {

constexpr f32 kPosTolCm   = 1e-4f;
constexpr f32 kRotTolComp = 5e-7f;     // per quaternion component, about 1e-6 rad
constexpr f32 kSclTol     = 1e-4f;

struct BakedGuard {
    ufbx_baked_anim* b = nullptr;
    BakedGuard() = default;
    BakedGuard(const BakedGuard&) = delete;
    BakedGuard& operator=(const BakedGuard&) = delete;
    ~BakedGuard() { if (b) ufbx_free_baked_anim(b); }
};

ufbx_matrix mulM(const ufbx_matrix& a, const ufbx_matrix& b) { return ufbx_matrix_mul(&a, &b); }

// ---- the glTF importer's decomposition rules, ported (GltfImport.cpp decomposeTrs / decomposeKey) ----

bool decomposeTrs(const f32 m[16], f32 t[3], f32 r[4], f32 s[3], std::string* why) {
    f32 len[3];
    for (int i = 0; i < 3; ++i)
        len[i] = std::sqrt(m[i*4+0]*m[i*4+0] + m[i*4+1]*m[i*4+1] + m[i*4+2]*m[i*4+2]);
    if (len[0] <= 1e-8f || len[1] <= 1e-8f || len[2] <= 1e-8f) {
        t[0] = m[12]; t[1] = m[13]; t[2] = m[14];
        r[0] = r[1] = r[2] = 0.0f; r[3] = 1.0f;
        s[0] = s[1] = s[2] = 1.0f;
        if (why) *why = "a degenerate (zero-scale) axis";
        return false;
    }
    f32 skew = 0.0f;
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b) {
            f32 d = 0.0f;
            for (int k = 0; k < 3; ++k) d += (m[a*4+k]/len[a]) * (m[b*4+k]/len[b]);
            d = std::fabs(d);
            skew = d > skew ? d : skew;
        }
    Mat4 mm;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) mm.m[i][j] = m[i*4+j];
    const Transform x = transformFromMatrix(mm);
    t[0] = x.position.x; t[1] = x.position.y; t[2] = x.position.z;
    r[0] = x.rotation.x; r[1] = x.rotation.y; r[2] = x.rotation.z; r[3] = x.rotation.w;
    s[0] = x.scale.x;    s[1] = x.scale.y;    s[2] = x.scale.z;
    if (skew > 1.0e-3f) {
        if (why) *why = "shear, which a translation/rotation/scale triple cannot represent";
        return false;
    }
    return true;
}

// One sampled transform as translation, rotation and scale for a clip key. A collapsed axis stays a
// zero scale (an animation that shrinks a node to nothing has to stay shrunk); `rotationKnown` comes back
// false when every axis collapsed and the caller borrows a neighbouring key's rotation.
bool decomposeKey(const f32 m[16], f32 t[3], f32 r[4], f32 s[3], bool& rotationKnown, std::string* why) {
    f32 len[3], big = 0.0f;
    for (int i = 0; i < 3; ++i) {
        len[i] = std::sqrt(m[i*4+0]*m[i*4+0] + m[i*4+1]*m[i*4+1] + m[i*4+2]*m[i*4+2]);
        big = std::fmax(big, len[i]);
    }
    t[0] = m[12]; t[1] = m[13]; t[2] = m[14];
    for (int i = 0; i < 3; ++i) s[i] = len[i];
    r[0] = r[1] = r[2] = 0.0f; r[3] = 1.0f;
    rotationKnown = false;

    int gone = -1, nGone = 0;
    for (int i = 0; i < 3; ++i) if (!(len[i] > 1e-6f * big)) { gone = i; ++nGone; }
    if (nGone > 1) return true;

    f32 n[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};
    for (int i = 0; i < 3; ++i)
        if (i != gone) for (int k = 0; k < 3; ++k) n[i*4+k] = m[i*4+k] / big;
    if (gone >= 0) {
        const f32* ra = &n[((gone + 1) % 3) * 4];
        const f32* rb = &n[((gone + 2) % 3) * 4];
        const f32 c[3] = {ra[1]*rb[2] - ra[2]*rb[1], ra[2]*rb[0] - ra[0]*rb[2], ra[0]*rb[1] - ra[1]*rb[0]};
        const f32 cl = std::sqrt(c[0]*c[0] + c[1]*c[1] + c[2]*c[2]);
        if (!(cl > 1e-12f)) return true;
        for (int k = 0; k < 3; ++k) n[gone*4 + k] = c[k] / cl;
    }
    f32 unusedT[3], normS[3];
    const bool ok = decomposeTrs(n, unusedT, r, normS, why);
    for (int i = 0; i < 3; ++i) if (i != gone && normS[i] < 0.0f) s[i] = -len[i];   // a mirror keeps its sign
    rotationKnown = true;
    return ok;
}

// ---- small comparisons ----

bool posDiffers(const Vec3& a, const Vec3& b) {
    return std::fabs(a.x - b.x) > kPosTolCm || std::fabs(a.y - b.y) > kPosTolCm || std::fabs(a.z - b.z) > kPosTolCm;
}
bool sclDiffers(const Vec3& a, const Vec3& b) {
    return std::fabs(a.x - b.x) > kSclTol || std::fabs(a.y - b.y) > kSclTol || std::fabs(a.z - b.z) > kSclTol;
}
bool rotDiffers(const Quat& a, const Quat& b) {
    const f64 d = std::fabs(f64(a.x) * b.x + f64(a.y) * b.y + f64(a.z) * b.z + f64(a.w) * b.w);
    return 1.0 - d > 1e-7;
}

// q and -q are one rotation: keep neighbours in one hemisphere so a runtime slerp takes the short arc.
void fixHemisphere(std::vector<Quat>& q) {
    for (usize i = 1; i < q.size(); ++i) {
        const Quat& p = q[i - 1];
        if (q[i].x * p.x + q[i].y * p.y + q[i].z * p.z + q[i].w * p.w < 0.0f) q[i] = Quat{-q[i].x, -q[i].y, -q[i].z, -q[i].w};
    }
}

OcTrack makeTrack(usize bone, u8 channel, const std::vector<f32>& times, std::vector<f32> values) {
    OcTrack t;
    t.boneIndex = static_cast<u16>(bone);
    t.channels = channel;
    t.interp = OcInterp::Linear;     // ufbx bakes every curve to linearly interpolated keys; steps are near-duplicate keys
    t.times = times;
    t.values = std::move(values);
    return t;
}

void pushVec(std::vector<f32>& v, const Vec3& p) { v.insert(v.end(), {p.x, p.y, p.z}); }
void pushQuat(std::vector<f32>& v, const Quat& q) { v.insert(v.end(), {q.x, q.y, q.z, q.w}); }

// ---- skeletal tracks ----

bool keysVary(const ufbx_baked_vec3_list& k) {
    for (usize i = 1; i < k.count; ++i)
        for (int c = 0; c < 3; ++c)
            if (std::fabs(k.data[i].value.v[c] - k.data[0].value.v[c]) > 1e-9) return true;
    return false;
}
bool keysVary(const ufbx_baked_quat_list& k) {
    for (usize i = 1; i < k.count; ++i)
        for (int c = 0; c < 4; ++c)
            if (std::fabs(k.data[i].value.v[c] - k.data[0].value.v[c]) > 1e-9) return true;
    return false;
}

// Emits the tracks of one bone from its baked node. `top` = no bone parent: its keys are folded through
// the static world of the node above it.
void addBoneTracks(Ctx& c, OcAnimation& clip, usize boneIdx, const OcBone& rest, const ufbx_baked_node& bn,
                   const ufbx_node* node, bool top, bool& animatedAncestor) {
    const ufbx_matrix A = (top && node->parent) ? node->parent->node_to_world : ufbx_identity_matrix;
    const bool fold = top && !isIdentity(A);
    // A channel counts as animated only if its keys actually differ: an exporter's keys on a held pose are a constant.
    const bool animT = bn.translation_keys.count > 1 && !bn.constant_translation && keysVary(bn.translation_keys);
    const bool animR = bn.rotation_keys.count > 1 && !bn.constant_rotation && keysVary(bn.rotation_keys);
    const bool animS = bn.scale_keys.count > 1 && !bn.constant_scale && keysVary(bn.scale_keys);
    (void)animatedAncestor;

    if (!fold) {
        if (animT) {
            std::vector<f32> times, vals;
            for (usize k = 0; k < bn.translation_keys.count; ++k) {
                times.push_back(f32(bn.translation_keys.data[k].time));
                pushVec(vals, c.basis.pos(bn.translation_keys.data[k].value));
            }
            clip.tracks.push_back(makeTrack(boneIdx, kOcChannelTranslation, times, std::move(vals)));
        } else if (bn.translation_keys.count >= 1) {
            const Vec3 v = c.basis.pos(bn.translation_keys.data[0].value);
            if (posDiffers(v, rest.translation)) {
                std::vector<f32> vals; pushVec(vals, v);
                clip.tracks.push_back(makeTrack(boneIdx, kOcChannelTranslation, {0.0f}, std::move(vals)));
            }
        }
        if (animR) {
            std::vector<Quat> q;
            std::vector<f32> times;
            for (usize k = 0; k < bn.rotation_keys.count; ++k) {
                times.push_back(f32(bn.rotation_keys.data[k].time));
                q.push_back(c.basis.quat(bn.rotation_keys.data[k].value).normalized());
            }
            fixHemisphere(q);
            std::vector<f32> vals;
            for (const Quat& e : q) pushQuat(vals, e);
            clip.tracks.push_back(makeTrack(boneIdx, kOcChannelRotation, times, std::move(vals)));
        } else if (bn.rotation_keys.count >= 1) {
            const Quat q = c.basis.quat(bn.rotation_keys.data[0].value).normalized();
            if (rotDiffers(q, rest.rotation)) {
                std::vector<f32> vals; pushQuat(vals, q);
                clip.tracks.push_back(makeTrack(boneIdx, kOcChannelRotation, {0.0f}, std::move(vals)));
            }
        }
        if (animS) {
            std::vector<f32> times, vals;
            for (usize k = 0; k < bn.scale_keys.count; ++k) {
                times.push_back(f32(bn.scale_keys.data[k].time));
                pushVec(vals, c.basis.scl(bn.scale_keys.data[k].value));
            }
            clip.tracks.push_back(makeTrack(boneIdx, kOcChannelScale, times, std::move(vals)));
        } else if (bn.scale_keys.count >= 1) {
            const Vec3 v = c.basis.scl(bn.scale_keys.data[0].value);
            if (sclDiffers(v, rest.scale)) {
                std::vector<f32> vals; pushVec(vals, v);
                clip.tracks.push_back(makeTrack(boneIdx, kOcChannelScale, {0.0f}, std::move(vals)));
            }
        }
        return;
    }

    // ---- a topmost bone under a non-identity parent chain: sample, fold, decompose ----
    std::vector<f64> times;
    if (animT) for (usize k = 0; k < bn.translation_keys.count; ++k) times.push_back(bn.translation_keys.data[k].time);
    if (animR) for (usize k = 0; k < bn.rotation_keys.count; ++k) times.push_back(bn.rotation_keys.data[k].time);
    if (animS) for (usize k = 0; k < bn.scale_keys.count; ++k) times.push_back(bn.scale_keys.data[k].time);
    if (times.empty()) times.push_back(0.0);
    std::sort(times.begin(), times.end());
    {
        usize kept = 0;
        for (const f64 t : times) if (kept == 0 || t - times[kept - 1] > 1e-9) times[kept++] = t;
        times.resize(kept);
    }
    const usize n = times.size();
    std::vector<Vec3> pos(n), scl(n);
    std::vector<Quat> rot(n);
    for (usize i = 0; i < n; ++i) {
        ufbx_transform lt = node->local_transform;
        if (bn.translation_keys.count > 0) lt.translation = ufbx_evaluate_baked_vec3(bn.translation_keys, times[i]);
        if (bn.rotation_keys.count > 0)    lt.rotation    = ufbx_evaluate_baked_quat(bn.rotation_keys, times[i]);
        if (bn.scale_keys.count > 0)       lt.scale       = ufbx_evaluate_baked_vec3(bn.scale_keys, times[i]);
        const ufbx_matrix lm = ufbx_transform_to_matrix(&lt);
        const ufbx_matrix wm = mulM(A, lm);
        const ufbx_transform wt = ufbx_matrix_to_transform(&wm);
        pos[i] = c.basis.pos(wt.translation);
        rot[i] = c.basis.quat(wt.rotation).normalized();
        scl[i] = c.basis.scl(wt.scale);
    }
    fixHemisphere(rot);

    bool varyT = false, varyR = false, varyS = false;
    for (usize i = 1; i < n; ++i) {
        varyT = varyT || posDiffers(pos[i], pos[0]);
        varyR = varyR || rotDiffers(rot[i], rot[0]);
        varyS = varyS || sclDiffers(scl[i], scl[0]);
    }
    std::vector<f32> ft(times.begin(), times.end());
    const std::vector<f32> zero = {0.0f};
    if (varyT) {
        std::vector<f32> vals; for (const Vec3& p : pos) pushVec(vals, p);
        clip.tracks.push_back(makeTrack(boneIdx, kOcChannelTranslation, ft, std::move(vals)));
    } else if (posDiffers(pos[0], rest.translation)) {
        std::vector<f32> vals; pushVec(vals, pos[0]);
        clip.tracks.push_back(makeTrack(boneIdx, kOcChannelTranslation, zero, std::move(vals)));
    }
    if (varyR) {
        std::vector<f32> vals; for (const Quat& q : rot) pushQuat(vals, q);
        clip.tracks.push_back(makeTrack(boneIdx, kOcChannelRotation, ft, std::move(vals)));
    } else if (rotDiffers(rot[0], rest.rotation)) {
        std::vector<f32> vals; pushQuat(vals, rot[0]);
        clip.tracks.push_back(makeTrack(boneIdx, kOcChannelRotation, zero, std::move(vals)));
    }
    if (varyS) {
        std::vector<f32> vals; for (const Vec3& s : scl) pushVec(vals, s);
        clip.tracks.push_back(makeTrack(boneIdx, kOcChannelScale, ft, std::move(vals)));
    } else if (sclDiffers(scl[0], rest.scale)) {
        std::vector<f32> vals; pushVec(vals, scl[0]);
        clip.tracks.push_back(makeTrack(boneIdx, kOcChannelScale, zero, std::move(vals)));
    }
}

// ---- object clips ----

// Keeps the keys a straight line between its neighbours does not reproduce (Ramer-Douglas-Peucker on
// the key index), within 1e-4 cm, ~1e-6 rad and 1e-4 of scale.
std::vector<u8> reduceKeepMask(const std::vector<f32>& t, const std::vector<f32>& pos, const std::vector<f32>& rot,
                               const std::vector<f32>& scl, bool useScale) {
    const usize n = t.size();
    std::vector<u8> keep(n, 0);
    if (n == 0) return keep;
    keep[0] = keep[n - 1] = 1;
    std::vector<std::pair<usize, usize>> stack;
    if (n > 2) stack.push_back({0, n - 1});
    while (!stack.empty()) {
        const auto [a, b] = stack.back();
        stack.pop_back();
        f32 worst = 0.0f;
        usize worstAt = 0;
        const f32 dt = t[b] - t[a];
        for (usize i = a + 1; i < b; ++i) {
            const f32 s = dt > 0.0f ? (t[i] - t[a]) / dt : 0.0f;
            f32 err = 0.0f;
            for (int k = 0; k < 3; ++k) {
                const f32 want = pos[a*3+k] + (pos[b*3+k] - pos[a*3+k]) * s;
                err = std::fmax(err, std::fabs(pos[i*3+k] - want) / kPosTolCm);
                if (useScale) {
                    const f32 sw = scl[a*3+k] + (scl[b*3+k] - scl[a*3+k]) * s;
                    err = std::fmax(err, std::fabs(scl[i*3+k] - sw) / kSclTol);
                }
            }
            const Quat q = Quat::slerp(Quat{rot[a*4], rot[a*4+1], rot[a*4+2], rot[a*4+3]},
                                       Quat{rot[b*4], rot[b*4+1], rot[b*4+2], rot[b*4+3]}, s);
            const f32 comps[4] = {q.x, q.y, q.z, q.w};
            for (int k = 0; k < 4; ++k) err = std::fmax(err, std::fabs(rot[i*4+k] - comps[k]) / kRotTolComp);
            if (err > worst) { worst = err; worstAt = i; }
        }
        if (worst > 1.0f) {
            keep[worstAt] = 1;
            stack.push_back({a, worstAt});
            stack.push_back({worstAt, b});
        }
    }
    return keep;
}

struct PendingObject {
    OcAnimation clip;
    u32 node = 0;
    i32 placement = -1;
    std::string stack, nodeName;
};

void objectClips(Ctx& c, const ufbx_anim_stack* stack, const std::string& stackName,
                 const std::vector<const ufbx_baked_node*>& bakedOf, f32 duration, std::vector<PendingObject>& pending) {
    const ufbx_scene* scene = c.scene;
    const usize nn = scene->nodes.count;

    std::vector<u8> animated(nn, 0);
    for (usize i = 0; i < nn; ++i) {
        const ufbx_baked_node* bn = bakedOf[i];
        if (!bn) continue;
        animated[i] = (bn->translation_keys.count > 1 && !bn->constant_translation && keysVary(bn->translation_keys)) ||
                      (bn->rotation_keys.count > 1 && !bn->constant_rotation && keysVary(bn->rotation_keys)) ||
                      (bn->scale_keys.count > 1 && !bn->constant_scale && keysVary(bn->scale_keys));
    }
    const auto ancestorAnimated = [&](const ufbx_node* n) {
        usize hops = 0;
        for (const ufbx_node* p = n->parent; p && !p->is_root && hops <= nn; p = p->parent, ++hops)
            if (animated[p->typed_id]) return true;
        return false;
    };

    for (usize i = 0; i < nn; ++i) {
        const ufbx_node* n = scene->nodes.data[i];
        if (n->is_root || c.boneOfNode[i].skeleton >= 0) continue;          // bones are skeletal
        const bool own = animated[i] != 0;
        const bool hasPlacement = c.placementOfNode[i] >= 0;
        const bool rides = hasPlacement && ancestorAnimated(n);
        if (!own && !rides) continue;
        if (c.skinnedMeshNode[i]) {
            c.note("an animated node that carries a skinned mesh (its own transform is baked at the bind pose)");
            continue;
        }
        if (n->camera || n->light) continue;
        if (!hasPlacement && (n->is_scale_helper || n->is_geometry_transform_helper)) continue;

        // Q0: what was baked into this node's mesh copy; nothing for a node with no mesh.
        Mat4 q0 = Mat4::identity();
        if (hasPlacement) {
            const Basis3& k = c.bakedBasisOfNode[i];
            for (int rr = 0; rr < 3; ++rr) for (int cc = 0; cc < 3; ++cc) q0.m[rr][cc] = k.k[rr * 3 + cc];
        }
        const f32* b = &q0.m[0][0];
        const f32 det = b[0] * (b[5]*b[10] - b[6]*b[9]) - b[1] * (b[4]*b[10] - b[6]*b[8]) + b[2] * (b[4]*b[9] - b[5]*b[8]);
        const std::string nodeName = str(n->name).empty() ? "Node" + std::to_string(i) : str(n->name);
        if (std::fabs(det) < 1e-30f) {
            c.note("an animated node ('" + nodeName + "') whose baked rotation/scale is singular; its motion was not imported");
            continue;
        }
        const Mat4 q0inv = q0.inverse();

        // The chain, node first, root last (the implicit root node is the identity under ADJUST_TRANSFORMS).
        std::vector<const ufbx_node*> chain;
        for (const ufbx_node* p = n; p && !p->is_root && chain.size() <= nn; p = p->parent) chain.push_back(p);

        // Sample times: every key of the chain, plus a bakeFps grid (an ancestor that turns swings its
        // children on an arc, which two end keys do not reproduce).
        std::vector<f64> times;
        for (const ufbx_node* p : chain) {
            const ufbx_baked_node* pb = bakedOf[p->typed_id];
            if (!pb) continue;
            for (usize k = 0; k < pb->translation_keys.count; ++k) times.push_back(pb->translation_keys.data[k].time);
            for (usize k = 0; k < pb->rotation_keys.count; ++k) times.push_back(pb->rotation_keys.data[k].time);
            for (usize k = 0; k < pb->scale_keys.count; ++k) times.push_back(pb->scale_keys.data[k].time);
        }
        const f64 fps = std::max<f64>(1.0, c.opt->bakeFps);
        if (duration * fps <= 20000.0)
            for (f64 t = 0.0; t < duration; t += 1.0 / fps) times.push_back(t);
        times.push_back(duration);
        std::sort(times.begin(), times.end());
        {
            usize kept = 0;
            for (const f64 t : times) if (t >= 0.0 && t <= duration + 1e-9 && (kept == 0 || t - times[kept - 1] > 1e-6)) times[kept++] = t;
            times.resize(kept);
        }
        if (times.empty()) continue;

        const usize ns = times.size();
        std::vector<Vec3> pos(ns), scl(ns);
        std::vector<Quat> rot(ns);
        std::vector<u8> rotKnown(ns, 1);
        std::string lostNote;
        f32 world0[16] = {};
        bool worldMoves = false;
        for (usize si = 0; si < ns; ++si) {
            ufbx_matrix w = ufbx_identity_matrix;
            for (usize ci = chain.size(); ci-- > 0;) {
                const ufbx_transform lt = ufbx_evaluate_transform(stack->anim, chain[ci], times[si] + stack->time_begin);
                const ufbx_matrix lm = ufbx_transform_to_matrix(&lt);
                w = mulM(w, lm);
            }
            if (n->mesh && hasPlacement) w = mulM(w, n->geometry_to_node);
            Mat4 wm = toMat4(w);
            if (si == 0) {
                std::memcpy(world0, &wm.m[0][0], sizeof world0);
            } else if (!worldMoves) {
                const f32* wf = &wm.m[0][0];
                for (int k = 0; k < 16; ++k)
                    if (std::fabs(wf[k] - world0[k]) > 1e-5f * (1.0f + std::fabs(world0[k]))) { worldMoves = true; break; }
            }
            const Mat4 am = q0inv * wm;
            f32 t[3], r[4], s[3];
            bool known = true;
            std::string lost;
            if (!decomposeKey(&am.m[0][0], t, r, s, known, &lost) && lostNote.empty()) lostNote = lost;
            pos[si] = c.basis.toEngine(t[0], t[1], t[2], false);
            rot[si] = c.basis.quat(r[0], r[1], r[2], r[3]);
            scl[si] = c.basis.scl(s[0], s[1], s[2]);
            rotKnown[si] = known ? 1 : 0;
        }
        // A node that only follows an ancestor whose world never changes has nothing to play.
        if (!own && !worldMoves) continue;
        if (!lostNote.empty())
            c.note("an animated node ('" + nodeName + "') whose motion carries " + lostNote + "; the rest of the transform was recovered");

        // A key whose axes all collapsed has no rotation of its own: it borrows a neighbour's.
        {
            usize firstKnown = 0;
            while (firstKnown < ns && !rotKnown[firstKnown]) ++firstKnown;
            if (firstKnown < ns)
                for (usize k = 0; k < ns; ++k)
                    if (!rotKnown[k]) rot[k] = rot[k == 0 ? firstKnown : k - 1];
        }
        fixHemisphere(rot);
        if (std::fabs(scl[0].x) < 1e-6f || std::fabs(scl[0].y) < 1e-6f || std::fabs(scl[0].z) < 1e-6f)
            c.note("an animated node ('" + nodeName + "') that starts with a zero-scale axis; start it (animtime) at a time it is not collapsed");
        bool scaled = false;
        for (const Vec3& s : scl) if (std::fabs(s.x - 1.0f) > 1e-4f || std::fabs(s.y - 1.0f) > 1e-4f || std::fabs(s.z - 1.0f) > 1e-4f) { scaled = true; break; }

        std::vector<f32> ft(times.begin(), times.end()), fp, fr, fs;
        for (usize k = 0; k < ns; ++k) { pushVec(fp, pos[k]); pushQuat(fr, rot[k]); pushVec(fs, scl[k]); }
        const std::vector<u8> keep = reduceKeepMask(ft, fp, fr, fs, scaled);

        OcTrack tr;
        tr.boneIndex = 0;
        tr.channels = static_cast<u8>(kOcChannelTranslation | kOcChannelRotation | (scaled ? kOcChannelScale : 0));
        tr.interp = OcInterp::Linear;
        for (usize k = 0; k < ns; ++k) {
            if (!keep[k]) continue;
            tr.times.push_back(ft[k]);
            tr.values.insert(tr.values.end(), fp.begin() + k * 3, fp.begin() + k * 3 + 3);
            tr.values.insert(tr.values.end(), fr.begin() + k * 4, fr.begin() + k * 4 + 4);
            if (scaled) tr.values.insert(tr.values.end(), fs.begin() + k * 3, fs.begin() + k * 3 + 3);
        }

        PendingObject po;
        po.clip.storage = OcAnimStorage::Keyframed;
        po.clip.flags = static_cast<u8>(kOcAnimObject | kOcAnimLoop);
        po.clip.duration = std::max(duration, tr.times.empty() ? 0.0f : tr.times.back());
        po.clip.tracks.push_back(std::move(tr));
        po.node = static_cast<u32>(i);
        po.placement = c.placementOfNode[i];
        po.stack = stackName;
        po.nodeName = nodeName;
        pending.push_back(std::move(po));
    }
}

} // namespace

std::vector<OcCurve> bakeMorphCurves(const ufbx_scene* scene, ufbx_baked_anim* baked) {
    std::vector<OcCurve> curves;
    for (usize i = 0; i < scene->blend_channels.count; ++i) {
        const ufbx_blend_channel* ch = scene->blend_channels.data[i];
        const ufbx_baked_element* el = ufbx_find_baked_element_by_element_id(baked, ch->element_id);
        if (!el) continue;
        for (usize p = 0; p < el->props.count; ++p) {
            const ufbx_baked_prop& bp = el->props.data[p];
            if (str(bp.name) != "DeformPercent" || bp.keys.count == 0) continue;
            OcCurve cv;
            cv.name = std::string(kFbxMorphCurvePrefix) + str(ch->name);
            cv.interp = OcInterp::Linear;
            for (usize k = 0; k < bp.keys.count; ++k) {
                cv.times.push_back(f32(bp.keys.data[k].time));
                cv.values.push_back(f32(std::min(1.0, std::max(0.0, double(bp.keys.data[k].value.x) / 100.0))));
            }
            curves.push_back(std::move(cv));
        }
    }
    return curves;
}

void importAnimations(Ctx& c) {
    const ufbx_scene* scene = c.scene;
    const FbxImportOptions& o = *c.opt;
    FbxImportResult& r = *c.out;
    const usize nn = scene->nodes.count;
    std::unordered_set<std::string> usedClipNames;
    std::vector<PendingObject> pending;

    for (usize si = 0; si < scene->anim_stacks.count; ++si) {
        const ufbx_anim_stack* stack = scene->anim_stacks.data[si];
        std::string stackName = str(stack->name);
        while (!stackName.empty() && stackName.back() == ' ') stackName.pop_back();
        if (stackName.empty()) stackName = "Take" + std::to_string(si + 1);

        ufbx_bake_opts bo = {};
        bo.trim_start_time = true;                                          // the clip starts at 0
        bo.resample_rate = double(o.bakeFps);
        bo.key_reduction_enabled = o.reduceKeys;
        bo.key_reduction_rotation = o.reduceKeys;                           // the sampler slerps
        bo.bake_transform_props = false;                                    // node keys come as TRS keys
        ufbx_error err = {};
        BakedGuard guard;
        guard.b = ufbx_bake_anim(scene, stack->anim, &bo, &err);
        if (!guard.b) {
            char msg[256];
            ufbx_format_error(msg, sizeof msg, &err);
            c.note("an animation stack that could not be baked ('" + stackName + "')");
            c.line("stack '" + stackName + "' not baked: " + msg);
            continue;
        }
        const ufbx_baked_anim* ba = guard.b;
        const f32 duration = f32(ba->playback_duration);
        if (!(duration > 1e-6f)) { c.line("stack '" + stackName + "' has no duration and was skipped"); continue; }

        std::vector<const ufbx_baked_node*> bakedOf(nn, nullptr);
        for (usize k = 0; k < ba->nodes.count; ++k)
            if (ba->nodes.data[k].typed_id < nn) bakedOf[ba->nodes.data[k].typed_id] = &ba->nodes.data[k];

        // ---- skeletal: one clip per skeleton the stack touches ----
        u32 produced = 0;
        for (usize sk = 0; sk < r.skeletons.size(); ++sk) {
            const OcSkeleton& skel = r.skeletons[sk];
            OcAnimation clip;
            clip.storage = OcAnimStorage::Keyframed;
            clip.skeletonRef = r.skeletonNames[sk];
            clip.duration = duration;
            bool ancestorNote = false;
            for (usize b = 0; b < skel.bones.size(); ++b) {
                const ufbx_node* node = c.boneNodes[sk][b];
                const ufbx_baked_node* bn = bakedOf[node->typed_id];
                if (!bn) continue;
                const bool top = skel.bones[b].parent == kOcBoneNoParent;
                if (top) {
                    for (const ufbx_node* p = node->parent; p && !p->is_root; p = p->parent) {
                        const ufbx_baked_node* pb = bakedOf[p->typed_id];
                        if (pb && ((pb->translation_keys.count > 1 && !pb->constant_translation) ||
                                   (pb->rotation_keys.count > 1 && !pb->constant_rotation) ||
                                   (pb->scale_keys.count > 1 && !pb->constant_scale))) ancestorNote = true;
                    }
                }
                addBoneTracks(c, clip, b, skel.bones[b], *bn, node, top, ancestorNote);
            }
            if (ancestorNote)
                c.note("an animated node above a skeleton root (its motion was sampled at rest, not carried)");
            if (clip.tracks.empty()) continue;

            // Root motion: the flag only has effect on an entity with CRootMotion, so Auto changes nothing
            // for existing content. The importer never edits keys.
            {
                const OcTrack* rootT = nullptr;
                bool anyRoot = false;
                for (const OcTrack& t : clip.tracks) {
                    if (t.boneIndex != 0) continue;
                    anyRoot = true;
                    if (t.channels & kOcChannelTranslation) rootT = &t;
                }
                f32 travel = 0.0f;
                if (rootT && rootT->times.size() > 1) {
                    const usize last = rootT->times.size() - 1;
                    const f32 dx = rootT->values[last * 3 + 0] - rootT->values[0];
                    const f32 dy = rootT->values[last * 3 + 1] - rootT->values[1];
                    travel = std::sqrt(dx * dx + dy * dy);
                }
                bool flag = false;
                if (o.rootMotion == FbxImportOptions::RootMotion::All) flag = anyRoot;
                else if (o.rootMotion == FbxImportOptions::RootMotion::Auto) flag = rootT && travel > o.rootMotionMinTravelCm;
                if (flag) clip.flags = static_cast<u8>(clip.flags | kOcAnimRootMotion);
                c.line("clip '" + stackName + "': root bone '" + skel.bones[0].name + "' travels " + std::to_string(travel) +
                       " cm in the plane" + (flag ? " (root motion flagged)" : ""));
            }

            std::string name = produced == 0 ? stackName : stackName + "_" + r.skeletonNames[sk];
            ++produced;
            if (!usedClipNames.insert(lowerOf(name)).second) {
                const std::string base = name;
                for (int k = 2; !usedClipNames.insert(lowerOf(name = base + "_" + std::to_string(k))).second; ++k) {}
            }
#if defined(AVER_OCMESH_HAS_MORPH)
            if (o.importBlendShapes) clip.curves = bakeMorphCurves(scene, guard.b);
#endif
            r.animations.push_back(std::move(clip));
            r.animationNames.push_back(name);
            r.animationSkeleton.push_back(static_cast<i32>(sk));
        }

        // ---- object clips ----
        objectClips(c, stack, stackName, bakedOf, duration, pending);
    }

    // Named after the node, or "<stack>_<node>" when the node moves in several stacks; unique among the
    // object clips and the skeletal ones.
    std::vector<u32> stacksOf(nn, 0);
    for (const PendingObject& p : pending) ++stacksOf[p.node];
    for (PendingObject& p : pending) {
        std::string name = stacksOf[p.node] > 1 ? p.stack + "_" + p.nodeName : p.nodeName;
        if (!usedClipNames.insert(lowerOf(name)).second) {
            const std::string base = name;
            for (int k = 2; !usedClipNames.insert(lowerOf(name = base + "_" + std::to_string(k))).second; ++k) {}
        }
        r.objectAnimations.push_back(std::move(p.clip));
        r.objectAnimationNames.push_back(name);
        r.objectAnimationPlacement.push_back(p.placement);
    }
}

} // namespace aver::fmt::fbx

#endif // AVER_HAVE_UFBX
