// ---- MANY LIGHTS: the light tree on the GPU (docs/plans/wave4/MANY_LIGHTS.md 3.1 and 3.2) ----
// Included from voxi_rt.hlsli straight after rdLightIndex, inside AVER_RD_LAMPS, so voxi_pt.hlsli sees it. It
// declares no resource: the tree is records in the light buffer (gRdLocalLights, t18) after the grid, built by
// VoxiRenderer::buildLocalLights from aver/voxi/LightTree.hpp, whose CPU twins (importance, sample, pdfOf) this file
// mirrors line for line. SM 6.0 arithmetic and loads only: no derivatives, no wave ops, no groupshared, no recursion,
// no arrays, so the one source is the Vulkan implementation too.
//
// A node is one 80-byte record (TreeNodeRec). Its fields by float index, which tests/render.voxi/LightTreeTest.cpp
// checks against the header:
#ifndef AVER_LIGHTTREE_HLSLI
#define AVER_LIGHTTREE_HLSLI

#define AVER_TREE_OFF_BMIN     0    // xyz AABB of the lights' centres (cm)
#define AVER_TREE_OFF_PHI      3    // power, irradiance-at-1-m units
#define AVER_TREE_OFF_BMAX     4
#define AVER_TREE_OFF_RANGE    7    // largest range (cm)
#define AVER_TREE_OFF_AXIS     8    // unit emission/orientation axis, zero length = omni
#define AVER_TREE_OFF_COSO     11   // cos of the orientation cone's half angle
#define AVER_TREE_OFF_COSE     12   // cos of the emission spread
#define AVER_TREE_OFF_LEFT     13   // node indices; a leaf has left = -1, right = the light's list index
#define AVER_TREE_OFF_RIGHT    14
#define AVER_TREE_OFF_PARENT   15
#define AVER_TREE_OFF_MAXR     16   // largest emitter radius (cm)
#define AVER_TREE_OFF_FLAGS    17
#define AVER_TREE_F_LEAF       1u
#define AVER_TREE_F_NOSHADOW   2u
#define AVER_TREE_F_RECT       4u
#define AVER_TREE_MAX_DEPTH    40u

struct RdTreeNode {
    float3 bmin; float phi;
    float3 bmax; float range;
    float3 axis; float cosO;
    float  cosE; float left; float right; float parent;
    float  maxR; float flags;
};

// Node `rec` is a buffer record (the header's node base plus the node index).
RdTreeNode rdTreeNode(uint rec) {
    RdTreeNode n;
    const float4 a = gRdLocalLights[rec].posRadius;
    const float4 b = gRdLocalLights[rec].radianceRange;
    const float4 c = gRdLocalLights[rec].axisKind;
    const float4 d = gRdLocalLights[rec].shape;
    const float4 e = gRdLocalLights[rec].right;
    n.bmin = a.xyz; n.phi = a.w;
    n.bmax = b.xyz; n.range = b.w;
    n.axis = c.xyz; n.cosO = c.w;
    n.cosE = d.x; n.left = d.y; n.right = d.z; n.parent = d.w;
    n.maxR = e.x; n.flags = e.y;
    return n;
}

// The tree header record (TreeHeader, LightTree.hpp): valid only when the light grid names one and the CPU built it.
struct RdTreeHdr {
    bool valid;
    uint count;        // node count
    uint nodeBase;     // buffer record of node 0
    uint leafMap;      // buffer record of the light-index -> leaf float pool
    uint samples;      // default S, M, hit M
    uint candidates;
    uint hitCandidates;
    float weightClamp;
};
RdTreeHdr rdTreeHeader() {
    RdTreeHdr h;
    h.valid = false; h.count = 0u; h.nodeBase = 0u; h.leafMap = 0u;
    h.samples = 1u; h.candidates = 1u; h.hitCandidates = 1u; h.weightClamp = 0.0;
    const uint grid = (uint)(gDecalParams.y + 0.5);
    if (grid == 0u) return h;
    const uint rec = (uint)(gRdLocalLights[grid].right.x + 0.5);   // 0 = no tree
    if (rec == 0u) return h;
    const float4 a = gRdLocalLights[rec].posRadius;
    const float4 b = gRdLocalLights[rec].radianceRange;
    h.valid = gRdLocalLights[rec].axisKind.x > 0.5;
    h.count = (uint)(a.y + 0.5);
    h.nodeBase = (uint)(a.z + 0.5);
    h.leafMap = (uint)(a.w + 0.5);
    h.samples = (uint)(b.x + 0.5);
    h.candidates = (uint)(b.y + 0.5);
    h.hitCandidates = (uint)(b.z + 0.5);
    h.weightClamp = b.w;
    return h;
}
// Lights in the tree (= list entries before the directional ones): the leaves of a binary tree of `count` nodes.
uint rdTreeLightCount(RdTreeHdr h) { return (h.count + 1u) >> 1u; }

