// Aver.Synapse.Gpu: one ORCA avoidance solve per agent.
//
// A line-for-line mirror of CrowdOrca.cpp (orcaAgentLine, orcaWallLine, the three linear programs)
// and of CrowdCpuSolver::solve's neighbour and wall gathering. The CPU runs the contact pass
// afterwards, so this kernel only chooses velocities. Where it differs from the CPU solver:
// neighbours and walls are the K/W nearest found by insertion rather than a sort, so tie-breaks
// (and so exact results) are not bit-identical.

#define MAXN 16
#define MAXW 8
#define MAXL 24   // MAXN + MAXW

struct GpuAgent {
    float2 pos;
    float2 vel;
    float2 pref;     // biased preferred velocity, computed on the CPU
    float  radius;
    float  maxSpeed;
    float  priority;
    float  stuck;
    uint   flags;    // bit 0: static
    uint   id;
};

StructuredBuffer<GpuAgent> gAgents    : register(t0);
StructuredBuffer<uint>     gCellStart : register(t1);
StructuredBuffer<uint>     gItems     : register(t2);
StructuredBuffer<uint>     gBlocked   : register(t3);   // 1 = wall cell, wallW*wallH entries
RWStructuredBuffer<float2> gOut       : register(u0);

// 20 dwords, in this order; CrowdGpu.cpp writes cb_[0..19] to match.
cbuffer CrowdCB : register(b1) {
    uint  gCount;
    uint  gGridW;
    uint  gGridH;
    uint  gMaxNeighbors;
    float gMinX;
    float gMinY;
    float gCellSize;
    float gNeighborRadius;
    float gTimeHorizon;
    float gObstacleHorizon;
    float gStep;
    float gPad;
    uint  gWallW;
    uint  gWallH;
    float gWallOriginX;
    float gWallOriginY;
    float gWallCell;
    uint  gMaxWalls;
    uint  gHasWalls;
    float gStuckSeconds;
};

// Two line sets: 0 is the problem, 1 is the projected set linearProgram3 builds.
static float2 sP[2][MAXL];
static float2 sD[2][MAXL];

float det2(float2 a, float2 b) { return a.x * b.y - a.y * b.x; }

bool lp1(uint set, uint lineNo, float radius, float2 opt, bool dirOpt, inout float2 result) {
    float2 P = sP[set][lineNo];
    float2 D = sD[set][lineNo];
    float dotp = dot(P, D);
    float disc = dotp * dotp + radius * radius - dot(P, P);
    if (disc < 0.0) return false;
    float root = sqrt(disc);
    float tL = -dotp - root;
    float tR = -dotp + root;
    for (uint i = 0; i < lineNo; ++i) {
        float denom = det2(D, sD[set][i]);
        float numer = det2(sD[set][i], P - sP[set][i]);
        if (abs(denom) <= 1e-5) {
            if (numer < 0.0) return false;
            continue;
        }
        float t = numer / denom;
        if (denom >= 0.0) tR = min(tR, t);
        else              tL = max(tL, t);
        if (tL > tR) return false;
    }
    if (dirOpt) {
        result = P + (dot(opt, D) > 0.0 ? tR : tL) * D;
    } else {
        float t = clamp(dot(D, opt - P), tL, tR);
        result = P + t * D;
    }
    return true;
}

uint lp2(uint set, uint count, float radius, float2 opt, bool dirOpt, inout float2 result) {
    if (dirOpt)                           result = opt * radius;
    else if (dot(opt, opt) > radius * radius) result = normalize(opt) * radius;
    else                                  result = opt;
    for (uint i = 0; i < count; ++i) {
        if (det2(sD[set][i], sP[set][i] - result) > 0.0) {
            float2 keep = result;
            if (!lp1(set, i, radius, opt, dirOpt, result)) {
                result = keep;
                return i;
            }
        }
    }
    return count;
}

