#include "GraphEditorGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <vector>

namespace aver::editor {

namespace {

bool ciEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

const fmt::OcGraphNode* findNode(const fmt::OcGraphData& g, const std::string& id) {
    for (const auto& n : g.nodes)
        if (n.id == id) return &n;
    return nullptr;
}

const fmt::OcGraphPin* findPin(const fmt::OcGraphNode& n, const std::string& name) {
    for (const auto& p : n.pins)
        if (p.name == name) return &p;
    return nullptr;
}

f32 dist(Vec2 a, Vec2 b) {
    const f32 dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

// Shortest distance from `p` to the segment [a,b].
f32 pointSegmentDist(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = b - a;
    const f32 abLen2 = ab.x * ab.x + ab.y * ab.y;
    if (abLen2 <= 1e-9f) return dist(p, a);
    const Vec2 ap = p - a;
    f32 t = (ap.x * ab.x + ap.y * ab.y) / abLen2;
    t = std::clamp(t, 0.0f, 1.0f);
    const Vec2 closest = a + ab * t;
    return dist(p, closest);
}

} // namespace

// ---------------------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------------------

GraphNodeLayout computeNodeLayout(const fmt::OcGraphNode& node, const std::string& title,
                                   const GraphLayoutStyle& style, f32 scale) {
    GraphNodeLayout layout;
    layout.nodeId = node.id;

    usize numInputs = 0, numOutputs = 0;
    f32 maxInputLabelPx = 0.0f, maxOutputLabelPx = 0.0f;
    for (const auto& p : node.pins) {
        const f32 labelPx = static_cast<f32>(p.name.size()) * style.charWidthPx * scale;
        if (p.isOutput) {
            ++numOutputs;
            maxOutputLabelPx = std::max(maxOutputLabelPx, labelPx);
        } else {
            ++numInputs;
            maxInputLabelPx = std::max(maxInputLabelPx, labelPx);
        }
    }

    const f32 titleWidthPx = static_cast<f32>(title.size()) * style.charWidthPx * scale + 2.0f * style.paddingXPx * scale;
    f32 pinsWidthPx = 2.0f * style.paddingXPx * scale;
    if (numInputs > 0) pinsWidthPx += maxInputLabelPx + style.pinLabelReservePx * scale;
    if (numOutputs > 0) pinsWidthPx += maxOutputLabelPx + style.pinLabelReservePx * scale;

    const f32 width = std::max({style.minWidthPx * scale, titleWidthPx, pinsWidthPx});
    const usize rowCount = std::max(numInputs, numOutputs);
    const f32 height = style.headerHeightPx * scale + static_cast<f32>(rowCount) * style.pinRowHeightPx * scale
                        + style.paddingBottomPx * scale;

    layout.min = Vec2(static_cast<f32>(node.x), static_cast<f32>(node.y));
    layout.max = layout.min + Vec2(width, height);

    usize inputIdx = 0, outputIdx = 0;
    layout.pins.reserve(node.pins.size());
    for (const auto& p : node.pins) {
        GraphPinLayout pl;
        pl.name = p.name;
        pl.type = p.type;
        pl.isOutput = p.isOutput;
        const usize row = p.isOutput ? outputIdx++ : inputIdx++;
        const f32 y = layout.min.y + style.headerHeightPx * scale
                      + (static_cast<f32>(row) + 0.5f) * style.pinRowHeightPx * scale;
        pl.pos = Vec2(p.isOutput ? layout.max.x : layout.min.x, y);
        layout.pins.push_back(pl);
    }

    return layout;
}

// ---------------------------------------------------------------------------------------------------
// Hit-testing
// ---------------------------------------------------------------------------------------------------

const GraphPinLayout* findPinLayout(const std::vector<GraphNodeLayout>& layouts,
                                     const std::string& nodeId, const std::string& pinName) {
    for (const auto& nl : layouts) {
        if (nl.nodeId != nodeId) continue;
        for (const auto& pl : nl.pins)
            if (pl.name == pinName) return &pl;
    }
    return nullptr;
}

GraphBezier linkBezier(Vec2 sourcePos, Vec2 destPos) {
    // Minimum tangent length keeps the curve from degenerating into a near-straight vertical line
    // when two nodes sit almost directly above/below each other (dx close to 0).
    const f32 tangent = std::max(std::abs(destPos.x - sourcePos.x) * 0.5f, 40.0f);
    GraphBezier b;
    b.p1 = sourcePos;
    b.p2 = sourcePos + Vec2(tangent, 0.0f);
    b.p3 = destPos - Vec2(tangent, 0.0f);
    b.p4 = destPos;
    return b;
}

void sampleCubicBezier(const GraphBezier& b, int segments, std::vector<Vec2>& outPoints) {
    outPoints.clear();
    if (segments < 1) segments = 1;
    outPoints.reserve(static_cast<usize>(segments) + 1);
    for (int i = 0; i <= segments; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(segments);
        const f32 u = 1.0f - t;
        const f32 w1 = u * u * u;
        const f32 w2 = 3.0f * u * u * t;
        const f32 w3 = 3.0f * u * t * t;
        const f32 w4 = t * t * t;
        outPoints.push_back(b.p1 * w1 + b.p2 * w2 + b.p3 * w3 + b.p4 * w4);
    }
}

GraphHitResult hitTest(const fmt::OcGraphData& graph, const std::vector<GraphNodeLayout>& layouts,
                        Vec2 canvasPoint, const GraphLayoutStyle& style, f32 scale) {
    // Pins first: they are the smallest targets and must win over the node body they sit on.
    // Iterate layouts back-to-front so a pin belonging to the topmost (last-drawn) node wins.
    const f32 pinHitR = style.pinHitRadiusPx * scale;
    for (auto it = layouts.rbegin(); it != layouts.rend(); ++it) {
        for (const auto& pl : it->pins) {
            if (dist(canvasPoint, pl.pos) <= pinHitR) {
                GraphHitResult r;
                r.kind = GraphHitKind::Pin;
                r.nodeId = it->nodeId;
                r.pinName = pl.name;
                r.pinIsOutput = pl.isOutput;
                return r;
            }
        }
    }

    // Node bodies next, also back-to-front.
    for (auto it = layouts.rbegin(); it != layouts.rend(); ++it) {
        if (canvasPoint.x >= it->min.x && canvasPoint.x <= it->max.x &&
            canvasPoint.y >= it->min.y && canvasPoint.y <= it->max.y) {
            GraphHitResult r;
            r.kind = GraphHitKind::Node;
            r.nodeId = it->nodeId;
            return r;
        }
    }

    // Links last: sample each link's bezier and take the nearest segment distance.
    const f32 linkHitR = style.linkHitDistancePx * scale;
    std::vector<Vec2> samples;
    for (usize i = 0; i < graph.links.size(); ++i) {
        const auto& link = graph.links[i];
        const GraphPinLayout* sp = findPinLayout(layouts, link.sourceNode, link.sourcePin);
        const GraphPinLayout* dp = findPinLayout(layouts, link.destNode, link.destPin);
        if (!sp || !dp) continue;
        const GraphBezier b = linkBezier(sp->pos, dp->pos);
        sampleCubicBezier(b, 24, samples);
        for (usize s = 0; s + 1 < samples.size(); ++s) {
            if (pointSegmentDist(canvasPoint, samples[s], samples[s + 1]) <= linkHitR) {
                GraphHitResult r;
                r.kind = GraphHitKind::Link;
                r.linkIndex = static_cast<isize>(i);
                return r;
            }
        }
    }

    return GraphHitResult{};
}

// ---------------------------------------------------------------------------------------------------
// Link rules
// ---------------------------------------------------------------------------------------------------

bool wouldCreateCycle(const fmt::OcGraphData& graph, const std::string& sourceNode, const std::string& destNode) {
    if (sourceNode == destNode) return true;
    std::unordered_set<std::string> visited;
    std::vector<std::string> stack{destNode};
    visited.insert(destNode);
    while (!stack.empty()) {
        const std::string cur = stack.back();
        stack.pop_back();
        if (cur == sourceNode) return true;
        for (const auto& link : graph.links) {
            if (link.sourceNode == cur && visited.insert(link.destNode).second) {
                stack.push_back(link.destNode);
            }
        }
    }
    return false;
}

GraphLinkCheck canConnectPins(const fmt::OcGraphData& graph,
                               const std::string& sourceNode, const std::string& sourcePin,
                               const std::string& destNode, const std::string& destPin) {
    GraphLinkCheck r;

    const fmt::OcGraphNode* sn = findNode(graph, sourceNode);
    if (!sn) { r.reason = GraphLinkReject::UnknownPin; r.message = "source node '" + sourceNode + "' does not exist"; return r; }
    const fmt::OcGraphNode* dn = findNode(graph, destNode);
    if (!dn) { r.reason = GraphLinkReject::UnknownPin; r.message = "dest node '" + destNode + "' does not exist"; return r; }

    const fmt::OcGraphPin* sp = findPin(*sn, sourcePin);
    if (!sp) { r.reason = GraphLinkReject::UnknownPin; r.message = "source pin '" + sourceNode + "." + sourcePin + "' does not exist"; return r; }
    const fmt::OcGraphPin* dp = findPin(*dn, destPin);
    if (!dp) { r.reason = GraphLinkReject::UnknownPin; r.message = "dest pin '" + destNode + "." + destPin + "' does not exist"; return r; }

    if (!sp->isOutput) {
        r.reason = GraphLinkReject::SourceNotOutput;
        r.message = "source pin '" + sourceNode + "." + sourcePin + "' is an input, not an output";
        return r;
    }
    if (dp->isOutput) {
        r.reason = GraphLinkReject::DestNotInput;
        r.message = "dest pin '" + destNode + "." + destPin + "' is an output, not an input";
        return r;
    }
    if (sourceNode == destNode) {
        r.reason = GraphLinkReject::SelfConnection;
        r.message = "node '" + sourceNode + "' cannot connect to itself";
        return r;
    }
    for (const auto& link : graph.links) {
        if (link.destNode == destNode && link.destPin == destPin) {
            r.reason = GraphLinkReject::DestOccupied;
            r.message = "dest pin '" + destNode + "." + destPin + "' already has an incoming link from '"
                        + link.sourceNode + "." + link.sourcePin + "'";
            return r;
        }
    }
    if (!ciEquals(sp->type, dp->type)) {
        r.reason = GraphLinkReject::TypeMismatch;
        r.message = "type mismatch: '" + sourceNode + "." + sourcePin + "' is " + sp->type
                    + ", '" + destNode + "." + destPin + "' is " + dp->type;
        return r;
    }
    if (wouldCreateCycle(graph, sourceNode, destNode)) {
        r.reason = GraphLinkReject::WouldCreateCycle;
        r.message = "link would create a cycle: '" + destNode + "' can already reach '" + sourceNode + "'";
        return r;
    }

    r.ok = true;
    return r;
}

GraphLinkCheck tryAddLink(fmt::OcGraphData& graph,
                           const std::string& sourceNode, const std::string& sourcePin,
                           const std::string& destNode, const std::string& destPin) {
    GraphLinkCheck check = canConnectPins(graph, sourceNode, sourcePin, destNode, destPin);
    if (check.ok) {
        fmt::OcGraphLink link;
        link.sourceNode = sourceNode;
        link.sourcePin = sourcePin;
        link.destNode = destNode;
        link.destPin = destPin;
        graph.links.push_back(link);
    }
    return check;
}

// ---------------------------------------------------------------------------------------------------
// Canvas transform
// ---------------------------------------------------------------------------------------------------

Vec2 canvasToScreen(const CanvasTransform& t, Vec2 canvasPt) {
    return Vec2(canvasPt.x * t.zoom + t.panPx.x, canvasPt.y * t.zoom + t.panPx.y);
}

Vec2 screenToCanvas(const CanvasTransform& t, Vec2 screenPt) {
    return Vec2((screenPt.x - t.panPx.x) / t.zoom, (screenPt.y - t.panPx.y) / t.zoom);
}

CanvasTransform zoomAroundScreenPoint(const CanvasTransform& t, f32 newZoom, Vec2 screenPivot) {
    const Vec2 canvasUnderPivot = screenToCanvas(t, screenPivot);
    CanvasTransform out;
    out.zoom = newZoom;
    // Solve panPx so that canvasToScreen(out, canvasUnderPivot) == screenPivot exactly.
    out.panPx = Vec2(screenPivot.x - canvasUnderPivot.x * newZoom, screenPivot.y - canvasUnderPivot.y * newZoom);
    return out;
}

// ---------------------------------------------------------------------------------------------------
// Auto-layout
// ---------------------------------------------------------------------------------------------------

std::unordered_map<std::string, Vec2> autoLayoutPositions(const fmt::OcGraphData& graph,
                                                            const GraphLayoutStyle& style, f32 scale) {
    std::unordered_map<std::string, Vec2> out;
    if (graph.nodes.empty()) return out;

    // Kahn's-algorithm layering. `inDegree` counts remaining incoming links per node; a node joins the
    // current layer once its in-degree drops to zero, and every link leaving this layer's nodes then
    // decrements its target's count for the next pass. Guarded by `guard` (one pass budget per node) so
    // a cycle in a hand-written file -- the editor's own connect rules refuse creating one, but cannot
    // stop a hand-edited file from containing one -- cannot loop forever: if a pass finds nothing with
    // zero in-degree, every remaining node is dumped into one final layer instead.
    std::unordered_map<std::string, int> inDegree;
    for (const auto& n : graph.nodes) inDegree[n.id] = 0;
    for (const auto& l : graph.links) {
        auto it = inDegree.find(l.destNode);
        if (it != inDegree.end()) ++it->second;
    }

    std::vector<std::vector<std::string>> layers;
    std::unordered_set<std::string> placed;
    usize guard = graph.nodes.size() + 1;
    while (placed.size() < graph.nodes.size() && guard-- > 0) {
        std::vector<std::string> layer;
        for (const auto& n : graph.nodes) {
            if (placed.count(n.id)) continue;
            if (inDegree[n.id] <= 0) layer.push_back(n.id);
        }
        if (layer.empty()) {
            // Cycle remnant: everything left over goes in one final layer so the loop terminates.
            for (const auto& n : graph.nodes)
                if (!placed.count(n.id)) layer.push_back(n.id);
        }
        for (const auto& id : layer) placed.insert(id);
        for (const auto& l : graph.links) {
            if (inDegree.count(l.destNode) &&
                std::find(layer.begin(), layer.end(), l.sourceNode) != layer.end()) {
                --inDegree[l.destNode];
            }
        }
        layers.push_back(std::move(layer));
    }

    const f32 colGapPx = 60.0f * scale;
    const f32 rowGapPx = 24.0f * scale;
    f32 x = 0.0f;
    for (const auto& layer : layers) {
        f32 layerWidth = style.minWidthPx * scale;
        f32 y = 0.0f;
        for (const auto& id : layer) {
            const fmt::OcGraphNode* node = findNode(graph, id);
            if (!node) continue;
            const GraphNodeLayout gl = computeNodeLayout(*node, node->type, style, scale);
            const f32 w = gl.max.x - gl.min.x;
            const f32 h = gl.max.y - gl.min.y;
            layerWidth = std::max(layerWidth, w);
            out[id] = Vec2(x, y);
            y += h + rowGapPx;
        }
        x += layerWidth + colGapPx;
    }
    return out;
}

} // namespace aver::editor
