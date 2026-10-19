// Binding a render mesh to a coarse proxy cage: closest-point barycentric coordinates plus a signed normal offset,
// evaluated on demand against any proxy triangle. Pure arithmetic; no GPU, no scene, no Jolt.
#include "aver/core/Log.hpp"
#include "aver/softbody/ProxyBind.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;
namespace sb = aver::softbody;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

struct RenderMesh {
    std::vector<f32> pos;
    std::vector<u32> idx;
    sb::SimMeshInput view() const {
        sb::SimMeshInput in;
        in.positions = pos.data();
        in.indices = idx.data();
        in.vertexCount = static_cast<u32>(pos.size() / 3);
        in.indexCount = static_cast<u32>(idx.size());
        return in;
    }
};

// n x n quads of `spacing` in the XY plane at height z(x, y).
template <class H>
static RenderMesh sheet(u32 n, f32 spacing, H height) {
    RenderMesh m;
    for (u32 y = 0; y <= n; ++y)
        for (u32 x = 0; x <= n; ++x) {
            const f32 fx = static_cast<f32>(x) * spacing, fy = static_cast<f32>(y) * spacing;
            m.pos.insert(m.pos.end(), {fx, fy, height(fx, fy)});
        }
    for (u32 y = 0; y < n; ++y)
        for (u32 x = 0; x < n; ++x) {
            const u32 a = y * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
            for (const u32 i : {a, b, d, a, d, c}) m.idx.push_back(i);
        }
    return m;
}

struct Proxy {
    sb::Cage cage;
    std::vector<Vec3> local;
};

template <class H>
static Proxy makeProxy(u32 n, f32 spacing, H height) {
    Proxy p;
    sb::GridSpec g;
    g.cols = n + 1; g.rows = n + 1;
    g.du = Vec3{spacing, 0, 0}; g.dv = Vec3{0, spacing, 0};
    g.material = sb::addMaterial(p.cage, sb::Material{});
    sb::addGrid(p.cage, g);
    for (sb::Particle& q : p.cage.particles) q.pos.z = q.rest.z = height(q.pos.x, q.pos.y);
    sb::build(p.cage);
    for (const sb::Particle& q : p.cage.particles) p.local.push_back(q.pos);
    return p;
}

static Vec3 vertexAt(const RenderMesh& m, u32 v) { return Vec3{m.pos[v * 3], m.pos[v * 3 + 1], m.pos[v * 3 + 2]}; }