void lp3(uint count, uint hard, uint begin, float radius, inout float2 result) {
    float worst = 0.0;
    for (uint i = begin; i < count; ++i) {
        if (det2(sD[0][i], sP[0][i] - result) <= worst) continue;
        uint n = 0;
        for (uint k = 0; k < hard; ++k) {
            sP[1][n] = sP[0][k];
            sD[1][n] = sD[0][k];
            ++n;
        }
        for (uint j = hard; j < i; ++j) {
            float d = det2(sD[0][i], sD[0][j]);
            float2 pt;
            if (abs(d) <= 1e-5) {
                if (dot(sD[0][i], sD[0][j]) > 0.0) continue;
                pt = 0.5 * (sP[0][i] + sP[0][j]);
            } else {
                pt = sP[0][i] + (det2(sD[0][j], sP[0][i] - sP[0][j]) / d) * sD[0][i];
            }
            float2 dir = sD[0][j] - sD[0][i];
            float l = length(dir);
            sP[1][n] = pt;
            sD[1][n] = l > 1e-6 ? dir / l : float2(1.0, 0.0);
            ++n;
        }
        float2 keep = result;
        float2 optDir = float2(-sD[0][i].y, sD[0][i].x);
        if (lp2(1, n, radius, optDir, true, result) < n) result = keep;
        worst = det2(sD[0][i], sP[0][i] - result);
    }
}

bool blockedCell(int cx, int cy) {
    if (cx < 0 || cy < 0 || cx >= (int)gWallW || cy >= (int)gWallH) return true;
    return gBlocked[(uint)cy * gWallW + (uint)cx] != 0;
}

void addWallLine(uint idx, float2 pos, float radius, float2 closest, float2 fallbackNormal) {
    float2 away = pos - closest;
    float d = length(away);
    float2 n = d > 1e-5 ? away / d : fallbackNormal;
    float c = -(d - radius) / gObstacleHorizon;
    sP[0][idx] = n * c;
    sD[0][idx] = float2(n.y, -n.x);
}

void addAgentLine(uint idx, GpuAgent a, GpuAgent b) {
    float share = 0.5;
    if ((b.flags & 1u) != 0 || b.maxSpeed <= 0.0) {
        share = 1.0;
    } else if (a.stuck >= gStuckSeconds && b.stuck >= gStuckSeconds) {
        bool aWins = (a.priority > b.priority) || (a.priority == b.priority && a.id < b.id);
        share = aWins ? 0.2 : 0.8;
    }

    float2 relPos = b.pos - a.pos;
    float2 relVel = a.vel - b.vel;
    float distSq = dot(relPos, relPos);
    float rr = a.radius + b.radius;
    float rrSq = rr * rr;
    float2 dir;
    float2 u;
    if (distSq > rrSq) {
        float invT = 1.0 / gTimeHorizon;
        float2 w = relVel - relPos * invT;
        float wLenSq = dot(w, w);
        float dot1 = dot(w, relPos);
        if (dot1 < 0.0 && dot1 * dot1 > rrSq * wLenSq) {
            float wLen = sqrt(wLenSq);
            float2 unitW = w / wLen;
            dir = float2(unitW.y, -unitW.x);
            u = unitW * (rr * invT - wLen);
        } else {
            float leg = sqrt(distSq - rrSq);
            if (det2(relPos, w) > 0.0)
                dir = float2(relPos.x * leg - relPos.y * rr, relPos.x * rr + relPos.y * leg) / distSq;
            else
                dir = -(float2(relPos.x * leg + relPos.y * rr, -relPos.x * rr + relPos.y * leg) / distSq);
            u = dir * dot(relVel, dir) - relVel;
        }
    } else {
        float invDt = 1.0 / gStep;
        float2 w = relVel - relPos * invDt;
        float wLen = length(w);
        float2 unitW = wLen > 1e-6 ? w / wLen : float2(1.0, 0.0);
        dir = float2(unitW.y, -unitW.x);
        u = unitW * (rr * invDt - wLen);
    }
    sP[0][idx] = a.vel + u * share;
    sD[0][idx] = dir;
}

