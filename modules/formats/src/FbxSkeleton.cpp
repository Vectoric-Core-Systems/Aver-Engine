// FBX skeletons, skin weights and sockets. See docs/plans/wave9/FBX_IMPORT.md 2.5.
//
// THE REST POSE IS THE BIND POSE (the skin clusters' bind_to_world), not whatever the scene's frame-0
// node transforms happen to be, and the inverse binds are re-derived from it, so a bind-pose skeleton
// skins to the identity by construction -- the glTF importer's invariant (GltfImport.cpp importSkins).
// A topmost bone has no bone parent, so its "local" transform IS its model-space transform: the whole
// chain above it is folded in, which is what the glTF importer does for the armature-root-carries-a-
// 100x-rotation case.
#include "FbxInternal.hpp"

#if AVER_HAVE_UFBX

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace aver::fmt::fbx {
namespace {

ufbx_matrix mulM(const ufbx_matrix& a, const ufbx_matrix& b) { return ufbx_matrix_mul(&a, &b); }
ufbx_matrix invM(const ufbx_matrix& a) { return ufbx_matrix_invert(&a); }

f64 maxAbs(const ufbx_matrix& m) {
    f64 big = 0.0;
    for (int i = 0; i < 12; ++i) big = std::max(big, std::fabs(m.v[i]));
    return big;
}
f64 matDiff(const ufbx_matrix& a, const ufbx_matrix& b) {
    f64 d = 0.0;
    for (int i = 0; i < 12; ++i) d = std::max(d, std::fabs(a.v[i] - b.v[i]));
    return d;
}

} // namespace

bool skinBindMatrix(Ctx& c, const ufbx_node* node, const ufbx_skin_deformer* skin, ufbx_matrix& out) {
    bool have = false, mismatch = false;
    for (usize i = 0; i < skin->clusters.count; ++i) {
        const ufbx_skin_cluster* cl = skin->clusters.data[i];
        if (!cl->bone_node) continue;
        const ufbx_matrix m = mulM(cl->bind_to_world, cl->geometry_to_bone);
        if (!have) { out = m; have = true; continue; }
        if (matDiff(out, m) > 1e-3 * (1.0 + maxAbs(out))) mismatch = true;
    }
    if (mismatch)
        c.line("skin of '" + str(node->name) + "': its clusters' bind matrices disagree; the first cluster's was used for the whole mesh");
    return have;
}

i32 skeletonOfSkin(const Ctx& c, const ufbx_skin_deformer* skin) {
    for (usize i = 0; i < skin->clusters.count; ++i) {
        const ufbx_node* b = skin->clusters.data[i]->bone_node;
        if (!b || b->typed_id >= c.boneOfNode.size()) continue;
        if (c.boneOfNode[b->typed_id].skeleton >= 0) return c.boneOfNode[b->typed_id].skeleton;
    }
    return -1;
}

void buildSkeletons(Ctx& c, const std::vector<const ufbx_node*>& skinned) {
    const ufbx_scene* scene = c.scene;
    const usize nn = scene->nodes.count;
    c.boneOfNode.assign(nn, BoneRef{});

    // ---- the bone set B ----
    std::vector<u8> member(nn, 0);
    std::vector<const ufbx_skin_cluster*> clusterOf(nn, nullptr);
    std::vector<const ufbx_node*> seeds;
    for (const ufbx_node* mn : skinned) {
        const ufbx_mesh* mesh = mn->mesh;
        if (mesh->skin_deformers.count > 1) c.note("more than one skin deformer on a mesh (only the first is used)");
        const ufbx_skin_deformer* skin = mesh->skin_deformers.data[0];
        if (skin->skinning_method == UFBX_SKINNING_METHOD_DUAL_QUATERNION ||
            skin->skinning_method == UFBX_SKINNING_METHOD_BLENDED_DQ_LINEAR)
            c.note("dual-quaternion skinning (imported as linear blend skinning)");
        for (usize i = 0; i < skin->clusters.count; ++i) {
            const ufbx_skin_cluster* cl = skin->clusters.data[i];
            const ufbx_node* b = cl->bone_node;
            if (!b) continue;
            if (!member[b->typed_id]) { member[b->typed_id] = 1; seeds.push_back(b); }
            if (!clusterOf[b->typed_id]) clusterOf[b->typed_id] = cl;
        }
    }
    // A file with bones and no skinned mesh -- an animation-only export -- still gets its rig, from the bone
    // nodes themselves: rest = each node's own transform (there is no bind pose to prefer).
    if (skinned.empty()) {
        for (usize i = 0; i < nn; ++i) {
            const ufbx_node* n = scene->nodes.data[i];
            if (n->is_root || !n->bone || !n->visible) continue;
            member[i] = 1;
            seeds.push_back(n);
        }
    }
    if (seeds.empty()) return;

    // Every node on the parent path between two members: a helper node (geometry or scale helper, a
    // Maya group) sits inside the chain and is part of it.
    for (const ufbx_node* s : seeds) {
        std::vector<const ufbx_node*> chain;
        for (const ufbx_node* p = s->parent; p && !p->is_root; p = p->parent) chain.push_back(p);
        i64 highest = -1;
        for (usize i = 0; i < chain.size(); ++i) if (member[chain[i]->typed_id]) highest = static_cast<i64>(i);
        for (i64 i = 0; i < highest; ++i) member[chain[usize(i)]->typed_id] = 1;
    }
    // Bone-typed descendants of members: end bones and helper limbs the skin never weights.
    {
        std::vector<const ufbx_node*> stack;
        for (usize i = 0; i < nn; ++i) if (member[i]) stack.push_back(scene->nodes.data[i]);
        while (!stack.empty()) {
            const ufbx_node* n = stack.back();
            stack.pop_back();
            for (usize k = 0; k < n->children.count; ++k) {
                const ufbx_node* ch = n->children.data[k];
                if (member[ch->typed_id] || !ch->bone) continue;
                member[ch->typed_id] = 1;
                stack.push_back(ch);
            }
        }
    }

    // ---- group by topmost member ----
    std::vector<const ufbx_node*> tops;
    std::vector<i32> groupOf(nn, -1);
    for (usize i = 0; i < nn; ++i) {
        if (!member[i]) continue;
        const ufbx_node* top = scene->nodes.data[i];
        for (const ufbx_node* p = top->parent; p && !p->is_root; p = p->parent) if (member[p->typed_id]) top = p;
        i32 g = -1;
        for (usize t = 0; t < tops.size(); ++t) if (tops[t] == top) { g = static_cast<i32>(t); break; }
        if (g < 0) { g = static_cast<i32>(tops.size()); tops.push_back(top); }
        groupOf[i] = g;
    }

    std::unordered_set<std::string> usedSkeletonNames;
    c.boneNodes.assign(tops.size(), {});
    c.skeletonTop = tops;
    for (usize g = 0; g < tops.size(); ++g) {
        // Parents before children: depth-first pre-order from the top, children in file order.
        std::vector<const ufbx_node*>& order = c.boneNodes[g];
        {
            std::vector<const ufbx_node*> stack{tops[g]};
            while (!stack.empty()) {
                const ufbx_node* n = stack.back();
                stack.pop_back();
                order.push_back(n);
                for (usize k = n->children.count; k-- > 0;) {
                    const ufbx_node* ch = n->children.data[k];
                    if (member[ch->typed_id] && groupOf[ch->typed_id] == static_cast<i32>(g)) stack.push_back(ch);
                }
            }
        }
        for (usize b = 0; b < order.size(); ++b) c.boneOfNode[order[b]->typed_id] = BoneRef{static_cast<i32>(g), static_cast<i32>(b)};

        OcSkeleton sk;
        sk.bones.resize(order.size());
        sk.rootBone = 0;
        std::vector<ufbx_matrix> rest(order.size());
        std::unordered_set<std::string> usedBoneNames;
        for (usize b = 0; b < order.size(); ++b) {
            const ufbx_node* n = order[b];
            OcBone& bone = sk.bones[b];
            const ufbx_node* par = n->parent;
            const i32 pi = (par && !par->is_root && member[par->typed_id] && groupOf[par->typed_id] == static_cast<i32>(g))
                         ? c.boneOfNode[par->typed_id].bone : -1;
            bone.parent = pi >= 0 ? pi : kOcBoneNoParent;

            // The bind pose where the skin has one; a chain node or end bone no cluster weights keeps its
            // own local transform under its parent's REST pose, so a bind pose that differs from frame 0
            // does not tear the chain apart.
            const ufbx_skin_cluster* cl = clusterOf[n->typed_id];
            ufbx_matrix local;
            if (cl) {
                rest[b] = cl->bind_to_world;
                local = pi >= 0 ? mulM(invM(rest[usize(pi)]), rest[b]) : rest[b];
            } else {
                local = pi >= 0 ? n->node_to_parent : n->node_to_world;
                rest[b] = pi >= 0 ? mulM(rest[usize(pi)], local) : local;
            }

            std::string name = str(n->name);
            if (name.empty()) name = "Bone" + std::to_string(b);
            if (!usedBoneNames.insert(name).second) {
                const std::string base = name;
                for (int k = 2; !usedBoneNames.insert(name = base + "_" + std::to_string(k)).second; ++k) {}
                c.note("two bones named '" + base + "'; the later ones were renamed so tracks and sockets stay unambiguous");
            }
            bone.name = name;

            const ufbx_transform tr = ufbx_matrix_to_transform(&local);
            const ufbx_matrix back = ufbx_transform_to_matrix(&tr);
            if (matDiff(back, local) > 1e-3 * (1.0 + maxAbs(local)))
                c.note("a bone ('" + name + "') whose rest transform carries shear or a mirror that a translation/"
                       "rotation/scale triple cannot hold; the recovered triple was kept");
            bone.translation = c.basis.pos(tr.translation);
            bone.rotation    = c.basis.quat(tr.rotation).normalized();
            bone.scale       = c.basis.scl(tr.scale);
        }

        // The inverse bind is the inverse of the model-space rest, re-derived (GltfImport.cpp does the same).
        {
            std::vector<Mat4> model(sk.bones.size(), Mat4::identity());
            for (usize i = 0; i < sk.bones.size(); ++i) {
                Transform x;
                x.position = sk.bones[i].translation;
                x.rotation = sk.bones[i].rotation;
                x.scale    = sk.bones[i].scale;
                const Mat4 local = x.toMatrix();
                const i32 p = sk.bones[i].parent;
                model[i] = (p >= 0 && usize(p) < i) ? local * model[usize(p)] : local;
                const Mat4 inv = model[i].inverse();
                for (int rr = 0; rr < 4; ++rr)
                    for (int cc = 0; cc < 4; ++cc) sk.bones[i].inverseBind[rr * 4 + cc] = inv.m[rr][cc];
            }
        }

        // ---- sockets: a non-bone node hanging off a bone of THIS skeleton ----
        // 1 its parent is a bone; 2 it is not one; 3 it holds no mesh, camera or light (a skinned mesh is
        // routinely parented under the armature); 4 no bone below it; helper nodes are chain, not attachments.
        for (usize ni = 0; ni < nn; ++ni) {
            const ufbx_node* n = scene->nodes.data[ni];
            if (n->is_root || member[ni]) continue;
            const ufbx_node* par = n->parent;
            if (!par || par->is_root || !member[par->typed_id] || groupOf[par->typed_id] != static_cast<i32>(g)) continue;
            if (n->mesh || n->camera || n->light || n->bone) continue;
            if (n->is_geometry_transform_helper || n->is_scale_helper) continue;
            bool boneBelow = false;
            {
                std::vector<const ufbx_node*> stack{n};
                while (!stack.empty() && !boneBelow) {
                    const ufbx_node* d = stack.back();
                    stack.pop_back();
                    for (usize k = 0; k < d->children.count; ++k) {
                        if (member[d->children.data[k]->typed_id]) { boneBelow = true; break; }
                        stack.push_back(d->children.data[k]);
                    }
                }
            }
            if (boneBelow) continue;

            OcSocket k;
            k.bone = static_cast<u32>(c.boneOfNode[par->typed_id].bone);
            k.name = str(n->name);
            if (k.name.empty()) k.name = "Socket";
            if (sk.socket(k.name)) {
                c.note("two sockets named '" + k.name + "'; the later was renamed to keep both reachable by name");
                const std::string base = k.name;
                for (int i = 2; sk.socket(k.name); ++i) k.name = base + std::to_string(i);
            }
            const ufbx_transform& lt = n->local_transform;
            k.translation = c.basis.pos(lt.translation);
            k.rotation    = c.basis.quat(lt.rotation).normalized();
            k.scale       = c.basis.scl(lt.scale);
            sk.sockets.push_back(std::move(k));
            ++c.out->stats.sockets;
        }

        if (sk.bones.size() > 256)
            c.note("a skeleton of " + std::to_string(sk.bones.size()) + " bones (the skinning pass carries 256 per mesh)");
        if (sk.bones.size() > 65535) c.note("a skeleton of more than 65535 bones (bone indices are 16-bit)");

        std::string name = sk.bones.empty() ? std::string() : sk.bones[0].name;
        if (name.empty()) name = "Skeleton" + std::to_string(g);
        {
            const std::string base = name;
            for (int k = 2; !usedSkeletonNames.insert(lowerOf(name)).second; ++k) name = base + "_" + std::to_string(k);
        }
        c.out->stats.bones += static_cast<u32>(sk.bones.size());
        c.out->skeletonNames.push_back(name);
        c.out->skeletons.push_back(std::move(sk));
    }
}

void writeSkinStreams(Ctx& c, const ufbx_skin_deformer* skin, const std::vector<u32>& cps, OcMeshData& m) {
    const i32 skel = skeletonOfSkin(c, skin);
    std::vector<i32> boneOfCluster(skin->clusters.count, -1);
    for (usize i = 0; i < skin->clusters.count; ++i) {
        const ufbx_node* b = skin->clusters.data[i]->bone_node;
        if (!b || b->typed_id >= c.boneOfNode.size()) continue;
        const BoneRef r = c.boneOfNode[b->typed_id];
        if (r.skeleton == skel) boneOfCluster[i] = r.bone;
    }

    const usize n = cps.size();
    m.joints.assign(n * kOcMeshInfluences, 0);
    m.weights.assign(n * kOcMeshInfluences, 0.0f);

    struct Entry { i32 bone; f64 weight; };
    std::vector<Entry> entries;
    for (usize v = 0; v < n; ++v) {
        entries.clear();
        const u32 cp = cps[v];
        if (cp < skin->vertices.count) {
            const ufbx_skin_vertex& sv = skin->vertices.data[cp];
            for (u32 k = 0; k < sv.num_weights; ++k) {
                const ufbx_skin_weight& w = skin->weights.data[sv.weight_begin + k];
                if (w.cluster_index >= boneOfCluster.size()) continue;
                const i32 bone = boneOfCluster[w.cluster_index];
                if (bone < 0 || !(w.weight > 0.0)) continue;
                entries.push_back({bone, w.weight});
            }
        }
        // ufbx lists a vertex's weights largest first; a filtered or hand-built list is not trusted to be.
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.weight > b.weight; });

        const usize kept = std::min<usize>(entries.size(), kOcMeshInfluences);
        if (entries.size() > kOcMeshInfluences) {
            ++c.out->stats.truncatedInfluenceVertices;
            c.out->stats.maxDroppedWeight = std::max(c.out->stats.maxDroppedWeight, f32(entries[kOcMeshInfluences].weight));
        }
        f64 sum = 0.0;
        for (usize k = 0; k < kept; ++k) sum += entries[k].weight;
        u16* jo = &m.joints[v * kOcMeshInfluences];
        f32* wo = &m.weights[v * kOcMeshInfluences];
        if (kept == 0 || !(sum > 1e-12)) {
            jo[0] = 0;
            wo[0] = 1.0f;                       // the back-fill rule: no weight binds the vertex rigidly to bone 0
            continue;
        }
        for (usize k = 0; k < kept; ++k) {
            jo[k] = static_cast<u16>(std::min<i32>(entries[k].bone, 65535));
            wo[k] = static_cast<f32>(entries[k].weight / sum);
        }
    }
}

} // namespace aver::fmt::fbx

#endif // AVER_HAVE_UFBX