int main() {
    AVER_INFO("ProxyBindTest");
    const auto flat = [](f32, f32) { return 0.0f; };

    AVER_INFO("a flat 30 x 30 sheet on a 6 x 6 proxy");
    {
        const RenderMesh m = sheet(30, 1.0f, flat);
        const Proxy p = makeProxy(6, 5.0f, flat);
        sb::ProxyBind bind;
        std::string why;
        check(sb::bindRenderToProxy(m.view(), p.cage, p.local, 0.0f, bind, &why), "binds (" + why + ")");
        check(bind.homeTri.size() == 31 * 31 && bind.bary.size() == 31 * 31 * 3 && bind.triTri.size() == 30 * 30 * 2, "one entry per render vertex and triangle");
        check(bind.maxDistanceCm > 1.0f, "the default bound is 5% of the diagonal");
        f32 worst = 0.0f;
        for (u32 v = 0; v < 31 * 31; ++v) worst = std::max(worst, dist(sb::proxyEvaluate(bind, v, bind.homeTri[v], p.cage, nullptr), vertexAt(m, v)));
        check(worst < 0.01f, "every rest vertex is reproduced within 0.01 cm");

        std::vector<Vec3> smooth;
        sb::smoothParticleNormals(p.cage, smooth);
        f32 worstSmooth = 0.0f;
        for (u32 v = 0; v < 31 * 31; ++v) worstSmooth = std::max(worstSmooth, dist(sb::proxyEvaluate(bind, v, bind.homeTri[v], p.cage, smooth.data()), vertexAt(m, v)));
        check(worstSmooth < 0.01f, "and with the smooth normals");

        bool inside = true;
        for (u32 r = 0; r < bind.triTri.size(); ++r) {
            Vec3 c{0, 0, 0};
            for (u32 k = 0; k < 3; ++k) c += vertexAt(m, m.idx[r * 3 + k]);
            c = c / 3.0f;
            const sb::Triangle& t = p.cage.triangles[bind.triTri[r]];
            const Vec3 &a = p.local[t.v0[0]], &b = p.local[t.v0[1]], &d = p.local[t.v0[2]];
            const Vec3 n = cross(b - a, d - a);
            const f32 nn = dot(n, n);
            const f32 wa = dot(cross(d - b, c - b), n) / nn, wb = dot(cross(a - d, c - d), n) / nn, wc = 1.0f - wa - wb;
            inside = inside && wa > -1e-3f && wb > -1e-3f && wc > -1e-3f;
        }
        check(inside, "every render triangle follows the proxy triangle that contains its centroid");

        // Moving the proxy moves the vertices linearly.
        sb::Cage moved = p.cage;
        for (sb::Particle& q : moved.particles) q.pos += Vec3{5, -3, 2};
        bool shifted = true;
        for (u32 v = 0; v < 31 * 31; v += 17)
            shifted = shifted && dist(sb::proxyEvaluate(bind, v, bind.homeTri[v], moved, nullptr), vertexAt(m, v) + Vec3{5, -3, 2}) < 0.01f;
        check(shifted, "a translated proxy translates the render vertices");
        sb::Cage stretched = p.cage;
        for (sb::Particle& q : stretched.particles) q.pos.x *= 2.0f;
        bool stretchedOk = true;
        for (u32 v = 0; v < 31 * 31; v += 13) {
            const Vec3 want = vertexAt(m, v);
            stretchedOk = stretchedOk && dist(sb::proxyEvaluate(bind, v, bind.homeTri[v], stretched, nullptr), Vec3{want.x * 2.0f, want.y, want.z}) < 0.01f;
        }
        check(stretchedOk, "and a stretched proxy stretches them (barycentric coordinates are linear)");

        // Bit identical twice.
        sb::ProxyBind again;
        sb::bindRenderToProxy(m.view(), p.cage, p.local, 0.0f, again, nullptr);
        check(again.homeTri == bind.homeTri && again.triTri == bind.triTri && again.bary.size() == bind.bary.size() &&
              std::memcmp(again.bary.data(), bind.bary.data(), bind.bary.size() * sizeof(f32)) == 0 &&
              std::memcmp(again.offset.data(), bind.offset.data(), bind.offset.size() * sizeof(f32)) == 0, "the bind is bit identical twice");
    }

    AVER_INFO("signed offsets");
    {
        RenderMesh m = sheet(6, 5.0f, flat);
        const Proxy p = makeProxy(2, 15.0f, flat);
        const u32 up = 3 * 7 + 3, down = 4 * 7 + 4;   // two interior vertices
        m.pos[up * 3 + 2] = 2.0f;
        m.pos[down * 3 + 2] = -1.5f;
        sb::ProxyBind bind;
        check(sb::bindRenderToProxy(m.view(), p.cage, p.local, 10.0f, bind), "binds");
        check(std::fabs(bind.offset[up] - 2.0f) < 1e-3f && std::fabs(bind.offset[down] + 1.5f) < 1e-3f, "above the front face is positive, below is negative");
        check(std::fabs(bind.offset[0]) < 1e-3f, "a vertex on the surface has no offset");
        sb::Cage lifted = p.cage;
        for (sb::Particle& q : lifted.particles) q.pos.z += 10.0f;
        check(std::fabs(sb::proxyEvaluate(bind, up, bind.homeTri[up], lifted, nullptr).z - 12.0f) < 1e-2f, "the offset rides the proxy normal");
        sb::ProxyBind scaled = bind;
        scaled.offsetScale = 2.0f;
        check(std::fabs(sb::proxyEvaluate(scaled, up, scaled.homeTri[up], p.cage, nullptr).z - 4.0f) < 1e-3f, "the offset is scaled by the entity's scale");
    }

    AVER_INFO("a curved proxy");
    {
        const auto bump = [](f32 x, f32 y) { return 6.0f * std::exp(-((x - 15.0f) * (x - 15.0f) + (y - 15.0f) * (y - 15.0f)) / 80.0f); };
        const RenderMesh m = sheet(30, 1.0f, bump);
        const Proxy p = makeProxy(6, 5.0f, bump);
        sb::ProxyBind bind;
        std::string why;
        check(sb::bindRenderToProxy(m.view(), p.cage, p.local, 0.0f, bind, &why), "binds (" + why + ")");
        std::vector<Vec3> smooth;
        sb::smoothParticleNormals(p.cage, smooth);
        f32 worst = 0.0f;
        for (u32 v = 0; v < 31 * 31; ++v) worst = std::max(worst, dist(sb::proxyEvaluate(bind, v, bind.homeTri[v], p.cage, smooth.data()), vertexAt(m, v)));
        AVER_INFO("  worst reproduction error on the bump {:.4f} cm", worst);
        check(worst < 0.6f, "reproduced within the simplification error");
        bool signsOk = true;
        for (u32 v = 0; v < 31 * 31; ++v) signsOk = signsOk && std::fabs(bind.offset[v]) <= 1.0f;
        check(signsOk, "offsets stay small where the proxy follows the surface");
    }

    AVER_INFO("evaluating against an adjacent triangle");
    {
        RenderMesh m = sheet(4, 5.0f, flat);
        Proxy p = makeProxy(4, 5.0f, flat);
        sb::ProxyBind bind;
        check(sb::bindRenderToProxy(m.view(), p.cage, p.local, 3.0f, bind), "binds");
        // Render vertex 12 sits on proxy particle 12 at (10, 10).
        const u32 v = 12;
        u32 lowest = sb::kNone, other = sb::kNone;
        for (u32 t = 0; t < p.cage.triangles.size(); ++t) {
            const sb::Triangle& tri = p.cage.triangles[t];
            if (tri.v0[0] != 12 && tri.v0[1] != 12 && tri.v0[2] != 12) continue;
            if (lowest == sb::kNone) lowest = t;
            else if (other == sb::kNone) other = t;
        }
        check(bind.homeTri[v] == lowest, "a tie goes to the lower triangle id");
        p.cage.particles[12].pos += Vec3{0, 0, 3};
        const Vec3 a = sb::proxyEvaluate(bind, v, lowest, p.cage, nullptr);
        const Vec3 b = sb::proxyEvaluate(bind, v, other, p.cage, nullptr);
        check(dist(a, Vec3{10, 10, 3}) < 1e-3f && dist(b, a) < 1e-3f, "the same position from either triangle on a shared corner (continuous)");
    }

    AVER_INFO("a vertex too far from the proxy");
    {
        RenderMesh m = sheet(4, 5.0f, flat);
        m.pos[7 * 3 + 2] = 100.0f;
        const Proxy p = makeProxy(2, 10.0f, flat);
        sb::ProxyBind bind;
        std::string why;
        check(!sb::bindRenderToProxy(m.view(), p.cage, p.local, 5.0f, bind, &why), "the bind fails");
        check(why.find("render vertex 7 ") != std::string::npos, "and names the vertex: " + why);
    }

    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