[numthreads(64, 1, 1)]
void CSCrowd(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= gCount) return;

    GpuAgent a = gAgents[i];
    if ((a.flags & 1u) != 0 || a.maxSpeed <= 0.0) {
        gOut[i] = float2(0.0, 0.0);
        return;
    }

    uint nLines = 0;

    // ---- walls: the gMaxWalls nearest boundary cells within reach ------------------------------
    if (gHasWalls != 0) {
        float reach = a.maxSpeed * gObstacleHorizon;
        float range = a.radius + reach;
        int x0 = (int)floor((a.pos.x - range - gWallOriginX) / gWallCell);
        int x1 = (int)floor((a.pos.x + range - gWallOriginX) / gWallCell);
        int y0 = (int)floor((a.pos.y - range - gWallOriginY) / gWallCell);
        int y1 = (int)floor((a.pos.y + range - gWallOriginY) / gWallCell);

        float wd[MAXW];
        float2 wp[MAXW];
        float2 wc[MAXW];
        uint wn = 0;
        uint maxW = min(gMaxWalls, (uint)MAXW);

        for (int cy = y0; cy <= y1; ++cy) {
            for (int cx = x0; cx <= x1; ++cx) {
                if (!blockedCell(cx, cy)) continue;
                if (blockedCell(cx - 1, cy) && blockedCell(cx + 1, cy) &&
                    blockedCell(cx, cy - 1) && blockedCell(cx, cy + 1)) continue;
                float2 lo = float2(gWallOriginX + cx * gWallCell, gWallOriginY + cy * gWallCell);
                float2 p = clamp(a.pos, lo, lo + gWallCell);
                float d = length(a.pos - p);
                if (d > range) continue;

                uint slot;
                if (wn < maxW) {
                    slot = wn++;
                } else {
                    uint farIdx = 0;
                    for (uint k = 1; k < maxW; ++k) if (wd[k] > wd[farIdx]) farIdx = k;
                    if (d >= wd[farIdx]) continue;
                    slot = farIdx;
                }
                wd[slot] = d;
                wp[slot] = p;
                wc[slot] = lo + 0.5 * gWallCell;
            }
        }
        for (uint k = 0; k < wn; ++k) {
            float2 away = a.pos - wc[k];
            float al = length(away);
            addWallLine(nLines++, a.pos, a.radius, wp[k], al > 1e-6 ? away / al : float2(1.0, 0.0));
        }
    }
    uint hard = nLines;

    // ---- neighbours: the gMaxNeighbors nearest in the 3x3 cells around the agent ---------------
    float nd[MAXN];
    uint ni[MAXN];
    uint nn = 0;
    uint maxN = min(gMaxNeighbors, (uint)MAXN);
    float reachSq = gNeighborRadius * gNeighborRadius;

    int cellX = clamp((int)floor((a.pos.x - gMinX) / gCellSize), 0, (int)gGridW - 1);
    int cellY = clamp((int)floor((a.pos.y - gMinY) / gCellSize), 0, (int)gGridH - 1);
    for (int ny = max(cellY - 1, 0); ny <= min(cellY + 1, (int)gGridH - 1); ++ny) {
        for (int nx = max(cellX - 1, 0); nx <= min(cellX + 1, (int)gGridW - 1); ++nx) {
            uint c = (uint)ny * gGridW + (uint)nx;
            for (uint k = gCellStart[c]; k < gCellStart[c + 1]; ++k) {
                uint j = gItems[k];
                if (j == i) continue;
                float2 dv = gAgents[j].pos - a.pos;
                float d2 = dot(dv, dv);
                if (d2 > reachSq) continue;

                uint slot;
                if (nn < maxN) {
                    slot = nn++;
                } else {
                    uint farIdx = 0;
                    for (uint m = 1; m < maxN; ++m) if (nd[m] > nd[farIdx]) farIdx = m;
                    if (d2 >= nd[farIdx]) continue;
                    slot = farIdx;
                }
                nd[slot] = d2;
                ni[slot] = j;
            }
        }
    }
    for (uint k = 0; k < nn; ++k) addAgentLine(nLines++, a, gAgents[ni[k]]);

    float2 result;
    uint fail = lp2(0, nLines, a.maxSpeed, a.pref, false, result);
    if (fail < nLines) lp3(nLines, hard, fail, a.maxSpeed, result);
    gOut[i] = result;
}
