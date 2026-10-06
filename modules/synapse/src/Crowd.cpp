#include "aver/synapse/Crowd.hpp"

#include "aver/core/Math.hpp"
#include "aver/synapse/Steering.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace aver::synapse {
namespace {

bool finite2(V2 p) { return std::isfinite(p.x) && std::isfinite(p.y); }

// Responsibility split for a pair that is both stuck: the winner keeps going, the other gives way.
constexpr f32 kWinnerShare = 0.2f;
constexpr f32 kLoserShare  = 0.8f;

f32 maxRadius(const CrowdAgent* a, u32 n) {
    f32 r = 0.0f;
    for (u32 i = 0; i < n; ++i) r = std::max(r, a[i].radius);
    return r;
}

bool outranks(const CrowdAgent& a, const CrowdAgent& b) {
    if (a.priority != b.priority) return a.priority > b.priority;
    return a.id < b.id;
}

} // namespace

// ---- grid -----------------------------------------------------------------------------------------

void CrowdGrid::build(const CrowdAgent* a, u32 n, f32 minCell) {
    cellStart.clear();
    items.clear();
    w = h = 0;
    cellSize = std::max(minCell, 1.0f);
    if (n == 0) return;

    f32 mnx = a[0].pos.x, mxx = mnx, mny = a[0].pos.y, mxy = mny;
    for (u32 i = 1; i < n; ++i) {
        mnx = std::min(mnx, a[i].pos.x); mxx = std::max(mxx, a[i].pos.x);
        mny = std::min(mny, a[i].pos.y); mxy = std::max(mxy, a[i].pos.y);
    }
    for (;;) {
        w = static_cast<u32>(std::floor((mxx - mnx) / cellSize)) + 1;
        h = static_cast<u32>(std::floor((mxy - mny) / cellSize)) + 1;
        if (static_cast<u64>(w) * h <= kMaxCells) break;
        cellSize *= 1.5f;
    }
    minX = mnx;
    minY = mny;

    const usize cells = static_cast<usize>(w) * h;
    cellStart.assign(cells + 1, 0);
    thread_local std::vector<u32> cellOfAgent;
    cellOfAgent.resize(n);
    for (u32 i = 0; i < n; ++i) {
        i32 cx, cy;
        cellOf(a[i].pos, cx, cy);
        const u32 c = static_cast<u32>(cy) * w + static_cast<u32>(cx);
        cellOfAgent[i] = c;
        ++cellStart[c + 1];
    }
    for (usize c = 0; c < cells; ++c) cellStart[c + 1] += cellStart[c];

    items.resize(n);
    thread_local std::vector<u32> cursor;
    cursor.assign(cellStart.begin(), cellStart.end() - 1);
    for (u32 i = 0; i < n; ++i) items[cursor[cellOfAgent[i]]++] = i;
}

void CrowdGrid::cellOf(V2 p, i32& cx, i32& cy) const {
    cx = static_cast<i32>(std::floor((p.x - minX) / cellSize));
    cy = static_cast<i32>(std::floor((p.y - minY) / cellSize));
    cx = std::clamp(cx, 0, static_cast<i32>(w) - 1);
    cy = std::clamp(cy, 0, static_cast<i32>(h) - 1);
}

// ---- CPU solver -----------------------------------------------------------------------------------

V2 CrowdCpuSolver::biasedPreferred(const CrowdAgent& a, const CrowdParams& p) {
    const f32 sp = len2(a.prefVel);
    if (sp < 1.0f) return {};
    // Everyone drifts the same rotational way, so head-on pairs part on opposite sides instead of
    // meeting nose to nose and cancelling exactly.
    const f32 jitter = p.sidestepBias * (0.5f + hashToUnit(hashU32(a.id)));
    return clampLen2(a.prefVel + perp2(a.prefVel * (1.0f / sp)) * (sp * jitter), a.maxSpeed);
}

