// Baking a polygon navigation mesh from triangles: floors, walls, headroom, ramps, stacked floors, obstacle carving and the
// parameters, each against geometry whose answer is worked out by hand. The baked mesh is then loaded into a NavServer to prove
// it routes.
#include "aver/core/Log.hpp"
#include "aver/synapse/NavMesh.hpp"
#include "aver/synapse/NavMeshBake.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::synapse;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}
static bool near(f32 a, f32 b, f32 eps = 0.5f) { return std::fabs(a - b) <= eps; }

struct Soup {
    std::vector<Vec3> v;
    std::vector<u32> t;
    // A quad a-b-c-d (counter-clockwise seen from above for a floor); two triangles, normals by cross(b-a, c-a).
    void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
        const u32 i = static_cast<u32>(v.size());
        v.push_back(a); v.push_back(b); v.push_back(c); v.push_back(d);
        t.insert(t.end(), {i, i + 1, i + 2, i, i + 2, i + 3});
    }
    void floor(f32 x0, f32 y0, f32 x1, f32 y1, f32 z) { quad({x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}); }
    // A vertical wall along (x0,y0)-(x1,y1) from z0 to z1 (facing either way: walls are never walkable).
    void wall(f32 x0, f32 y0, f32 x1, f32 y1, f32 z0, f32 z1) { quad({x0, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y0, z1}); }
};

static f32 area(const NavBakeResult& r) {
    f32 a = 0;
    for (const auto& p : r.polys) {
        std::vector<Vec3> pts;
        for (u32 i : p) pts.push_back(r.verts[i]);
        a += std::fabs(navSignedAreaXY(pts));
    }
    return a;
}

static NavBakeParams baseParams() {
    NavBakeParams p;
    p.cellSizeCm = 25; p.cellHeightCm = 25;
    p.agentRadiusCm = 50; p.agentHeightCm = 150; p.agentMaxClimbCm = 25; p.agentMaxSlopeDeg = 45;
    p.regionMinSize = 2;
    return p;
}

static NavBakeResult bake(const Soup& s, const NavBakeParams& p, const std::vector<NavBakeObstacle>& o = {}, bool expect = true) {
    NavBakeResult r;
    std::string why;
    const bool ok = navBake(s.v, s.t, o, p, r, &why);
    if (expect && !ok) check(false, "bake failed: " + why);
    return r;
}

static u32 load(NavServer& srv, u32 map, const NavBakeResult& r) {
    const u32 reg = srv.createRegion(map);
    srv.setRegionMesh(reg, r.verts, r.polys);
    return reg;
}

static NavPathQuery query(u32 map, Vec3 a, Vec3 b) {
    NavPathQuery q;
    q.map = map; q.start = a; q.target = b;
    return q;
}

static bool anyPolyVertex(const NavBakeResult& r, bool (*pred)(const Vec3&)) {
    for (const Vec3& v : r.verts) if (pred(v)) return true;
    return false;
}

