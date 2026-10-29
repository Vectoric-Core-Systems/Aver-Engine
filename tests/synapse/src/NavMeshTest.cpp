// Polygon navigation maps: path queries across regions, floors, links, layers and costs, closest points, and avoidance.
// Every map is a few rectangles whose answer can be worked out by hand.
#include "aver/core/Log.hpp"
#include "aver/synapse/NavMesh.hpp"

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
static bool near3(const Vec3& a, const Vec3& b, f32 eps = 0.5f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps); }

// A region holding one rectangle [x0,x1] x [y0,y1] at height z.
static u32 rect(NavServer& s, u32 map, f32 x0, f32 y0, f32 x1, f32 y1, f32 z = 0.0f, u32 layers = 1) {
    const u32 r = s.createRegion(map);
    s.setRegionMesh(r, {{x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}}, {{0, 1, 2, 3}});
    s.region(r)->layers = layers;
    return r;
}

static bool arrives(const NavPathResult& p) { return p.reachedTarget && p.endGapCm < 5.0f; }

static NavPathQuery query(u32 map, Vec3 a, Vec3 b, u32 layers = 1) {
    NavPathQuery q;
    q.map = map; q.start = a; q.target = b; q.layers = layers;
    return q;
}

int main() {
    AVER_INFO("NavMeshTest");

    AVER_INFO("one region: a straight line");
    {
        NavServer s;
        const u32 m = s.createMap();
        rect(s, m, 0, 0, 1000, 1000);
        const NavPathResult r = s.queryPath(query(m, {100, 100, 0}, {900, 900, 0}));
        check(r.points.size() == 2, "two points");
        check(r.reachedTarget, "the target is reached");
        check(near(r.lengthCm, std::sqrt(2.0f) * 800.0f), "length is the straight distance");
        check(near3(r.points.front(), {100, 100, 0}) && near3(r.points.back(), {900, 900, 0}), "endpoints");
        const NavPathResult off = s.queryPath(query(m, {-500, 100, 0}, {900, 900, 0}));
        check(off.points.size() == 2 && near3(off.points.front(), {0, 100, 0}), "an off-mesh start snaps to the mesh edge");
    }

    AVER_INFO("an L corridor bends at the inner corner (funnel)");
    {
        NavServer s;
        const u32 m = s.createMap();
        const u32 r = s.createRegion(m);
        s.setRegionMesh(r, {{0, 0, 0}, {1000, 0, 0}, {1000, 200, 0}, {0, 200, 0}, {800, 200, 0}, {800, 1000, 0}, {1000, 1000, 0}},
                        {{0, 1, 2, 3}, {4, 2, 6, 5}});
        s.region(r)->owner = 77;
        NavPathResult p = s.queryPath(query(m, {100, 100, 0}, {900, 900, 0}));
        check(p.points.size() == 3, "start, corner, goal");
        check(p.points.size() == 3 && near3(p.points[1], {800, 200, 0}), "the corner is the inner vertex");
        check(near(p.lengthCm, std::sqrt(700.0f * 700 + 100 * 100) + std::sqrt(100.0f * 100 + 700 * 700)), "length through the corner");
        check(p.owners.size() == 3 && p.owners[0] == 77 && p.rids[0] == r && p.types[0] == kNavPointRegion, "region, owner and type per point");

        NavPathQuery q = query(m, {100, 100, 0}, {900, 900, 0});
        q.postprocess = kNavPostEdgeCentered;
        p = s.queryPath(q);
        check(p.points.size() == 3 && near3(p.points[1], {900, 200, 0}), "edge-centred: the portal midpoint");
        q.postprocess = kNavPostNone;
        p = s.queryPath(q);
        check(p.points.size() == 3, "none: the raw corridor");
    }

    AVER_INFO("two regions join by overlapping edges; a gap beyond the margin does not");
    {
        NavServer s;
        const u32 m = s.createMap();
        rect(s, m, 0, 0, 1000, 1000);
        const u32 b = rect(s, m, 1010, 0, 2000, 1000);      // 10 cm apart: inside the 25 cm margin
        NavPathResult p = s.queryPath(query(m, {500, 500, 0}, {1500, 500, 0}));
        check(p.reachedTarget && near(p.lengthCm, 1000.0f), "joined across a 10 cm seam");
        check(!s.regionConnections(b).empty(), "region_get_connections sees the seam");
        s.mapParams(m)->edgeConnectionMarginCm = 5.0f;
        s.markDirty(m);
        p = s.queryPath(query(m, {500, 500, 0}, {1500, 500, 0}));
        check(!p.reachedTarget, "a 5 cm margin no longer joins them");
        s.mapParams(m)->edgeConnectionMarginCm = 25.0f;
        s.mapParams(m)->useEdgeConnections = false;
        s.markDirty(m);
        check(!s.queryPath(query(m, {500, 500, 0}, {1500, 500, 0})).reachedTarget, "edge connections off");
    }

    AVER_INFO("floors over the same ground plan stay apart; a ramp joins levels");
    {
        NavServer s;
        const u32 m = s.createMap();
        rect(s, m, 0, 0, 1000, 1000, 0);
        const u32 mezzanine = rect(s, m, 0, 0, 1000, 1000, 300);       // over floor 0, not joined to it
        rect(s, m, 1300, 0, 2300, 1000, 300);                          // the upper level in the east
        // the ramp: rises from z 0 at x 1000 to z 300 at x 1300
        const u32 ramp = s.createRegion(m);
        s.setRegionMesh(ramp, {{1000, 0, 0}, {1300, 0, 300}, {1300, 1000, 300}, {1000, 1000, 0}}, {{0, 1, 2, 3}});
        NavPathQuery q = query(m, {500, 500, 0}, {2000, 500, 300});
        NavPathResult p = s.queryPath(q);
        check(p.reachedTarget && p.endGapCm < 1.0f, "up the ramp to the upper level");
        check(near(p.points.front().z, 0) && near(p.points.back().z, 300), "starts on the lower level, ends on the upper");
        check(p.points.size() == 2 && near(p.lengthCm, std::sqrt(1500.0f * 1500 + 300.0f * 300), 1.0f), "the funnel keeps the straight line");
        q.postprocess = kNavPostEdgeCentered;
        p = s.queryPath(q);
        check(p.points.size() == 4 && near3(p.points[1], {1000, 500, 0}) && near3(p.points[2], {1300, 500, 300}), "edge-centred points sit on the ramp's two edges, at its heights");
        const NavClosest low = s.closestPoint(m, {500, 500, 20});
        const NavClosest high = s.closestPoint(m, {500, 500, 290});
        check(low.found && near(low.point.z, 0) && high.found && near(high.point.z, 300) && high.region == mezzanine, "closest point picks the floor by height");
        p = s.queryPath(query(m, {500, 500, 0}, {500, 500, 300}));
        check(!p.reachedTarget && near(p.points.back().z, 0) && p.endGapCm > 299.0f, "the floor above is not joined: the path ends at the closest reachable spot");
        s.destroy(ramp);
        p = s.queryPath(query(m, {500, 500, 0}, {2000, 500, 300}));
        check(!p.reachedTarget, "without the ramp the upper level is cut off");
    }

    AVER_INFO("layers filter regions; costs steer the route");
    {
        NavServer s;
        const u32 m = s.createMap();
        rect(s, m, 0, 0, 1000, 1000, 0, 1);
        const u32 gate = rect(s, m, 1000, 0, 2000, 1000, 0, 2);
        check(!arrives(s.queryPath(query(m, {500, 500, 0}, {1500, 500, 0}, 1))), "layer 1 cannot enter a layer-2 region");
        check(arrives(s.queryPath(query(m, {500, 500, 0}, {1500, 500, 0}, 3))), "layers 1|2 can");
        s.region(gate)->enabled = false;
        s.markDirty(m);
        check(!arrives(s.queryPath(query(m, {500, 500, 0}, {1500, 500, 0}, 3))), "a disabled region is gone");
    }
    {
        // Two routes from the west to the east block: a short one through the swamp (cost 10 per cm) and a long one around it.
        NavServer s;
        const u32 m = s.createMap();
        rect(s, m, 0, 0, 1000, 1000);                         // west
        const u32 swamp = rect(s, m, 1000, 400, 2000, 600);   // middle, short
        rect(s, m, 1000, 600, 2000, 1000);                    // north road (also reaches east)
        rect(s, m, 2000, 0, 3000, 1000);                      // east
        rect(s, m, 1000, 0, 2000, 400);                       // south road
        NavPathQuery q = query(m, {500, 500, 0}, {2500, 500, 0});
        NavPathResult cheap = s.queryPath(q);
        check(cheap.reachedTarget && near(cheap.lengthCm, 2000.0f), "all cost 1: straight through");
        s.region(swamp)->travelCost = 10.0f;
        s.markDirty(m);
        NavPathResult dear = s.queryPath(q);
        bool throughSwamp = false;
        for (const Vec3& v : dear.points) if (v.x > 1000 && v.x < 2000 && v.y > 400 && v.y < 600) throughSwamp = true;
        check(dear.reachedTarget && !throughSwamp && dear.lengthCm > 2000.0f, "a swamp at travel cost 10 is walked around");
        s.region(swamp)->travelCost = 1.0f;
        s.region(swamp)->enterCost = 100000.0f;
        s.markDirty(m);
        dear = s.queryPath(q);
        throughSwamp = false;
        for (const Vec3& v : dear.points) if (v.x > 1000 && v.x < 2000 && v.y > 400 && v.y < 600) throughSwamp = true;
        check(dear.reachedTarget && !throughSwamp, "an enter cost keeps the path out");
        NavPathQuery ex = q;
        ex.excludedRegions = {swamp};
        s.region(swamp)->enterCost = 0;
        s.markDirty(m);
        const NavPathResult exr = s.queryPath(ex);
        throughSwamp = false;
        for (const Vec3& v : exr.points) if (v.x > 1000 && v.x < 2000 && v.y > 400 && v.y < 600) throughSwamp = true;
        check(exr.reachedTarget && !throughSwamp, "excluded regions are skipped");
        NavPathQuery inc = q;
        inc.includedRegions = {swamp};
        check(s.queryPath(inc).points.empty() || !arrives(s.queryPath(inc)), "only the swamp: the ends are off it, no route");
    }

    AVER_INFO("links");
    {
        NavServer s;
        const u32 m = s.createMap();
        rect(s, m, 0, 0, 1000, 1000, 0);
        rect(s, m, 3000, 0, 4000, 1000, 0);                       // an island to the east, no seam
        const u32 l = s.createLink(m);
        s.link(l)->start = {900, 500, 0};
        s.link(l)->end = {3100, 500, 0};
        s.link(l)->owner = 5;
        NavPathQuery q = query(m, {100, 500, 0}, {3900, 500, 0});
        NavPathResult p = s.queryPath(q);
        check(p.reachedTarget, "the link joins the island");
        int linkPts = 0;
        for (usize i = 0; i < p.points.size(); ++i) if (p.types[i] == kNavPointLink) { ++linkPts; check(p.rids[i] == l && p.owners[i] == 5, "link points carry the link id and owner"); }
        check(linkPts == 2, "a link adds its start and its end");
        check(near3(p.points[1], {900, 500, 0}) && near3(p.points[2], {3100, 500, 0}), "at the link's own positions");
        check(near(p.lengthCm, 3800.0f), "length");
        check(s.linkConnectedPolys(l) == 2, "the link attached at both ends");
        NavPathQuery back = query(m, {3900, 500, 0}, {100, 500, 0});
        check(s.queryPath(back).reachedTarget, "bidirectional");
        s.link(l)->bidirectional = false;
        s.markDirty(m);
        check(s.queryPath(q).reachedTarget && !s.queryPath(back).reachedTarget, "one-way");
        s.link(l)->layers = 4;
        s.markDirty(m);
        check(!s.queryPath(q).reachedTarget, "link layers are checked against the query");
        q.layers = 5;
        check(s.queryPath(q).reachedTarget, "the query layer matches the link's");
        s.link(l)->layers = 1;
        s.link(l)->end = {3100, 500, 5000};   // far from any polygon (> the 100 cm radius)
        s.markDirty(m);
        check(s.linkConnectedPolys(l) == 1 && !s.queryPath(query(m, {100, 500, 0}, {3900, 500, 0})).reachedTarget, "an unattached link does nothing");
        s.link(l)->end = {3100, 500, 0};
        s.link(l)->enabled = false;
        s.markDirty(m);
        check(!s.queryPath(query(m, {100, 500, 0}, {3900, 500, 0})).reachedTarget, "a disabled link");
    }
    AVER_INFO("a link between two polygons is a shortcut only when its cost says so");
    {
        NavServer s;
        const u32 m = s.createMap();
        // A long thin chain of five rectangles so the walk is long, and a link from the first to the last.
        for (int i = 0; i < 5; ++i) rect(s, m, i * 2000.0f, 0, (i + 1) * 2000.0f, 200);
        const u32 l = s.createLink(m);
        s.link(l)->start = {1900, 100, 0};
        s.link(l)->end = {8100, 100, 0};
        s.link(l)->travelCost = 0.0f;
        NavPathQuery q = query(m, {500, 100, 0}, {9500, 100, 0});
        NavPathResult p = s.queryPath(q);
        bool usesLink = false;
        for (i32 t : p.types) if (t == kNavPointLink) usesLink = true;
        check(usesLink, "a free link is taken");
        s.link(l)->travelCost = 5.0f;
        s.markDirty(m);
        p = s.queryPath(q);
        usesLink = false;
        for (i32 t : p.types) if (t == kNavPointLink) usesLink = true;
        check(!usesLink, "an expensive link is not");
        s.link(l)->travelCost = 0.0f;
        s.link(l)->enterCost = 100000.0f;
        s.markDirty(m);
        p = s.queryPath(q);
        usesLink = false;
        for (i32 t : p.types) if (t == kNavPointLink) usesLink = true;
        check(!usesLink, "an enter cost on the link keeps it unused");
    }

    AVER_INFO("simplify, closest point, random point, maps");
    {
        NavServer s;
        const u32 m = s.createMap();
        const u32 r = s.createRegion(m);
        s.setRegionMesh(r, {{0, 0, 0}, {1000, 0, 0}, {1000, 200, 0}, {0, 200, 0}, {800, 200, 0}, {800, 1000, 0}, {1000, 1000, 0}}, {{0, 1, 2, 3}, {4, 2, 6, 5}});
        NavPathQuery q = query(m, {100, 100, 0}, {900, 900, 0});
        q.simplify = true;
        q.simplifyEpsilonCm = 500.0f;
        check(s.queryPath(q).points.size() == 2, "a large epsilon drops the corner");
        q.simplifyEpsilonCm = 1.0f;
        check(s.queryPath(q).points.size() == 3, "a small one keeps it");

        const NavClosest c = s.closestPoint(m, {2000, 100, 40});
        check(c.found && near3(c.point, {1000, 100, 0}) && c.region == r, "closest point on the mesh");
        check(near(c.normal.z, 1.0f), "normal faces up");
        Vec3 onSeg;
        const Vec3 cs = s.closestPointToSegment(m, {1500, 500, 100}, {1500, 900, 100}, &onSeg);
        check(near(cs.x, 1000, 1) && near(onSeg.x, 1500, 1), "closest point to a segment");

        bool found = false;
        const Vec3 rp = s.randomPoint(m, 1, true, 0.3f, 0.4f, 0.2f, &found);
        check(found && s.closestPoint(m, rp).point.x == rp.x, "a random point lies on the mesh");
        check(!s.randomPoint(m, 2, true, 0.3f, 0.4f, 0.2f, &found).z && !found, "no polygon on that layer");

        check(s.polygonCount(m) == 2, "polygon count");
        const u32 it = s.mapIteration(m);
        s.markDirty(m);
        s.update(m);
        check(s.mapIteration(m) == it + 1, "an update bumps the iteration");
        check(!s.update(m) && s.mapIteration(m) == it + 1, "a clean map does not rebuild");
        s.destroyMap(m);
        check(!s.hasMap(m) && s.region(r)->map == 0, "destroying a map detaches its regions");
        check(s.queryPath(q).points.empty(), "no map, no path");
    }

    AVER_INFO("avoidance");
    {
        NavServer s;
        const u32 m = s.createMap();
        auto mk = [&](Vec3 pos, Vec3 vel) {
            const u32 a = s.createAgent(m);
            NavAgentData* d = s.agent(a);
            d->avoidance = true; d->position = pos; d->velocity = vel; d->hasVelocity = true;
            d->radius = 50; d->maxSpeed = 300; d->timeHorizonAgents = 2.0f;
            return a;
        };
        {
            const u32 a = mk({0, 0, 0}, {300, 0, 0});
            s.stepAvoidance(m, 1.0f / 30.0f);
            const NavAgentData* d = s.agent(a);
            check(d->hasSafe && near3(d->safeVelocity, {300, 0, 0}, 0.01f), "alone: the safe velocity is the wanted one");
            check(!d->hasVelocity, "set_velocity is consumed");
        }
        {
            NavServer s2;
            const u32 m2 = s2.createMap();
            const u32 a = s2.createAgent(m2), b = s2.createAgent(m2);
            for (u32 id : {a, b}) {
                NavAgentData* d = s2.agent(id);
                d->avoidance = true; d->radius = 50; d->maxSpeed = 300; d->timeHorizonAgents = 2.0f; d->hasVelocity = true;
            }
            s2.agent(a)->position = {0, 0, 0};   s2.agent(a)->velocity = {300, 0, 0};
            s2.agent(b)->position = {400, 0, 0}; s2.agent(b)->velocity = {-300, 0, 0};
            s2.stepAvoidance(m2, 1.0f / 30.0f);
            const Vec3 va = s2.agent(a)->safeVelocity, vb = s2.agent(b)->safeVelocity;
            check(std::sqrt(va.x * va.x + va.y * va.y) <= 300.01f && std::sqrt(vb.x * vb.x + vb.y * vb.y) <= 300.01f, "never faster than max speed");
            check(std::fabs(va.y) > 1.0f && std::fabs(vb.y) > 1.0f, "head-on: both steer sideways");
            // Simulate to the end: they must pass without overlapping.
            f32 minGap = 1e9f;
            for (int i = 0; i < 90; ++i) {
                for (u32 id : {a, b}) s2.agent(id)->hasVelocity = true;
                s2.agent(a)->velocity = {300, 0, 0};
                s2.agent(b)->velocity = {-300, 0, 0};
                s2.stepAvoidance(m2, 1.0f / 30.0f);
                for (u32 id : {a, b}) {
                    NavAgentData* d = s2.agent(id);
                    d->position += d->safeVelocity * (1.0f / 30.0f);
                }
                const Vec3 pa = s2.agent(a)->position, pb = s2.agent(b)->position;
                minGap = std::min(minGap, std::sqrt((pa.x - pb.x) * (pa.x - pb.x) + (pa.y - pb.y) * (pa.y - pb.y)));
            }
            check(minGap > 95.0f, "they pass each other without touching (gap " + std::to_string(minGap) + ")");
            check(s2.agent(a)->position.x > 600 && s2.agent(b)->position.x < -200, "and keep going");
        }
        {
            // Layers: an agent whose mask does not include the other's layers ignores it.
            NavServer s3;
            const u32 m3 = s3.createMap();
            const u32 a = s3.createAgent(m3), b = s3.createAgent(m3);
            for (u32 id : {a, b}) {
                NavAgentData* d = s3.agent(id);
                d->avoidance = true; d->radius = 50; d->maxSpeed = 300; d->timeHorizonAgents = 2.0f; d->hasVelocity = true;
            }
            s3.agent(a)->position = {0, 0, 0};   s3.agent(a)->velocity = {300, 0, 0};
            s3.agent(b)->position = {200, 0, 0}; s3.agent(b)->velocity = {-300, 0, 0};
            s3.agent(a)->avoidanceMask = 2; s3.agent(b)->avoidanceLayers = 1;
            s3.stepAvoidance(m3, 1.0f / 30.0f);
            check(near3(s3.agent(a)->safeVelocity, {300, 0, 0}, 0.01f), "an agent ignores layers outside its mask");
            // Heights: agents on different floors do not see each other.
            s3.agent(a)->avoidanceMask = 1;
            s3.agent(a)->hasVelocity = true;
            s3.agent(b)->position.z = 1000;
            s3.stepAvoidance(m3, 1.0f / 30.0f);
            check(near3(s3.agent(a)->safeVelocity, {300, 0, 0}, 0.01f), "agents on other floors are ignored");
            // Priority: the lower priority gives way more.
            s3.agent(b)->position.z = 0;
            s3.agent(a)->priority = 0.0f; s3.agent(b)->priority = 1.0f;
            s3.agent(a)->hasVelocity = true; s3.agent(b)->hasVelocity = true;
            s3.agent(a)->position = {0, 0, 0}; s3.agent(b)->position = {250, 20, 0};
            s3.stepAvoidance(m3, 1.0f / 30.0f);
            const Vec3 dvA = s3.agent(a)->safeVelocity - Vec3{300, 0, 0};
            const Vec3 dvB = s3.agent(b)->safeVelocity - Vec3{-300, 0, 0};
            check(dvA.size() > dvB.size(), "the low-priority agent changes its velocity more");
        }
        {
            // A wall: an agent running at it is slowed to a stop at the wall rather than through it.
            NavServer s4;
            const u32 m4 = s4.createMap();
            const u32 o = s4.createObstacle(m4);
            NavObstacleData* od = s4.obstacle(o);
            od->avoidance = true;
            od->height = 300;
            // A strip x in [400, 500]. Clockwise in XY (negative area) pushes agents out, counter-clockwise keeps them in: the agent
            // lives on the left of each edge.
            od->verts = {{400, -1000, 0}, {400, 1000, 0}, {500, 1000, 0}, {500, -1000, 0}};
            const u32 a = s4.createAgent(m4);
            NavAgentData* d = s4.agent(a);
            d->avoidance = true; d->radius = 50; d->maxSpeed = 300; d->timeHorizonObstacles = 2.0f; d->height = 180;
            d->position = {300, 0, 0}; d->velocity = {300, 0, 0}; d->hasVelocity = true;
            s4.stepAvoidance(m4, 1.0f / 30.0f);
            check(d->safeVelocity.x < 120.0f, "a pushing-out wall slows an agent running at it (" + std::to_string(d->safeVelocity.x) + ")");
            od->verts = {{400, -1000, 0}, {500, -1000, 0}, {500, 1000, 0}, {400, 1000, 0}};   // counter-clockwise: agents are kept inside
            d->hasVelocity = true;
            s4.stepAvoidance(m4, 1.0f / 30.0f);
            check(d->safeVelocity.x > 299.0f, "the same strip wound the other way does not stop an agent outside it");
            od->verts = {{400, -1000, 0}, {400, 1000, 0}, {500, 1000, 0}, {500, -1000, 0}};
            od->height = 100;
            d->position.z = 500;   // above the obstacle's height range
            d->hasVelocity = true;
            s4.stepAvoidance(m4, 1.0f / 30.0f);
            check(d->safeVelocity.x > 299.0f, "a wall below the agent is not in its way");
        }
        {
            // Round obstacle.
            NavServer s5;
            const u32 m5 = s5.createMap();
            const u32 o = s5.createObstacle(m5);
            s5.obstacle(o)->avoidance = true; s5.obstacle(o)->radius = 80; s5.obstacle(o)->height = 300; s5.obstacle(o)->position = {300, 0, 0};
            const u32 a = s5.createAgent(m5);
            NavAgentData* d = s5.agent(a);
            d->avoidance = true; d->radius = 50; d->maxSpeed = 300; d->timeHorizonAgents = 2.0f; d->height = 180;
            d->position = {0, 0, 0}; d->velocity = {300, 0, 0}; d->hasVelocity = true;
            s5.stepAvoidance(m5, 1.0f / 30.0f);
            check(std::fabs(d->safeVelocity.y) > 1.0f, "a round obstacle ahead is steered around");
        }
    }

    AVER_INFO(g_failures == 0 ? "NavMeshTest passed" : "NavMeshTest FAILED");
    return g_failures == 0 ? 0 : 1;
}