// The directional lights of the list (the grid header's globals): they are not in the tree and reach every point.
uint rdTreeGlobals(out uint g0, out uint g1, out uint g2) {
    const float4 gk = gRdLocalLights[(uint)(gDecalParams.y + 0.5)].axisKind;
    g0 = (uint)max(gk.x, 0.0); g1 = (uint)max(gk.y, 0.0); g2 = (uint)max(gk.z, 0.0);
    return (uint)(gk.w + 0.5);
}

// A conservative bound of the node's lights' cheap irradiance luminance (rdLightWeight) at p for a receiver with
// unit normal N: the bound of Conty Estevez and Kulla 2018 in this record's units. The box of centres gives the
// nearest distance (no light is closer) and the window; the cone of the box (half angle thetaU) loosens the receiver
// and emitter cosines; a leaf's value is its light's cheap irradiance exactly. Zero is exact: a zero never hides a
// lit light.
float rdTreeImportance(RdTreeNode nd, float3 p, float3 N) {
    if (!(nd.phi > 0.0)) return 0.0;
    const float3 q = max(max(nd.bmin - p, p - nd.bmax), float3(0.0, 0.0, 0.0));
    const float dmin2 = dot(q, q);
    if (dmin2 >= nd.range * nd.range) return 0.0;
    const uint  flags = (uint)(nd.flags + 0.5);
    const bool  leaf = (flags & AVER_TREE_F_LEAF) != 0u;
    const bool  rect = (flags & AVER_TREE_F_RECT) != 0u;
    const float3 toC = 0.5 * (nd.bmin + nd.bmax) - p;
    const float hd = 0.5 * length(nd.bmax - nd.bmin);
    const float d = sqrt(dot(toC, toC));
    float cosI = 1.0;
    bool  emitOk = true;
    if (d > hd) {
        const float invD = 1.0 / d;
        const float sinU = min(hd * invD, 1.0);
        const float cosU = sqrt(max(1.0 - sinU * sinU, 0.0));
        const float cosIc = dot(N, toC) * invD;
        if (cosIc < cosU) {
            const float sinI = sqrt(max(1.0 - cosIc * cosIc, 0.0));
            cosI = max(cosIc * cosU + sinI * sinU, 0.0);
        }
        if (nd.cosO > -0.9999 && nd.cosE > -0.9999) {
            const float thetaU = asin(sinU);
            const float thetaO = acos(clamp(nd.cosO, -1.0, 1.0));
            const float thetaE = acos(clamp(nd.cosE, -1.0, 1.0));
            const float thetaW = acos(clamp(-dot(nd.axis, toC) * invD, -1.0, 1.0));
            emitOk = max(thetaW - thetaO - thetaU, 0.0) < thetaE;
        }
    }
    if (!emitOk) return 0.0;
    if (rect) cosI = max(cosI, 0.1);
    const float x2 = dmin2 / (nd.range * nd.range);
    float win = saturate(1.0 - x2 * x2);
    win *= win;
    // Distance term: a leaf is exact (its emitter radius floors it); an inner node's nearest-possible distance is floored
    // at its half-diagonal (LightTree.cpp explains). The zeros and the gating above stay strict.
    const float floor2 = leaf ? nd.maxR * nd.maxR : hd * hd;
    return nd.phi * 1e4 * cosI * win / max(max(dmin2, floor2), 1.0);
}