bool CrowdCpuSolver::solve(const CrowdSolveInput& in, CrowdSolveOutput& out) {
    const u32 n = in.count;
    out.ids.resize(n);
    out.vel.resize(n);
    if (n == 0 || !in.agents || !in.params) return true;

    const CrowdParams& P = *in.params;
    const CrowdAgent* A = in.agents;
    grid_.build(A, n, std::max(P.neighborRadiusCm, 2.0f * maxRadius(A, n) + 50.0f));
    const f32 reachSq = P.neighborRadiusCm * P.neighborRadiusCm;
    const auto less = [](const std::pair<f32, u32>& x, const std::pair<f32, u32>& y) {
        return x.first < y.first || (x.first == y.first && x.second < y.second);
    };

    for (u32 i = 0; i < n; ++i) {
        const CrowdAgent& a = A[i];
        out.ids[i] = a.id;
        if (a.flags & kCrowdStatic) { out.vel[i] = {}; continue; }

        cands_.clear();
        i32 cx, cy;
        grid_.cellOf(a.pos, cx, cy);
        for (i32 ny = std::max(cy - 1, 0); ny <= std::min(cy + 1, static_cast<i32>(grid_.h) - 1); ++ny) {
            for (i32 nx = std::max(cx - 1, 0); nx <= std::min(cx + 1, static_cast<i32>(grid_.w) - 1); ++nx) {
                const u32 c = static_cast<u32>(ny) * grid_.w + static_cast<u32>(nx);
                for (u32 k = grid_.cellStart[c]; k < grid_.cellStart[c + 1]; ++k) {
                    const u32 j = grid_.items[k];
                    if (j == i) continue;
                    const f32 d2 = distSq2(a.pos, A[j].pos);
                    if (d2 <= reachSq) cands_.push_back({d2, j});
                }
            }
        }
        if (cands_.size() > P.maxNeighbors) {
            std::nth_element(cands_.begin(), cands_.begin() + P.maxNeighbors, cands_.end(), less);
            cands_.resize(P.maxNeighbors);
        }
        std::sort(cands_.begin(), cands_.end(), less);

        lines_.clear();
        if (in.nav)
            orcaGatherWalls(*in.nav, a.pos, a.radius, a.maxSpeed * P.obstacleHorizon, P.obstacleHorizon,
                            P.maxWallLines, lines_);
        const u32 hard = static_cast<u32>(lines_.size());

        for (const auto& c : cands_) {
            const CrowdAgent& b = A[c.second];
            f32 share = 0.5f;
            if ((b.flags & kCrowdStatic) || b.maxSpeed <= 0.0f) {
                share = 1.0f;   // B cannot get out of the way
            } else if (a.stuckSec >= P.stuckSeconds && b.stuckSec >= P.stuckSeconds) {
                share = outranks(a, b) ? kWinnerShare : kLoserShare;
            }
            lines_.push_back(orcaAgentLine(a.pos, a.vel, a.radius, b.pos, b.vel, b.radius, share,
                                           P.timeHorizon, P.fixedStep));
        }
        out.vel[i] = orcaSolve(lines_, hard, biasedPreferred(a, P), a.maxSpeed, scratch_);
    }
    return true;
}

// ---- sim ------------------------------------------------------------------------------------------

const char* CrowdSim::activeBackendName() const {
    if (kind_ == CrowdBackendKind::Gpu && gpu_ && gpu_->available()) return gpu_->name();
    return cpu_.name();
}

void CrowdSim::reindex() {
    index_.clear();
    for (u32 i = 0; i < agents_.size(); ++i) index_[agents_[i].id] = i;
}

bool CrowdSim::addAgent(const CrowdAgent& in) {
    if (agents_.size() >= params_.maxAgents) return false;
    if (index_.count(in.id)) return false;
    if (!finite2(in.pos) || !finite2(in.vel) || !finite2(in.prefVel)) return false;
    CrowdAgent a = in;
    a.avoidVel = clampLen2(a.prefVel, a.maxSpeed);
    a.stuckSec = 0.0f;
    index_[a.id] = static_cast<u32>(agents_.size());
    agents_.push_back(a);
    return true;
}

bool CrowdSim::removeAgent(u32 id) {
    const auto it = index_.find(id);
    if (it == index_.end()) return false;
    agents_.erase(agents_.begin() + it->second);   // order-preserving: determinism needs a stable order
    reindex();
    return true;
}

CrowdAgent* CrowdSim::find(u32 id) {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : &agents_[it->second];
}

const CrowdAgent* CrowdSim::find(u32 id) const {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : &agents_[it->second];
}

void CrowdSim::clear() {
    agents_.clear();
    index_.clear();
    accumulator_ = 0.0f;
}