int main() {
    AVER_INFO("NavMeshBakeTest");

    AVER_INFO("a flat floor is eroded by the agent radius and covered by one rectangle");
    {
        Soup s;
        s.floor(0, 0, 1000, 1000, 0);
        const NavBakeResult r = bake(s, baseParams());
        check(r.polys.size() == 1 && r.verts.size() == 4, "one rectangle");
        check(near(area(r), 900.0f * 900.0f, 100.0f), "900 x 900 cm (1000 minus the 50 cm radius on each side)");
        Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        for (const Vec3& v : r.verts) { lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), 0}; hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), 0}; }
        check(near(lo.x, 50) && near(lo.y, 50) && near(hi.x, 950) && near(hi.y, 950), "from 50 to 950");
        for (const Vec3& v : r.verts) if (!near(v.z, 0)) check(false, "all heights are the floor's");
        check(navSignedAreaXY({r.verts[r.polys[0][0]], r.verts[r.polys[0][1]], r.verts[r.polys[0][2]], r.verts[r.polys[0][3]]}) > 0, "counter-clockwise");

        NavServer srv;
        const u32 m = srv.createMap();
        load(srv, m, r);
        const NavPathResult p = srv.queryPath(query(m, {100, 100, 0}, {900, 900, 0}));
        check(p.reachedTarget && near(p.lengthCm, std::sqrt(2.0f) * 800, 1), "the baked mesh routes");
    }

    AVER_INFO("a wall splits the floor in two");
    {
        Soup s;
        s.floor(0, 0, 2000, 1000, 0);
        s.wall(1000, -10, 1000, 1010, 0, 300);
        const NavBakeResult r = bake(s, baseParams());
        NavServer srv;
        const u32 m = srv.createMap();
        load(srv, m, r);
        check(!srv.queryPath(query(m, {300, 500, 0}, {1700, 500, 0})).reachedTarget, "no route through the wall");
        check(srv.queryPath(query(m, {300, 300, 0}, {300, 800, 0})).reachedTarget, "each side is walkable");
        f32 closest = 1e9f;
        for (const Vec3& v : r.verts) closest = std::min(closest, std::fabs(v.x - 1000.0f));
        check(closest >= 45.0f && closest <= 80.0f, "the mesh stops a radius or more from the wall (" + std::to_string(closest) + ")");
        check(r.stats.regions == 2, "two floors");
    }

    AVER_INFO("a gap in the wall is a doorway only if the agent fits");
    {
        Soup s;
        s.floor(0, 0, 2000, 1000, 0);
        s.wall(1000, -10, 1000, 380, 0, 300);   // door from y 380 to 620 (240 cm wide), wall again above
        s.wall(1000, 620, 1000, 1010, 0, 300);
        NavBakeParams p = baseParams();
        NavBakeResult r = bake(s, p);
        NavServer srv;
        u32 m = srv.createMap();
        load(srv, m, r);
        check(srv.queryPath(query(m, {300, 500, 0}, {1700, 500, 0})).reachedTarget, "a 240 cm door lets a 100 cm agent through");
        p.agentRadiusCm = 150;
        r = bake(s, p);
        NavServer srv2;
        m = srv2.createMap();
        load(srv2, m, r);
        check(!srv2.queryPath(query(m, {300, 500, 0}, {1700, 500, 0})).reachedTarget, "a 300 cm wide agent does not fit");
    }

    AVER_INFO("headroom: the floor under a low platform is not walkable");
    {
        Soup s;
        s.floor(0, 0, 1500, 1000, 0);
        s.floor(700, 0, 1500, 1000, 100);   // a platform 100 cm above the floor (a plate: its underside is a ceiling)
        s.quad({700, 0, 100}, {700, 1000, 100}, {1500, 1000, 100}, {1500, 0, 100});   // its underside, facing down
        const NavBakeResult r = bake(s, baseParams());
        check(!anyPolyVertex(r, [](const Vec3& v) { return v.x > 780 && v.z < 50; }), "no floor polygon under the platform");
        check(anyPolyVertex(r, [](const Vec3& v) { return v.x > 780 && std::fabs(v.z - 100) < 1; }), "the platform top is baked");
        check(anyPolyVertex(r, [](const Vec3& v) { return v.x < 600 && std::fabs(v.z) < 1; }), "the open floor is");
    }

    AVER_INFO("a ramp of 30 degrees joins two levels; a 60 degree face does not");
    {
        Soup s;
        s.floor(0, 0, 1000, 1000, 0);
        s.quad({1000, 0, 0}, {1520, 0, 300}, {1520, 1000, 300}, {1000, 1000, 0});   // up the +X side, 30 degrees
        s.floor(1520, 0, 2520, 1000, 300);
        const NavBakeResult r = bake(s, baseParams());
        NavServer srv;
        const u32 m = srv.createMap();
        load(srv, m, r);
        const NavPathResult p = srv.queryPath(query(m, {300, 500, 0}, {2200, 500, 300}));
        check(p.reachedTarget, "up the ramp");
        check(!p.points.empty() && near(p.points.back().z, 300, 30.0f) && near(p.points.front().z, 0, 25.0f),
              "ends at the right heights (" + std::to_string(p.points.empty() ? -1.0f : p.points.front().z) + " to " + std::to_string(p.points.empty() ? -1.0f : p.points.back().z) + ")");
        check(anyPolyVertex(r, [](const Vec3& v) { return v.x > 1000 && v.x < 1520 && v.z > 10 && v.z < 290; }), "the ramp's vertices are at intermediate heights");
        const NavClosest mid = srv.closestPoint(m, {1260, 500, 100});
        check(mid.found && mid.point.z > 100 && mid.point.z < 200, "a point over the middle of the ramp lands at ramp height (" + std::to_string(mid.point.z) + ")");

        Soup steep;
        steep.floor(0, 0, 1000, 1000, 0);
        steep.quad({1000, 0, 0}, {1173, 0, 300}, {1173, 1000, 300}, {1000, 1000, 0});   // 60 degrees
        steep.floor(1173, 0, 2173, 1000, 300);
        const NavBakeResult r2 = bake(steep, baseParams());
        NavServer srv2;
        const u32 m2 = srv2.createMap();
        load(srv2, m2, r2);
        check(!srv2.queryPath(query(m2, {300, 500, 0}, {1900, 500, 300})).reachedTarget, "too steep: no route");
        NavBakeParams steepOk = baseParams();
        steepOk.agentMaxSlopeDeg = 70;
        steepOk.agentMaxClimbCm = 25;
        const NavBakeResult r3 = bake(steep, steepOk);
        check(area(r3) > 0, "a larger maximum slope still bakes");
    }

    AVER_INFO("stacked floors stay separate; a link joins them");
    {
        Soup s;
        s.floor(0, 0, 1500, 1000, 0);
        s.floor(0, 0, 1500, 1000, 400);
        s.quad({0, 0, 400}, {0, 1000, 400}, {1500, 1000, 400}, {1500, 0, 400});   // the upper floor's underside
        const NavBakeResult r = bake(s, baseParams());
        check(anyPolyVertex(r, [](const Vec3& v) { return std::fabs(v.z) < 1; }) && anyPolyVertex(r, [](const Vec3& v) { return std::fabs(v.z - 400) < 1; }), "both floors are baked");
        NavServer srv;
        const u32 m = srv.createMap();
        load(srv, m, r);
        NavPathResult p = srv.queryPath(query(m, {500, 500, 0}, {500, 500, 400}));
        check(!p.reachedTarget && near(p.points.back().z, 0, 1), "no route between the floors");
        const u32 l = srv.createLink(m);
        srv.link(l)->start = {1300, 500, 0};
        srv.link(l)->end = {1300, 500, 400};
        p = srv.queryPath(query(m, {500, 500, 0}, {500, 500, 400}));
        check(p.reachedTarget, "with a link (a stair) they connect");
        bool top = false;
        for (usize i = 0; i < p.points.size(); ++i) if (near(p.points[i].z, 400, 1)) top = true;
        check(top, "the path arrives on the upper floor");
    }

    AVER_INFO("obstacles carve the mesh");
    {
        Soup s;
        s.floor(0, 0, 2000, 1000, 0);
        NavBakeObstacle circle;
        circle.position = {1000, 500, 0}; circle.radius = 100; circle.height = 200; circle.zMin = -10;
        NavBakeResult r = bake(s, baseParams(), {circle});
        NavServer srv;
        u32 m = srv.createMap();
        load(srv, m, r);
        const NavClosest c = srv.closestPoint(m, {1000, 500, 0});
        check(c.found && std::hypot(c.point.x - 1000, c.point.y - 500) >= 140.0f, "nothing within the obstacle plus the radius (" + std::to_string(std::hypot(c.point.x - 1000, c.point.y - 500)) + ")");
        check(srv.queryPath(query(m, {300, 500, 0}, {1700, 500, 0})).reachedTarget, "the way round is open");

        NavBakeObstacle wall;   // a slab that blocks the whole width
        wall.verts = {{950, -100, 0}, {1050, -100, 0}, {1050, 1100, 0}, {950, 1100, 0}};
        wall.zMin = -10; wall.height = 200;
        r = bake(s, baseParams(), {wall});
        NavServer srv2;
        m = srv2.createMap();
        load(srv2, m, r);
        check(!srv2.queryPath(query(m, {300, 500, 0}, {1700, 500, 0})).reachedTarget, "a polygon obstacle across the floor splits it");
        NavBakeObstacle above;   // far above: no effect
        above.position = {1000, 500, 0}; above.radius = 300; above.zMin = 500; above.height = 100;
        r = bake(s, baseParams(), {above});
        check(near(area(r), 1900.0f * 900.0f, 4000.0f), "an obstacle at another height leaves the floor alone");
    }

    AVER_INFO("parameters");
    {
        Soup s;
        s.floor(0, 0, 1000, 1000, 0);
        NavBakeParams p = baseParams();
        p.verticesPerPolygon = 3;
        NavBakeResult r = bake(s, p);
        bool allTri = !r.polys.empty();
        for (const auto& poly : r.polys) if (poly.size() != 3) allTri = false;
        check(allTri && r.polys.size() == 2 && near(area(r), 810000.0f, 100.0f), "vertices_per_polygon 3: two triangles, same area");

        p = baseParams();
        p.edgeMaxLengthCm = 300;
        r = bake(s, p);
        bool short_ = true;
        for (const auto& poly : r.polys)
            for (usize i = 0; i < poly.size(); ++i) {
                const Vec3 &a = r.verts[poly[i]], &b = r.verts[poly[(i + 1) % poly.size()]];
                if (std::hypot(b.x - a.x, b.y - a.y) > 300.01f) short_ = false;
            }
        check(r.polys.size() > 1 && short_ && near(area(r), 810000.0f, 100.0f), "edge_max_length splits the cover");

        p = baseParams();
        p.agentRadiusCm = 0;
        p.regionMinSize = 5;
        Soup islands;
        islands.floor(0, 0, 1000, 1000, 0);
        islands.floor(2000, 0, 2075, 75, 0);   // 3 x 3 cells
        r = bake(islands, p);
        check(r.stats.regions == 1 && r.stats.regionsRemoved == 1, "region_min_size removes the 3 x 3 island");
        p.regionMinSize = 2;
        r = bake(islands, p);
        check(r.stats.regions == 2, "and keeps it when the minimum is smaller");

        p = baseParams();
        p.useBounds = true;
        p.boundsMin = {0, 0, -100}; p.boundsMax = {500, 1000, 100};
        Soup wide;
        wide.floor(0, 0, 2000, 1000, 0);
        r = bake(wide, p);
        check(!anyPolyVertex(r, [](const Vec3& v) { return v.x > 510; }) && area(r) > 0, "the bounds clip the bake");

        p = baseParams();
        p.agentRadiusCm = 0;
        p.cellSizeCm = 50;
        r = bake(s, p);
        check(near(area(r), 1000.0f * 1000.0f, 100.0f) || near(area(r), 1000.0f * 1000.0f, 50.0f * 2000.0f), "cell size 50");
        Soup bad;
        NavBakeResult br;
        std::string why;
        check(!navBake(bad.v, bad.t, {}, baseParams(), br, &why) && !why.empty(), "no triangles is refused with a reason");
        bad.v = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        bad.t = {0, 1, 7};
        check(!navBake(bad.v, bad.t, {}, baseParams(), br, &why), "an index out of range is refused");
        NavBakeParams huge = baseParams();
        huge.cellSizeCm = 0.001f;
        Soup tiny;
        tiny.floor(0, 0, 100000, 100000, 0);
        check(!navBake(tiny.v, tiny.t, {}, huge, br, &why), "a grid beyond the cell cap is refused");
    }

    AVER_INFO("low hanging obstacles, ledges and low spans (filters)");
    {
        Soup s;
        s.floor(0, 0, 1000, 1000, 0);
        // a 20 cm kerb strip across the floor (walls and a top face)
        s.quad({400, 0, 20}, {500, 0, 20}, {500, 1000, 20}, {400, 1000, 20});
        NavBakeParams p = baseParams();
        p.agentRadiusCm = 0;
        p.filterLowHangingObstacles = true;
        const NavBakeResult r = bake(s, p);
        NavServer srv;
        const u32 m = srv.createMap();
        load(srv, m, r);
        check(srv.queryPath(query(m, {200, 500, 0}, {800, 500, 0})).reachedTarget, "a kerb under the climb height is stepped over");

        Soup ledge;
        ledge.floor(0, 0, 1000, 1000, 0);
        ledge.floor(1000, 0, 2000, 1000, -400);   // a 4 m drop
        NavBakeParams lp = baseParams();
        lp.agentRadiusCm = 0;
        lp.filterLedgeSpans = true;
        const NavBakeResult lr = bake(ledge, lp);
        auto upperMaxX = [](const NavBakeResult& b) {
            f32 mx = -1e9f;
            for (const Vec3& v : b.verts) if (v.z > -1) mx = std::max(mx, v.x);
            return mx;
        };
        check(near(upperMaxX(lr), 975.0f), "the ledge filter takes the cell at the drop away (edge at " + std::to_string(upperMaxX(lr)) + ")");
        lp.filterLedgeSpans = false;
        const NavBakeResult lr2 = bake(ledge, lp);
        check(near(upperMaxX(lr2), 1000.0f), "and the floor runs to the edge without it");
    }

    AVER_INFO(g_failures == 0 ? "NavMeshBakeTest passed" : "NavMeshBakeTest FAILED");
    return g_failures == 0 ? 0 : 1;
}