// Descends from the root, choosing a child in proportion to its importance; u in [0,1) is rescaled at each level.
// `lightIdx` is the light's list index and `pdf` the probability of reaching it. False where the descent meets a
// node whose children are both zero (a candidate of weight 0 for the caller, not an early exit: the estimator's
// divisor is the draw count). Depth loop bound 40, no recursion, no stack array.
bool rdTreeSample(RdTreeHdr h, float3 p, float3 N, float u, out uint lightIdx, out float pdf) {
    lightIdx = 0xFFFFFFFFu;
    pdf = 0.0;
    uint  node = 0u;
    float prob = 1.0;
    u = min(u, 0.99999994);
    [loop] for (uint it = 0u; it <= AVER_TREE_MAX_DEPTH; ++it) {
        const RdTreeNode nd = rdTreeNode(h.nodeBase + node);
        if (((uint)(nd.flags + 0.5) & AVER_TREE_F_LEAF) != 0u) {
            if (node == 0u && !(rdTreeImportance(nd, p, N) > 0.0)) return false;   // a one-light tree
            lightIdx = (uint)(nd.right + 0.5);
            pdf = prob;
            return true;
        }
        const uint  l = (uint)(nd.left + 0.5), r = (uint)(nd.right + 0.5);
        const float il = rdTreeImportance(rdTreeNode(h.nodeBase + l), p, N);
        const float ir = rdTreeImportance(rdTreeNode(h.nodeBase + r), p, N);
        const float sum = il + ir;
        if (!(sum > 0.0)) return false;
        const float pl = il / sum, pr = ir / sum;
        if (u < pl) { u = min(u / pl, 0.99999994); prob *= pl; node = l; }
        else        { u = min((u - pl) / pr, 0.99999994); prob *= pr; node = r; }
    }
    return false;
}

// The probability rdTreeSample reaches the light `lightIdx` with, walked UP from its leaf through the parent
// pointers (so the excluded exact light's q is computable): the same product, one traversal. 0 for an index that is
// not a tree light.
float rdTreePdfOf(RdTreeHdr h, uint lightIdx, float3 p, float3 N) {
    const float lf = rdListFloat(h.leafMap * 20u + lightIdx);
    if (lf < -0.5) return 0.0;
    uint  child = (uint)(lf + 0.5);
    float prob = 1.0;
    if (child == 0u) return rdTreeImportance(rdTreeNode(h.nodeBase), p, N) > 0.0 ? 1.0 : 0.0;
    [loop] for (uint it = 0u; it <= AVER_TREE_MAX_DEPTH; ++it) {
        const RdTreeNode cn = rdTreeNode(h.nodeBase + child);
        if (cn.parent < -0.5) break;
        const uint par = (uint)(cn.parent + 0.5);
        const RdTreeNode pn = rdTreeNode(h.nodeBase + par);
        const uint  l = (uint)(pn.left + 0.5), r = (uint)(pn.right + 0.5);
        const float il = rdTreeImportance(rdTreeNode(h.nodeBase + l), p, N);
        const float ir = rdTreeImportance(rdTreeNode(h.nodeBase + r), p, N);
        const float sum = il + ir;
        if (!(sum > 0.0)) return 0.0;
        prob *= (child == l ? il : ir) / sum;
        child = par;
    }
    return prob;
}

// ViewDebug::LightCount: the tree leaves with nonzero importance at p, by a stackless depth-first walk (parent
// pointers say where to return to), at most `maxVisits` steps. Debug only; CSRdManyLights calls it for that view.
uint rdTreeReach(RdTreeHdr h, float3 p, float3 N, uint maxVisits) {
    uint node = 0u, state = 0u, count = 0u;   // state 0 = arrived from above, 1 = back from the left child, 2 = back from the right
    [loop] for (uint it = 0u; it < maxVisits; ++it) {
        const RdTreeNode nd = rdTreeNode(h.nodeBase + node);
        bool up = false;
        if (state == 0u) {
            if (!(rdTreeImportance(nd, p, N) > 0.0)) up = true;
            else if (((uint)(nd.flags + 0.5) & AVER_TREE_F_LEAF) != 0u) { ++count; up = true; }
            else { node = (uint)(nd.left + 0.5); continue; }
        } else if (state == 1u) {
            node = (uint)(nd.right + 0.5);
            state = 0u;
            continue;
        } else {
            up = true;
        }
        if (up) {
            if (nd.parent < -0.5) break;
            const uint par = (uint)(nd.parent + 0.5);
            state = (uint)(rdTreeNode(h.nodeBase + par).left + 0.5) == node ? 1u : 2u;
            node = par;
        }
    }
    return count;
}

#endif