void CrowdSim::pushOutOfWalls(CrowdAgent& a) const {
    if (!nav_ || nav_->cellSizeCm <= 0.0f) return;
    const f32 cs = nav_->cellSizeCm;
    const f32 r = a.radius;
    const i32 x0 = static_cast<i32>(std::floor((a.pos.x - r - nav_->originXCm) / cs));
    const i32 x1 = static_cast<i32>(std::floor((a.pos.x + r - nav_->originXCm) / cs));
    const i32 y0 = static_cast<i32>(std::floor((a.pos.y - r - nav_->originYCm) / cs));
    const i32 y1 = static_cast<i32>(std::floor((a.pos.y + r - nav_->originYCm) / cs));

    for (i32 cy = y0; cy <= y1; ++cy) {
        for (i32 cx = x0; cx <= x1; ++cx) {
            if (!navCellBlocked(*nav_, cx, cy)) continue;
            const f32 minX = nav_->originXCm + static_cast<f32>(cx) * cs;
            const f32 minY = nav_->originYCm + static_cast<f32>(cy) * cs;
            const V2 p = closestPointOnCell(a.pos, minX, minY, cs);
            const f32 d = dist2(a.pos, p);
            if (d >= r) continue;
            if (d > 1e-4f) {
                a.pos += (a.pos - p) * ((r - d) / d);
            } else {
                // Centre inside the wall cell: leave through the nearest face.
                const V2 centre{minX + cs * 0.5f, minY + cs * 0.5f};
                const f32 dx = a.pos.x - centre.x, dy = a.pos.y - centre.y;
                if (std::fabs(dx) >= std::fabs(dy))
                    a.pos.x = centre.x + (dx < 0.0f ? -1.0f : 1.0f) * (cs * 0.5f + r);
                else
                    a.pos.y = centre.y + (dy < 0.0f ? -1.0f : 1.0f) * (cs * 0.5f + r);
            }
        }
    }
}

void CrowdSim::resolveContacts() {
    const u32 n = static_cast<u32>(agents_.size());
    if (n == 0) return;
    grid_.build(agents_.data(), n, std::max(params_.neighborRadiusCm, 2.0f * maxRadius(agents_.data(), n) + 50.0f));

    for (u32 it = 0; it < params_.contactIterations; ++it) {
        for (u32 i = 0; i < n; ++i) {
            CrowdAgent& a = agents_[i];
            i32 cx, cy;
            grid_.cellOf(a.pos, cx, cy);
            for (i32 ny = std::max(cy - 1, 0); ny <= std::min(cy + 1, static_cast<i32>(grid_.h) - 1); ++ny) {
                for (i32 nx = std::max(cx - 1, 0); nx <= std::min(cx + 1, static_cast<i32>(grid_.w) - 1); ++nx) {
                    const u32 c = static_cast<u32>(ny) * grid_.w + static_cast<u32>(nx);
                    for (u32 k = grid_.cellStart[c]; k < grid_.cellStart[c + 1]; ++k) {
                        const u32 j = grid_.items[k];
                        if (j <= i) continue;   // each pair once
                        CrowdAgent& b = agents_[j];
                        const f32 minD = a.radius + b.radius;
                        const V2 d = a.pos - b.pos;
                        const f32 dSq = lenSq2(d);
                        if (dSq >= minD * minD) continue;
                        const bool sa = (a.flags & kCrowdStatic) != 0;
                        const bool sb = (b.flags & kCrowdStatic) != 0;
                        if (sa && sb) continue;
                        const f32 dl = std::sqrt(dSq);
                        V2 dir;
                        if (dl > 1e-4f) {
                            dir = d * (1.0f / dl);
                        } else {
                            const f32 ang = hashToUnit(hashU32(a.id * 2654435761u ^ b.id)) * kTwoPi;
                            dir = {std::cos(ang), std::sin(ang)};
                        }
                        const f32 push = minD - dl;
                        const f32 wa = sa ? 0.0f : (sb ? 1.0f : 0.5f);
                        const f32 wb = sb ? 0.0f : (sa ? 1.0f : 0.5f);
                        a.pos += dir * (push * wa);
                        b.pos -= dir * (push * wb);
                    }
                }
            }
        }
        // Walls last, so an agent squeezed between a wall and a neighbour ends in the corridor.
        for (CrowdAgent& a : agents_)
            if (!(a.flags & kCrowdStatic)) pushOutOfWalls(a);
    }
}

void CrowdSim::step() {
    const u32 n = static_cast<u32>(agents_.size());
    if (n == 0) return;
    const f32 dt = params_.fixedStep;

    ICrowdBackend* be = (kind_ == CrowdBackendKind::Gpu && gpu_ && gpu_->available())
                            ? gpu_
                            : static_cast<ICrowdBackend*>(&cpu_);
    const CrowdSolveInput in{agents_.data(), n, &params_, nav_};
    if (be->solve(in, out_)) {
        for (usize k = 0; k < out_.ids.size(); ++k) {
            CrowdAgent* a = (k < n && agents_[k].id == out_.ids[k]) ? &agents_[k] : find(out_.ids[k]);
            if (a) a->avoidVel = out_.vel[k];
        }
    }

    for (CrowdAgent& a : agents_) {
        if (a.flags & kCrowdStatic) { a.vel = {}; continue; }
        a.vel = accelerateToward(a.vel, clampLen2(a.avoidVel, a.maxSpeed), a.maxAccel, dt);
        a.pos += a.vel * dt;
    }

    resolveContacts();

    for (CrowdAgent& a : agents_) {
        const f32 want = len2(a.prefVel);
        if (!(a.flags & kCrowdStatic) && want > 0.2f * a.maxSpeed && len2(a.vel) < params_.stuckSpeedFrac * want)
            a.stuckSec += dt;
        else
            a.stuckSec = std::max(0.0f, a.stuckSec - 2.0f * dt);
    }
}

u32 CrowdSim::advance(f32 dt) {
    accumulator_ += std::max(dt, 0.0f);
    u32 steps = 0;
    while (accumulator_ >= params_.fixedStep && steps < params_.maxStepsPerAdvance) {
        step();
        accumulator_ -= params_.fixedStep;
        ++steps;
    }
    // A stall must not queue unbounded catch-up.
    accumulator_ = std::min(accumulator_, params_.fixedStep);
    return steps;
}

f32 CrowdSim::worstOverlap() const {
    const u32 n = static_cast<u32>(agents_.size());
    f32 worst = 0.0f;
    if (n == 0) return worst;

    CrowdGrid g;
    g.build(agents_.data(), n, std::max(params_.neighborRadiusCm, 2.0f * maxRadius(agents_.data(), n) + 50.0f));
    for (u32 i = 0; i < n; ++i) {
        const CrowdAgent& a = agents_[i];
        i32 cx, cy;
        g.cellOf(a.pos, cx, cy);
        for (i32 ny = std::max(cy - 1, 0); ny <= std::min(cy + 1, static_cast<i32>(g.h) - 1); ++ny) {
            for (i32 nx = std::max(cx - 1, 0); nx <= std::min(cx + 1, static_cast<i32>(g.w) - 1); ++nx) {
                const u32 c = static_cast<u32>(ny) * g.w + static_cast<u32>(nx);
                for (u32 k = g.cellStart[c]; k < g.cellStart[c + 1]; ++k) {
                    const u32 j = g.items[k];
                    if (j <= i) continue;
                    worst = std::max(worst, a.radius + agents_[j].radius - dist2(a.pos, agents_[j].pos));
                }
            }
        }
        if (nav_ && nav_->cellSizeCm > 0.0f) {
            const f32 cs = nav_->cellSizeCm, r = a.radius;
            const i32 x0 = static_cast<i32>(std::floor((a.pos.x - r - nav_->originXCm) / cs));
            const i32 x1 = static_cast<i32>(std::floor((a.pos.x + r - nav_->originXCm) / cs));
            const i32 y0 = static_cast<i32>(std::floor((a.pos.y - r - nav_->originYCm) / cs));
            const i32 y1 = static_cast<i32>(std::floor((a.pos.y + r - nav_->originYCm) / cs));
            for (i32 wy = y0; wy <= y1; ++wy)
                for (i32 wx = x0; wx <= x1; ++wx) {
                    if (!navCellBlocked(*nav_, wx, wy)) continue;
                    const V2 p = closestPointOnCell(a.pos, nav_->originXCm + static_cast<f32>(wx) * cs,
                                                    nav_->originYCm + static_cast<f32>(wy) * cs, cs);
                    worst = std::max(worst, r - dist2(a.pos, p));
                }
        }
    }
    return worst;
}

} // namespace aver::synapse
