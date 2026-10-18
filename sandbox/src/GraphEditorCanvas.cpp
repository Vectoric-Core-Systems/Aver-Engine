// The .ocgraph editor's Event Graph tab: the node canvas, its popups and the details column (drawEventGraph).
// Split from GraphEditor.cpp, which crossed the COFF section cap in Debug. Same class, same includes.
#include "EditorTransform.hpp"
#include "aver/core/Types.hpp"   // AVER_PATH_SEP
#include "EditorKeybinds.hpp"
#include "EditorPrefs.hpp"
#include "GraphEditor.hpp"
#include "GraphNodeDefs.hpp"
#if AVER_MODULE_FLUIDS
#  include "aver/fluids/FluidVolume.hpp"   // the seed shell a Fluid component previews
#endif
#if AVER_WITH_IMGUI
// Viewport tab only; guarded because tests/editor compiles this file with no ImGui/preview module and a three-library link (tests/editor/CMakeLists.txt).
#  include "ActorEditor.hpp"   // sharedPreview / sharedPreviewMeshes / actorEditorContentRoot
#endif

#include "aver/core/Log.hpp"
#include "aver/formats/OcInput.hpp"       // loadOcinput -- refreshSchemeActionsIfNeeded's action= picker
#include "aver/formats/OcProject.hpp"     // loadOcproject -- resolves INPUT.SCHEME for the same picker
#include "aver/platform/FileSystem.hpp"   // writeFileTextAtomic -- see save() below
#if AVER_WITH_IMGUI
#  include "aver/runtime/Engine.hpp"
#  include "aver/render/preview/ActorPreview.hpp"
#  include "aver/render/preview/PreviewMeshCache.hpp"
#  include "PreviewChrome.hpp"   // drawPreviewToolbar/drawPreviewStats/drawPreviewAxes/formatCount
#  if AVER_MODULE_PBR
// Material-graph registry: a material graph's Viewport preview shades via the same compiled graph the renderer uses, so they can't drift.
// Nested inside the ImGui guard too, since tests/editor compiles this file with neither.
#    include "aver/pbr/MaterialGraphRegistry.hpp"
#  endif
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#  include "imgui_internal.h"   // GImGui/ImGuiLastItemData -- see drawMaterialShapeSelector below
#endif

#include "GraphEditorInternal.hpp"

namespace aver::editor {

using namespace gedetail;

// The Event Graph tab: the node canvas and its details column. Lifted out of draw() unchanged when
// the Viewport tab arrived, so draw() is now just the tab bar -- the only way either tab's body stays
// readable.
void GraphEditor::drawEventGraph(float dpi) {
    // Which nodes are running, refreshed once a frame. Cheap: the managed side keys its table by
    // graph NAME, so this asks only about this canvas's graph, and recording costs a static bool test
    // when off. SandboxApp arms it ONCE at editor start, deliberately (not per-tab, cheaper than
    // tracking tab lifetimes); a packaged game never arms it.
    //
    // Cleared when there is no source, so a set captured during Play doesn't keep glowing after Play
    // stops.
    if (nodeHits_) {
        std::vector<std::pair<std::string, f32>> hits;
        nodeHits_(graph_.name, kNodeHitFadeSec, hits);
        nodeHitAges_.clear();
        for (auto& h : hits) nodeHitAges_.emplace(std::move(h.first), h.second);
    } else if (!nodeHitAges_.empty()) {
        nodeHitAges_.clear();
    }

#if AVER_WITH_IMGUI
    tickDiagnostics();

    // The debugger's values for this graph, ten times a second at most (each poll is a call into the
    // managed side). Dropped when the debugger is disarmed so a stale label cannot outlive its session.
    if (debugSession().armed && debugValuesSrc_) {
        const double now = ImGui::GetTime();
        if (now - lastValuesPollSec_ > 0.1) {
            lastValuesPollSec_ = now;
            debugValues_.update(debugValuesSrc_(debugGraphName()), now);
            debugValues_.expire(now, 10.0);
        }
    } else if (debugValues_.size() != 0) {
        debugValues_.clear();
    }

    recomputeLayouts(dpi);

    // The canvas gives up a right-hand strip for the details panel (Gap B), draggable and persisted via
    // splitterHandle/clampSplitWidth (EditorWidgets.hpp; DPI convention: kDefaultDetailsColumn's comment).
    // Downstream canvas math reads GetContentRegionAvail() after BeginChild, so it sees the narrowed
    // region automatically.
    //
    // Before: `clamp(260*dpi, 180*dpi, max(avail.x*0.45f, 120*dpi))`, recomputed every frame -- a smooth
    // proportional squeeze. Now: detailsColW_ persists and clampSplitWidth only pushes it past two fixed
    // floors (minDetails 180*dpi, minCanvas 40*dpi), holding steady between them ("minSelf wins" favors
    // the details floor when both would bind).
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float minDetails = 180.0f * dpi, minCanvas = 40.0f * dpi;
    if (detailsColW_ <= 0.0f) detailsColW_ = prefFloat(kPrefDetailsColumn, kDefaultDetailsColumn) * dpi;
    detailsColW_ = clampSplitWidth(detailsColW_, avail.x, minDetails, minCanvas);
    const float detailsW = detailsColW_;
    const float canvasW = std::max(avail.x - detailsW - ImGui::GetStyle().ItemSpacing.x, minCanvas);
    // Problems / References live under the canvas, so canvas and panel are one group the splitter and
    // the details column sit beside.
    const float bottomH = bottomOpen_ ? std::min(170.0f * dpi, std::max(avail.y * 0.4f, 70.0f * dpi)) : 0.0f;
    ImGui::BeginGroup();
    ImGui::BeginChild("##graphCanvas",
                       ImVec2(canvasW, std::max(avail.y - bottomH - (bottomH > 0.0f ? ImGui::GetStyle().ItemSpacing.y : 0.0f), 80.0f * dpi)),
                       true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const ImVec2 originIm = ImGui::GetCursorScreenPos();
    const Vec2 origin = fromIm(originIm);
    const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##graphCanvasHit", ImVec2(std::max(canvasSize.x, 1.0f), std::max(canvasSize.y, 1.0f)),
                            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                            ImGuiButtonFlags_MouseButtonMiddle);
    const bool hoveredRaw = ImGui::IsItemHovered();
    // The find bar floats over the canvas; a click on it is not a click on the graph under it.
    const ImVec2 mouseNow = ImGui::GetIO().MousePos;
    const bool overFindBar = findOpen_ && mouseNow.x >= findBarMin_.x && mouseNow.x <= findBarMax_.x &&
                             mouseNow.y >= findBarMin_.y && mouseNow.y <= findBarMax_.y;
    const bool hovered = hoveredRaw && !overFindBar;
    const bool canvasFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) || hovered;
    if (canvasFocused) g_graphCanvasFocusFrame = ImGui::GetFrameCount();

    ImGuiIO& io = ImGui::GetIO();
    const Vec2 mouseScreen = fromIm(io.MousePos) - origin; // canvas-local screen space
    const Vec2 mouseCanvas = screenToCanvas(view_, mouseScreen);

    auto toScreenAbs = [&](Vec2 canvasPt) -> ImVec2 {
        const Vec2 s = canvasToScreen(view_, canvasPt);
        return ImVec2(originIm.x + s.x, originIm.y + s.y);
    };

    // Reframes on unframed content (pendingFrame_) or a canvas resize, but only until the author pans
    // or zooms (viewTouched_) -- "the view is mine until you touch it". One-shot-on-open and "wait two
    // frames" were both tried and both measured a stale 1853px canvas against a 2670px settled window;
    // tracking viewTouched_ instead sidesteps guessing when layout is final, and fixes window-resize
    // and the details-panel split for free.
    const bool canvasResized = canvasSize.x != lastCanvasSizePx_.x || canvasSize.y != lastCanvasSizePx_.y;
    if (framePendingFromToolbar_ || (!viewTouched_ && (pendingFrame_ || canvasResized))) {
        frameAll(Vec2{canvasSize.x, canvasSize.y}, dpi);
        pendingFrame_ = false;
        framePendingFromToolbar_ = false;
    }
    lastCanvasSizePx_ = Vec2{canvasSize.x, canvasSize.y};
    // A revealed node (Problems click, Find, a breakpoint) is framed here, after the frame block above
    // has had its say, and the view is then the author's until they move it.
    if (revealPending_) {
        frameSelection(Vec2{canvasSize.x, canvasSize.y}, dpi);
        revealPending_ = false;
        viewTouched_ = true;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(originIm, ImVec2(originIm.x + canvasSize.x, originIm.y + canvasSize.y), IM_COL32(30, 30, 34, 255));
    {
        const f32 gridStep = 64.0f * dpi * view_.zoom;
        if (gridStep > 6.0f) { // don't draw a fog of lines when zoomed far out
            const f32 startX = std::fmod(view_.panPx.x, gridStep);
            const f32 startY = std::fmod(view_.panPx.y, gridStep);
            for (f32 x = startX; x < canvasSize.x; x += gridStep)
                dl->AddLine(ImVec2(originIm.x + x, originIm.y), ImVec2(originIm.x + x, originIm.y + canvasSize.y), IM_COL32(255, 255, 255, 12));
            for (f32 y = startY; y < canvasSize.y; y += gridStep)
                dl->AddLine(ImVec2(originIm.x, originIm.y + y), ImVec2(originIm.x + canvasSize.x, originIm.y + y), IM_COL32(255, 255, 255, 12));
        }
    }

    if (hovered && dragMode_ == DragMode::None) {
        const bool spacePan = io.KeyShift == false && ImGui::IsKeyDown(ImGuiKey_Space);
        if ((spacePan && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            dragMode_ = DragMode::PanCanvas;
            rightButtonPan_ = false;
            dragStartScreen_ = mouseScreen;
            panAnchorPx_ = view_.panPx;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            // Right button is ambiguous at mouse-down (click opens Add Node, drag pans): start panning
            // immediately (same zero-latency feel as Middle-drag) and stash the press position/canvas
            // point (pendingSpawnCanvasPos_) so release decides which gesture it was (see the PanCanvas
            // release case below).
            dragMode_ = DragMode::PanCanvas;
            rightButtonPan_ = true;
            dragStartScreen_ = mouseScreen;
            panAnchorPx_ = view_.panPx;
            pendingSpawnCanvasPos_ = mouseCanvas;

            // Decided at press, not release: the hit test needs the canvas position under the cursor
            // at press time, since the view may have panned by release. Right-clicking a node used to
            // open Add Node like empty space -- no context menu existed anywhere in this editor, so
            // copy/cut/paste/duplicate/delete/break-links were keyboard-only and undiscoverable.
            const GraphHitResult rhit = hitTest(graph_, layouts_, mouseCanvas, style_, dpi);
            rightClickNode_ = rhit.kind == GraphHitKind::Node ? rhit.nodeId : std::string{};
            rightClickLink_ = rhit.kind == GraphHitKind::Link ? static_cast<int>(rhit.linkIndex) : -1;
            // Selects what was right-clicked unless already selected, so right-clicking one of several
            // selected nodes acts on all of them; right-clicking outside the selection retargets to it.
            if (!rightClickNode_.empty()) {
                const bool already = std::find(selectedNodes_.begin(), selectedNodes_.end(),
                                               rightClickNode_) != selectedNodes_.end();
                if (!already) { selectedNodes_ = {rightClickNode_}; selectedLink_ = -1; }
                selectedComment_.clear();
            } else if (rightClickLink_ >= 0) {
                selectedNodes_.clear();
                selectedLink_ = rightClickLink_;
                selectedComment_.clear();
            }
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const GraphHitResult hit = hitTest(graph_, layouts_, mouseCanvas, style_, dpi);
            const bool ctrl = io.KeyCtrl;
            if (hit.kind != GraphHitKind::None) selectedComment_.clear();
            if (hit.kind == GraphHitKind::Pin) {
                dragMode_ = DragMode::DrawLink;
                linkDragFromNode_ = hit.nodeId;
                linkDragFromPin_ = hit.pinName;
                linkDragFromIsOutput_ = hit.pinIsOutput;
            } else if (hit.kind == GraphHitKind::Node) {
                const bool already = std::find(selectedNodes_.begin(), selectedNodes_.end(), hit.nodeId) != selectedNodes_.end();
                if (ctrl) {
                    if (already) selectedNodes_.erase(std::remove(selectedNodes_.begin(), selectedNodes_.end(), hit.nodeId), selectedNodes_.end());
                    else selectedNodes_.push_back(hit.nodeId);
                } else if (!already) {
                    selectedNodes_ = {hit.nodeId};
                }
                selectedLink_ = -1;
                dragMode_ = DragMode::MoveNodes;
                dragStartCanvas_ = mouseCanvas;
                moveStart_.clear();
                for (const auto& id : selectedNodes_) {
                    auto it = displayPos_.find(id);
                    moveStart_[id] = it != displayPos_.end() ? it->second : Vec2{};
                }
                pendingMoveSnapshot_ = UndoState{graph_, displayPos_};
                moveUndoPushed_ = false;
            } else if (hit.kind == GraphHitKind::Link) {
                if (!ctrl) selectedNodes_.clear();
                selectedLink_ = static_cast<int>(hit.linkIndex);
            } else {
                // Comment boxes are tested last: they draw behind everything, so anything drawn on top
                // (e.g. a node's title bar) owns the click; testing first would make those unclickable.
                bool onGrip = false;
                const std::string cid = commentAtCanvas(mouseCanvas, dpi, &onGrip);
                if (!cid.empty()) {
                    selectedNodes_.clear();
                    selectedLink_ = -1;
                    selectedComment_ = cid;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !onGrip) {
                        // Double-click the bar opens the properties popup (title, colour, delete);
                        // queued because BeginPopup for it is submitted later this frame, after drawing.
                        commentEditId_ = cid;
                        const fmt::OcGraphComment* c = nullptr;
                        for (const auto& k : graph_.comments) if (k.id == cid) { c = &k; break; }
                        std::snprintf(commentEditBuf_, sizeof commentEditBuf_, "%s", c ? c->text.c_str() : "");
                        commentPopupQueued_ = true;
                    } else {
                        const fmt::OcGraphComment* c = nullptr;
                        for (const auto& k : graph_.comments) if (k.id == cid) { c = &k; break; }
                        if (c) {
                            dragMode_ = onGrip ? DragMode::ResizeComment : DragMode::MoveComment;
                            activeComment_ = cid;
                            dragStartCanvas_ = mouseCanvas;
                            commentDragStartPos_ = Vec2{static_cast<f32>(c->x), static_cast<f32>(c->y)};
                            commentDragStartSize_ = Vec2{static_cast<f32>(c->w), static_cast<f32>(c->h)};
                            commentCapturedStart_.clear();
                            if (dragMode_ == DragMode::MoveComment) {
                                for (const auto& nid : nodesInsideComment(cid, dpi)) {
                                    auto it = displayPos_.find(nid);
                                    commentCapturedStart_[nid] = it != displayPos_.end() ? it->second : Vec2{};
                                }
                            }
                            pendingCommentSnapshot_ = UndoState{graph_, displayPos_};
                            commentUndoPushed_ = false;
                        }
                    }
                } else {
                    if (!ctrl) { selectedNodes_.clear(); selectedLink_ = -1; }
                    selectedComment_.clear();
                    dragMode_ = DragMode::BoxSelect;
                    dragStartCanvas_ = mouseCanvas;
                    boxSelectCurrentCanvas_ = mouseCanvas;
                }
            }
        }
    }

    switch (dragMode_) {
    case DragMode::PanCanvas: {
        const bool stillDown = ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
                                ImGui::IsMouseDown(ImGuiMouseButton_Right);
        if (stillDown) {
            const Vec2 moved = mouseScreen - dragStartScreen_;
            if (vecLen(moved) > 0.0f) viewTouched_ = true;
            view_.panPx = panAnchorPx_ + moved;
        } else {
            // Right button only: resolve click-vs-drag now the button is up. A few px of threshold
            // absorbs mouse jitter (else every click would measure nonzero travel and never open the
            // popup); dpi-scaled, unlike MoveNodes's bare 3.0f below, since raw device pixels need it --
            // at 200% DPI the same hand-jitter covers twice as many, and unscaled it would grow harder
            // to summon the popup as DPI rises.
            constexpr f32 kRightClickDragThresholdPx = 4.0f;
            if (rightButtonPan_) {
                const f32 travelled = vecLen(mouseScreen - dragStartScreen_);
                if (travelled <= kRightClickDragThresholdPx * dpi) {
                    view_.panPx = panAnchorPx_; // a click must pan by exactly zero, not by a few stray px
                    // On a node or a link: the context menu; on empty canvas: Add Node.
                    if (!rightClickNode_.empty() || rightClickLink_ >= 0)
                        ImGui::OpenPopup("##graphNodeMenu");
                    else
                        ImGui::OpenPopup("##graphAddNode"); // pendingSpawnCanvasPos_ was set at press time
                }
                // else: a real drag happened. The pan already applied above stays; no popup.
            }
            dragMode_ = DragMode::None;
        }
        break;
    }
    case DragMode::MoveNodes: {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const Vec2 delta = mouseCanvas - dragStartCanvas_;
            if (!moveUndoPushed_ && vecLen(delta) * view_.zoom > 3.0f) {
                history_.push(pendingMoveSnapshot_);
                moveUndoPushed_ = true;
            }
            for (const auto& id : selectedNodes_) displayPos_[id] = moveStart_[id] + delta;
        } else {
            if (moveUndoPushed_) {
                for (const auto& id : selectedNodes_) {
                    for (auto& n : graph_.nodes) {
                        if (n.id == id) {
                            const Vec2 p = displayPos_[id];
                            n.x = p.x; n.y = p.y;
                            break;
                        }
                    }
                }
                dirty_ = true;
            }
            dragMode_ = DragMode::None;
        }
        break;
    }
    case DragMode::BoxSelect: {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            boxSelectCurrentCanvas_ = mouseCanvas;
        } else {
            const Vec2 lo(std::min(dragStartCanvas_.x, boxSelectCurrentCanvas_.x), std::min(dragStartCanvas_.y, boxSelectCurrentCanvas_.y));
            const Vec2 hi(std::max(dragStartCanvas_.x, boxSelectCurrentCanvas_.x), std::max(dragStartCanvas_.y, boxSelectCurrentCanvas_.y));
            for (const auto& nl : layouts_) {
                const bool intersects = nl.min.x <= hi.x && nl.max.x >= lo.x && nl.min.y <= hi.y && nl.max.y >= lo.y;
                if (intersects && std::find(selectedNodes_.begin(), selectedNodes_.end(), nl.nodeId) == selectedNodes_.end())
                    selectedNodes_.push_back(nl.nodeId);
            }
            dragMode_ = DragMode::None;
        }
        break;
    }
    case DragMode::DrawLink: {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const GraphHitResult hit = hitTest(graph_, layouts_, mouseCanvas, style_, dpi);
            if (hit.kind == GraphHitKind::Pin && !(hit.nodeId == linkDragFromNode_ && hit.pinName == linkDragFromPin_)) {
                if (linkDragFromIsOutput_) commitLink(linkDragFromNode_, linkDragFromPin_, hit.nodeId, hit.pinName);
                else                       commitLink(hit.nodeId, hit.pinName, linkDragFromNode_, linkDragFromPin_);
            } else if (hit.kind == GraphHitKind::None) {
                // Dragging a wire into empty space opens the palette (filtered to types that can accept
                // this wire) and connects the pick -- the gesture Blueprint authors expect for building
                // a chain without pre-placing nodes. Only on truly empty canvas (hit.kind == None):
                // missing a node's pin does nothing, deliberately -- a second node on top would guess wrong.
                // Releasing here used to do nothing at all -- the wire simply vanished.
                beginLinkDrop(linkDragFromNode_, linkDragFromPin_, linkDragFromIsOutput_);
                if (linkDropPending_) {
                    pendingSpawnCanvasPos_ = mouseCanvas;
                    ImGui::OpenPopup("##graphAddNode");
                }
            }
            dragMode_ = DragMode::None;
        }
        break;
    }
    case DragMode::MoveComment:
    case DragMode::ResizeComment: {
        fmt::OcGraphComment* c = findComment(activeComment_);
        if (!c) { dragMode_ = DragMode::None; break; }
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const Vec2 delta = mouseCanvas - dragStartCanvas_;
            // Same lazy undo boundary MoveNodes uses, and the same 3px-of-travel threshold, so a
            // plain click to select a box does not push an identical state onto the stack.
            if (!commentUndoPushed_ && vecLen(delta) * view_.zoom > 3.0f) {
                history_.push(pendingCommentSnapshot_);
                commentUndoPushed_ = true;
            }
            if (dragMode_ == DragMode::MoveComment) {
                c->x = commentDragStartPos_.x + delta.x;
                c->y = commentDragStartPos_.y + delta.y;
                for (const auto& kv : commentCapturedStart_) displayPos_[kv.first] = kv.second + delta;
            } else {
                c->w = std::max(static_cast<f64>(commentDragStartSize_.x + delta.x), static_cast<f64>(kCommentMinPx));
                c->h = std::max(static_cast<f64>(commentDragStartSize_.y + delta.y), static_cast<f64>(kCommentMinPx));
            }
        } else {
            if (commentUndoPushed_) {
                // The nodes that rode along commit their new positions into graph_, exactly as the
                // end of a MoveNodes drag does -- displayPos_ alone is not saved.
                for (const auto& kv : commentCapturedStart_) {
                    for (auto& n : graph_.nodes) {
                        if (n.id != kv.first) continue;
                        const Vec2 p = displayPos_[kv.first];
                        n.x = p.x; n.y = p.y;
                        break;
                    }
                }
                dirty_ = true;
            }
            commentCapturedStart_.clear();
            dragMode_ = DragMode::None;
        }
        break;
    }
    case DragMode::None: default: break;
    }

    // Wheel zooms by default; Shift+wheel or MouseWheelH pans instead. MouseWheelH was unbound before;
    // Shift+wheel is repurposed from an ignored modifier (old handler zoomed the same with or without
    // Shift), so this loses no behavior and gives a mouse-only user a direct pan gesture; the bare-wheel
    // zoom path below is unchanged.
    if (hovered && io.KeyShift && io.MouseWheel != 0.0f) {
        // Sign matches "content scrolls like a document": wheel-up moves the CONTENT down (panPx.y
        // grows), as in a text editor -- not the "camera pans up" reading, which would flip the sign.
        constexpr f32 kWheelPanPxPerNotch = 60.0f;
        viewTouched_ = true;
        view_.panPx.y += io.MouseWheel * kWheelPanPxPerNotch * dpi;
    } else if (hovered && io.MouseWheel != 0.0f) {
        const f32 newZoom = std::clamp(view_.zoom * std::pow(1.1f, io.MouseWheel), minZoomForDpi(dpi), kMaxZoom);
        viewTouched_ = true;
        view_ = zoomAroundScreenPoint(view_, newZoom, mouseScreen);
    }
    if (hovered && io.MouseWheelH != 0.0f) {
        // Always horizontal pan, no modifier needed: this axis has no existing zoom meaning to collide
        // with (the code above reads only io.MouseWheel), so it is free in every state.
        constexpr f32 kWheelPanPxPerNotch = 60.0f;
        viewTouched_ = true;
        view_.panPx.x -= io.MouseWheelH * kWheelPanPxPerNotch * dpi;
    }

    // Not while a text field has the keyboard: Delete inside the find bar must edit the query, not the graph.
    if (canvasFocused && !io.WantTextInput) {
        // Ctrl+F and F9 are hardcoded like Ctrl+Shift+Z below: the registry has no command for either, and
        // F9 is the debugger's breakpoint here while the level view keeps it for the screenshot.
        if (io.KeyCtrl && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_F, false)) openFind();
        if (!io.KeyCtrl && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_F9, false)) toggleBreakpointOnSelection();

        // Through the registry (was nine hardcoded keys duplicating existing commands, so rebinding
        // Copy changed it everywhere except this canvas), so a Preferences rebind applies here too.
        // Delete/Undo/Redo/Copy/Paste/Duplicate/Frame Selected are the SAME commands as the level
        // viewport's (one "Copy", rebound once); Comment Box and Frame Everything are graph-only.
        auto& kb = editor::keybinds();
        using editor::CommandId;

        if (kb.pressed(CommandId::EditDelete, io)) deleteSelection();
        // C wraps the selection in a comment box (same key as Blueprint) -- hand-drawing a box around
        // several nodes and nudging its edges is enough work that nobody does it without this.
        if (kb.pressed(CommandId::GraphCommentBox, io) && !selectedNodes_.empty())
            addCommentAroundSelection(dpi);

        // Ctrl+Shift+Z stays hardcoded (matches SandboxApp's copy), not rebindable: it is an alternate
        // spelling of Redo folded into the undo branch (`if (io.KeyShift) redo()`), and EditUndo checks
        // Shift, so routing this through the registry would make it match nothing and go quietly dead --
        // one Redo drifting into two spellings is worse than one spelling that can't be rebound.
        if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)) redo();
        if (kb.pressed(CommandId::EditUndo, io)) undo();
        if (kb.pressed(CommandId::EditRedo, io)) redo();
        // Paste lands under the cursor: expected, and what makes pasting twice land the copies in
        // different places; a fixed offset would stack them.
        if (kb.pressed(CommandId::EditCopy, io)) copySelection();
        if (kb.pressed(CommandId::EditPaste, io)) pasteClipboard(mouseCanvas);
        if (kb.pressed(CommandId::EditDuplicate, io)) duplicateSelection();
        // F frames the selection (falling back to everything), Home always frames everything -- same
        // two bindings as Blueprint; "show me what I clicked" and "show me where I am" differ.
        if (kb.pressed(CommandId::ViewFrameSelected, io))
            frameSelection(Vec2{canvasSize.x, canvasSize.y}, dpi);
        if (kb.pressed(CommandId::GraphFrameAll, io)) frameAll(Vec2{canvasSize.x, canvasSize.y}, dpi);
    }

    // Draw order back-to-front: comment boxes, links, nodes -- via ImDrawListSplitter (imgui.h's own
    // recommendation), not submission order. Three channels, not two: a box must sit behind wires too,
    // or a wire crossing it would read as glass rather than scenery.
    dl->ChannelsSplit(3);

    // Which pins are wired, built once per frame (not searched per pin, both link ends count): fills a
    // connected pin, leaves an unconnected one hollow, so an unwired exec input no longer looks wired.
    //
    // Text scales with zoom, stopping once unreadable (labels used to render at fixed size, stacking
    // sixty full labels on a 63-node graph zoomed to ~0.2 at 300% DPI -- a type bug that looked like a
    // layout bug). Two thresholds: pin labels (5x as many, the ones that collide) disappear first;
    // titles survive further out, since overview zoom only needs to show what the shapes ARE.
    const float uiFontPx     = ImGui::GetFontSize();
    const float scaledFontPx = uiFontPx * view_.zoom;
    const bool  showPinText  = scaledFontPx >= 7.0f;
    const bool  showTitles   = scaledFontPx >= 4.5f;
    ImFont* const font       = ImGui::GetFont();

    std::set<std::pair<std::string, std::string>> connectedPins;
    for (const auto& link : graph_.links) {
        connectedPins.emplace(link.sourceNode, link.sourcePin);
        connectedPins.emplace(link.destNode, link.destPin);
    }

    dl->ChannelsSetCurrent(0); // comment boxes, behind even the wires
    for (const auto& c : graph_.comments) {
        const ImVec2 pMin = toScreenAbs(Vec2{static_cast<f32>(c.x), static_cast<f32>(c.y)});
        const ImVec2 pMax = toScreenAbs(Vec2{static_cast<f32>(c.x + c.w), static_cast<f32>(c.y + c.h)});
        const f32 barH = kCommentBarPx * dpi * view_.zoom;
        const bool sel = (c.id == selectedComment_);
        const int r = std::clamp(c.r, 0, 255), g = std::clamp(c.g, 0, 255), b = std::clamp(c.b, 0, 255);
        // Body alpha 40: low enough that the grid and any wire crossing the box stay readable through
        // it, high enough that the region still reads as one thing from across a zoomed-out canvas.
        dl->AddRectFilled(pMin, pMax, IM_COL32(r, g, b, 40), 4.0f * dpi);
        dl->AddRectFilled(pMin, ImVec2(pMax.x, pMin.y + barH), IM_COL32(r, g, b, 200), 4.0f * dpi,
                          ImDrawFlags_RoundCornersTop);
        dl->AddRect(pMin, pMax, sel ? kSelectionCol : IM_COL32(r, g, b, 220), 4.0f * dpi, 0,
                    (sel ? 2.5f : 1.5f) * dpi);
        // The resize grip (two strokes, bottom-right) is drawn only; its hit test lives in
        // commentAtCanvas in canvas units, so the two agree at every zoom without measuring each other.
        const f32 grip = kCommentGripPx * dpi * view_.zoom;
        if (grip > 4.0f) {
            const ImU32 gc = IM_COL32(255, 255, 255, sel ? 200 : 110);
            dl->AddLine(ImVec2(pMax.x - grip, pMax.y - 2.0f * dpi), ImVec2(pMax.x - 2.0f * dpi, pMax.y - grip), gc, 1.5f * dpi);
            dl->AddLine(ImVec2(pMax.x - grip * 0.5f, pMax.y - 2.0f * dpi), ImVec2(pMax.x - 2.0f * dpi, pMax.y - grip * 0.5f), gc, 1.5f * dpi);
        }
        // A comment title is a navigation landmark, so it gets a floor node labels don't: it shrinks
        // with the box but never below readable size; the clip rect keeps an oversized title in its bar.
        const float cmtFontPx = std::max(uiFontPx * view_.zoom, 11.0f);
        if (!c.text.empty() && barH > 3.0f) {
            dl->PushClipRect(pMin, ImVec2(pMax.x, pMin.y + barH), true);
            dl->AddText(font, cmtFontPx, ImVec2(pMin.x + 6.0f * dpi * view_.zoom, pMin.y + 2.0f * dpi * view_.zoom),
                         IM_COL32(255, 255, 255, 255), c.text.c_str());
            dl->PopClipRect();
        }
    }

    dl->ChannelsSetCurrent(1); // links
    for (usize i = 0; i < graph_.links.size(); ++i) {
        const auto& link = graph_.links[i];
        const GraphPinLayout* sp = findPinLayout(layouts_, link.sourceNode, link.sourcePin);
        const GraphPinLayout* dp = findPinLayout(layouts_, link.destNode, link.destPin);
        if (!sp || !dp) continue; // a link to a pin this file's PIN records never declared; draw nothing rather than guess
        const GraphBezier b = linkBezier(sp->pos, dp->pos);
        const bool selected = (static_cast<int>(i) == selectedLink_);
        const ImU32 col = selected ? kSelectionCol : colorForType(sp->type);
        // Exec wires draw HEAVIER than data wires: the exec chain is the spine (the order things
        // happen in), everything else is an argument being fetched. Weight tells them apart from
        // across the canvas, at a distance where colour is still legible but shape is not.
        const f32 baseWidth = isExecPinType(sp->type) ? 3.0f : 2.0f;
        dl->AddBezierCubic(toScreenAbs(b.p1), toScreenAbs(b.p2), toScreenAbs(b.p3), toScreenAbs(b.p4),
                            col, (selected ? baseWidth + 1.0f : baseWidth) * dpi, 24);
        // Debugger armed: the value this wire last carried, at its midpoint (dimmer once it stops changing).
        if (debugSession().armed && showPinText) {
            if (const DebugValue* dv = debugValues_.find(link.destNode, link.destPin)) {
                const std::string txt = formatDebugValue(dv->value, dp->type);
                const ImVec2 mid = toScreenAbs(Vec2{(b.p1.x + 3.0f * b.p2.x + 3.0f * b.p3.x + b.p4.x) * 0.125f,
                                                    (b.p1.y + 3.0f * b.p2.y + 3.0f * b.p3.y + b.p4.y) * 0.125f});
                const ImVec2 ts = font->CalcTextSizeA(scaledFontPx, FLT_MAX, 0.0f, txt.c_str());
                const bool old = DebugValues::stale(*dv, ImGui::GetTime());
                dl->AddRectFilled(ImVec2(mid.x - ts.x * 0.5f - 3.0f * dpi, mid.y - ts.y * 0.5f - 1.0f * dpi),
                                  ImVec2(mid.x + ts.x * 0.5f + 3.0f * dpi, mid.y + ts.y * 0.5f + 1.0f * dpi),
                                  IM_COL32(18, 22, 28, 230), 3.0f * dpi);
                dl->AddText(font, scaledFontPx, ImVec2(mid.x - ts.x * 0.5f, mid.y - ts.y * 0.5f),
                            old ? IM_COL32(150, 155, 160, 255) : IM_COL32(240, 240, 200, 255), txt.c_str());
            }
        }
    }

    dl->ChannelsSetCurrent(2); // nodes
    for (const auto& nl : layouts_) {
        const bool selected = std::find(selectedNodes_.begin(), selectedNodes_.end(), nl.nodeId) != selectedNodes_.end();
        const ImVec2 pMin = toScreenAbs(nl.min);
        const ImVec2 pMax = toScreenAbs(nl.max);
        const f32 headerH = style_.headerHeightPx * dpi * view_.zoom;

        // Descriptor resolved before the header draws, since the header now takes its colour from the
        // node's category (previously looked up only for the label, with every node the same slate-blue).
        const fmt::OcGraphNode* srcNode = nullptr;
        for (const auto& n : graph_.nodes) if (n.id == nl.nodeId) { srcNode = &n; break; }
        const GraphNodeDesc* desc = srcNode ? findGraphNodeDesc(srcNode->type) : nullptr;
        const ImU32 headerCol = srcNode ? nodeHeaderColor(*srcNode, desc, graph_.variables)
                                        : headerColorForCategory(std::string());

        // A soft drop shadow, offset down-right, drawn before anything else in the node -- makes it
        // read as sitting ABOVE the wire layer rather than punched out of it. Matters more here than
        // in most UIs: the canvas behind a node is a mesh of bright wires, not empty background.
        const f32 shadowOff = 3.0f * dpi;
        dl->AddRectFilled(ImVec2(pMin.x + shadowOff, pMin.y + shadowOff),
                          ImVec2(pMax.x + shadowOff, pMax.y + shadowOff),
                          IM_COL32(0, 0, 0, 90), 6.0f * dpi);

        // Body in the engine's own card colour, not generic dark grey, so a node reads as part of
        // this editor. Fully opaque, deliberately -- at the old alpha 240 (94%) a wire behind a node
        // showed through as a faint smear, exactly what the channel split above exists to prevent.
        dl->AddRectFilled(pMin, pMax, IM_COL32(21, 25, 32, 255), 5.0f * dpi);
        dl->AddRectFilled(pMin, ImVec2(pMax.x, pMin.y + headerH), headerCol, 5.0f * dpi, ImDrawFlags_RoundCornersTop);
        // A hairline under the header. Cheap, and it stops a dark body and a dark header from
        // reading as one block on the categories whose colour is already close to the body.
        dl->AddLine(ImVec2(pMin.x, pMin.y + headerH), ImVec2(pMax.x, pMin.y + headerH),
                    IM_COL32(0, 0, 0, 90), 1.0f * dpi);
        dl->AddRect(pMin, pMax, selected ? kSelectionCol : IM_COL32(12, 14, 18, 255), 5.0f * dpi, 0, selected ? 2.5f * dpi : 1.0f * dpi);
        // The node the validator named is ringed in amber, drawn OVER the selection ring (so a node
        // both selected and broken reads as both) and outside its rect (so it isn't mistaken for the
        // node's border). Only ever one node: the validator reports the first problem, not a list.
        if (!validateNode_.empty() && nl.nodeId == validateNode_) {
            const f32 pad = 3.0f * dpi;
            dl->AddRect(ImVec2(pMin.x - pad, pMin.y - pad), ImVec2(pMax.x + pad, pMax.y + pad),
                        IM_COL32(240, 150, 60, 255), 7.0f * dpi, 0, 2.5f * dpi);
        }
        // A green ring for a node that just ran, fading with age, drawn outside the validator's ring
        // (both show for a broken+executing node) and from real recorded hits, not an animation -- a
        // plausible glow that didn't match execution would be trusted and worse than none.
        if (const auto hit = nodeHitAges_.find(nl.nodeId); hit != nodeHitAges_.end()) {
            const f32 t = 1.0f - (hit->second / kNodeHitFadeSec);
            if (t > 0.0f) {
                const f32 pad = 6.0f * dpi;
                dl->AddRect(ImVec2(pMin.x - pad, pMin.y - pad), ImVec2(pMax.x + pad, pMax.y + pad),
                            IM_COL32(90, 230, 130, static_cast<int>(255.0f * (t < 1.0f ? t : 1.0f))),
                            9.0f * dpi, 0, 2.0f * dpi);
            }
        }
        // A problem on this node: a dot on its top-right corner, red for an error, amber for a warning.
        if (const auto badge = badges_.find(nl.nodeId); badge != badges_.end()) {
            const f32 br = 6.5f * dpi * std::max(view_.zoom, 0.6f);
            const ImVec2 at(pMax.x, pMin.y);
            dl->AddCircleFilled(at, br, badge->second.errors ? IM_COL32(215, 62, 55, 255) : IM_COL32(230, 180, 60, 255));
            dl->AddCircle(at, br, IM_COL32(20, 20, 24, 255), 0, 1.5f * dpi);
        }
        // A breakpoint: a red dot on the top-left corner, hollow when disabled. The node that just
        // broke pulses red, from the session's own record of the hit rather than an animation.
        if (hasBreakpoint(nl.nodeId)) {
            const f32 br = 6.5f * dpi * std::max(view_.zoom, 0.6f);
            const ImVec2 at(pMin.x, pMin.y);
            if (breakpointEnabled(nl.nodeId)) dl->AddCircleFilled(at, br, IM_COL32(220, 40, 40, 255));
            else                              dl->AddCircle(at, br, IM_COL32(220, 40, 40, 255), 0, 2.0f * dpi);
        }
        if (const auto& lb = debugSession().lastBreak; lb && lb->graph == debugGraphName() && lb->node == nl.nodeId) {
            const f32 pulse = 0.5f + 0.5f * std::sin(static_cast<f32>(ImGui::GetTime()) * 6.0f);
            const f32 pad = 8.0f * dpi;
            dl->AddRect(ImVec2(pMin.x - pad, pMin.y - pad), ImVec2(pMax.x + pad, pMax.y + pad),
                        IM_COL32(235, 60, 60, static_cast<int>(120 + 135 * pulse)), 9.0f * dpi, 0, 3.0f * dpi);
        }
        // Hover: what is wrong with this node, and (debugger armed) the value each input last held.
        if (hovered && ImGui::IsMouseHoveringRect(pMin, pMax)) {
            std::vector<std::string> lines;
            for (const GraphProblem& pr : problems_)
                if (pr.nodeId == nl.nodeId) lines.push_back((pr.severity == ProblemSeverity::Error ? "error: " : "warning: ") + pr.message);
            if (debugSession().armed) {
                for (const auto& pl : nl.pins) {
                    if (pl.isOutput) continue;
                    if (const DebugValue* dv = debugValues_.find(nl.nodeId, pl.name))
                        lines.push_back(pl.name + " = " + formatDebugValue(dv->value, pl.type));
                }
            }
            if (!lines.empty()) {
                ImGui::BeginTooltip();
                for (const std::string& ln : lines) ImGui::TextUnformatted(ln.c_str());
                ImGui::EndTooltip();
            }
        }
        if (showTitles) {
            const std::string label = srcNode ? nodeTitle(*srcNode, desc, graph_.variables) : nl.nodeId;
            dl->PushClipRect(pMin, ImVec2(pMax.x, pMin.y + headerH), true);
            dl->AddText(font, scaledFontPx, ImVec2(pMin.x + 6.0f * dpi * view_.zoom, pMin.y + 3.0f * dpi * view_.zoom),
                         IM_COL32(255, 255, 255, 255), label.c_str());
            dl->PopClipRect();
        }

        for (const auto& pl : nl.pins) {
            const ImVec2 dot = toScreenAbs(pl.pos);
            const f32 r = style_.pinRadiusPx * dpi * view_.zoom;
            const bool isLinkEnd = (dragMode_ == DragMode::DrawLink && nl.nodeId == linkDragFromNode_ && pl.name == linkDragFromPin_);
            const bool wired = connectedPins.count({nl.nodeId, pl.name}) != 0;
            const ImU32 pinCol = colorForType(pl.type);
            // Exec pins draw as an arrow, data pins as a circle -- shape, not just colour (colour alone
            // fails a colour-blind reader or a greyscale screenshot); the arrow points (a diamond
            // didn't), so the chain reads left-to-right without following a wire (Blueprint's shape,
            // same learned-language reason as the pin colours). Hollow means
            // unconnected either way: an unwired exec input never runs, an unwired data input falls
            // back to its default -- both were invisible before this.
            if (isExecPinType(pl.type)) {
                // Slightly narrower than tall, so it reads as an arrowhead rather than a wedge.
                const ImVec2 arrow[3] = {
                    ImVec2(dot.x - r * 0.85f, dot.y - r),
                    ImVec2(dot.x + r * 0.95f, dot.y),
                    ImVec2(dot.x - r * 0.85f, dot.y + r),
                };
                if (wired) dl->AddConvexPolyFilled(arrow, 3, pinCol);
                // Current AddPolyline signature (points, num_points, col, thickness, flags) (imgui.h:3527);
                // pre-1.92.7 took (col, flags, thickness) -- imconfig.h's shim links either order, this
                // uses the current one deliberately. Outline separates a white arrow from a white-ish
                // header when filled; unfilled, it IS the pin.
                dl->AddPolyline(arrow, 3, wired ? IM_COL32(40, 40, 40, 255) : pinCol,
                                (wired ? 1.0f : 2.0f) * dpi, ImDrawFlags_Closed);
            } else if (wired) {
                dl->AddCircleFilled(dot, r, pinCol);
            } else {
                // Ring, not disc. Drawn a hair inside r so the stroke's outer edge lands where the
                // filled version's edge does and a pin does not appear to grow when it is wired.
                dl->AddCircle(dot, r - 1.0f * dpi, pinCol, 0, 2.0f * dpi);
            }
            if (isLinkEnd) dl->AddCircle(dot, r + 2.0f * dpi, kSelectionCol, 0, 2.0f * dpi);
            if (!showPinText) continue;
            // Measured at the SCALED size, not the UI size, or an output label would be positioned
            // from a width it no longer has and would drift off its own node as you zoom out.
            const ImVec2 textSize = font->CalcTextSizeA(scaledFontPx, FLT_MAX, 0.0f, pl.name.c_str());
            const f32 tx = pl.isOutput ? dot.x - textSize.x - r - 3.0f * dpi : dot.x + r + 3.0f * dpi;
            dl->AddText(font, scaledFontPx, ImVec2(tx, dot.y - textSize.y * 0.5f),
                         IM_COL32(220, 220, 220, 255), pl.name.c_str());
        }
    }
    dl->ChannelsMerge();

    // ---- overlays: in-progress link drag, box-select rect, rejection banner (drawn AFTER merge, so
    // always on top regardless of channel) ----------------------------------------------------------
    if (dragMode_ == DragMode::DrawLink) {
        const GraphPinLayout* from = findPinLayout(layouts_, linkDragFromNode_, linkDragFromPin_);
        if (from) {
            const Vec2 a = linkDragFromIsOutput_ ? from->pos : mouseCanvas;
            const Vec2 b = linkDragFromIsOutput_ ? mouseCanvas : from->pos;
            const GraphBezier bez = linkBezier(a, b);
            dl->AddBezierCubic(toScreenAbs(bez.p1), toScreenAbs(bez.p2), toScreenAbs(bez.p3), toScreenAbs(bez.p4),
                                IM_COL32(255, 255, 255, 180), 2.0f * dpi, 24);
        }
    }
    if (dragMode_ == DragMode::BoxSelect) {
        const ImVec2 a = toScreenAbs(dragStartCanvas_);
        const ImVec2 b = toScreenAbs(boxSelectCurrentCanvas_);
        dl->AddRectFilled(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), IM_COL32(120, 160, 255, 40));
        dl->AddRect(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), IM_COL32(120, 160, 255, 200));
    }
    if (!lastRejectMsg_.empty() && (ImGui::GetTime() - lastRejectAtSec_) < 4.0) {
        // lastRejectMsg_ already carries its own full message (a link rejection embeds "Connection
        // refused: " itself; a blocked variable delete embeds its own text) -- see
        // reportLinkRejection's comment for why the prefix moved to the caller.
        const std::string& msg = lastRejectMsg_;
        dl->AddRectFilled(originIm, ImVec2(originIm.x + ImGui::CalcTextSize(msg.c_str()).x + 16.0f * dpi, originIm.y + 22.0f * dpi), IM_COL32(90, 25, 25, 220));
        dl->AddText(ImVec2(originIm.x + 8.0f * dpi, originIm.y + 4.0f * dpi), IM_COL32(255, 210, 210, 255), msg.c_str());
    }

    // ---- comment box properties: title, colour, delete. Opened by double-clicking a box's title bar
    // (see the hit-test above), which is where an author reaches for it first.
    if (commentPopupQueued_) { ImGui::OpenPopup("##graphCommentProps"); commentPopupQueued_ = false; }
    if (ImGui::BeginPopup("##graphCommentProps")) {
        fmt::OcGraphComment* c = findComment(commentEditId_);
        if (!c) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::TextUnformatted("Comment box");
            ImGui::Separator();
            ImGui::SetNextItemWidth(220.0f * dpi);
            // Same activate/apply-live/deactivate boundary the attribute and variable fields use:
            // graph_ is untouched while keys are landing, and setCommentText -- with its pushUndo --
            // fires once, when the field is left. Otherwise one typed title would be thirty undo steps.
            ImGui::InputText("Title", commentEditBuf_, sizeof commentEditBuf_);
            if (ImGui::IsItemDeactivatedAfterEdit()) setCommentText(commentEditId_, commentEditBuf_);
            float col[3] = {c->r / 255.0f, c->g / 255.0f, c->b / 255.0f};
            if (ImGui::ColorEdit3("Colour", col, ImGuiColorEditFlags_NoInputs)) {
                setCommentColor(commentEditId_, static_cast<int>(col[0] * 255.0f + 0.5f),
                                static_cast<int>(col[1] * 255.0f + 0.5f),
                                static_cast<int>(col[2] * 255.0f + 0.5f));
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete box")) {
                deleteComment(commentEditId_);
                commentEditId_.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    // ---- find bar (Ctrl+F): top-right, so it never covers the rejection banner on the left.
    if (findOpen_) {
        const float w = 340.0f * dpi, bh = 28.0f * dpi;
        const ImVec2 pos(originIm.x + std::max(canvasSize.x - w - 10.0f * dpi, 0.0f), originIm.y + 6.0f * dpi);
        findBarMin_ = fromIm(pos);
        findBarMax_ = Vec2{pos.x + w, pos.y + bh};
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + bh), IM_COL32(28, 32, 40, 240), 5.0f * dpi);
        dl->AddRect(pos, ImVec2(pos.x + w, pos.y + bh), IM_COL32(90, 100, 120, 255), 5.0f * dpi);
        ImGui::PushID("##findbar");
        ImGui::SetCursorScreenPos(ImVec2(pos.x + 4.0f * dpi, pos.y + 2.0f * dpi));
        ImGui::SetNextItemWidth(w - 128.0f * dpi);
        if (findFocusRequest_) { ImGui::SetKeyboardFocusHere(); findFocusRequest_ = false; }
        const bool enter = ImGui::InputTextWithHint("##q", "Find in graph...", findBuf_, sizeof findBuf_,
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        const bool queryActive = ImGui::IsItemActive();
        if (queryActive && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) findOpen_ = false;
        // EnterReturnsTrue reports only Enter, so edits are noticed by comparing with the last query run.
        if (findLastQuery_ != findBuf_) setFindQuery(findBuf_);
        if (enter) {
            if (io.KeyShift) findPrevious(); else findNext();
            findFocusRequest_ = true;   // Enter drops focus; take it back so the next Enter steps again
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(find_.label().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("<")) findPrevious();
        ImGui::SameLine();
        if (ImGui::SmallButton(">")) findNext();
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) findOpen_ = false;
        ImGui::PopID();
    } else {
        findBarMin_ = findBarMax_ = Vec2{};
    }

    // ---- Collapse to Function's name prompt, queued by the node menu.
    if (collapsePopupQueued_) { ImGui::OpenPopup("##graphCollapse"); collapsePopupQueued_ = false; }
    if (ImGui::BeginPopup("##graphCollapse")) {
        ImGui::TextUnformatted("Collapse to Function");
        ImGui::SetNextItemWidth(220.0f * dpi);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool go = ImGui::InputText("Name", collapseNameBuf_, sizeof collapseNameBuf_, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Collapse") || go) {
            collapseSelectionToFunction(collapseNameBuf_);   // a refusal arrives as the banner
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- right-click "add node" palette (graphNodeCatalog(); GraphNodeDefs.hpp's header comment says
    // why types register there) and the node context menu below -- every item already existed as a
    // keyboard shortcut with no other way to discover it; shortcut labels teach them. The menu acts on
    // the SELECTION, not rightClickNode_ (the press handler already synced the two), so "Delete" on one
    // of five selected nodes deletes five; it routes through the same copySelection/pasteClipboard/
    // duplicateSelection/deleteSelection calls the shortcuts use, so undo, remapping and link pruning
    // behave identically either way.
    if (ImGui::BeginPopup("##graphNodeMenu")) {
        const bool onNode = !rightClickNode_.empty();
        const int  count  = static_cast<int>(selectedNodes_.size());
        if (onNode) {
            if (count > 1) ImGui::TextDisabled("%d nodes selected", count);
            else            ImGui::TextDisabled("%s", rightClickNode_.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Cut", "Ctrl+X"))       { copySelection(); deleteSelection(); }
            if (ImGui::MenuItem("Copy", "Ctrl+C"))      copySelection();
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, !clipboardEmpty()))
                pasteClipboard(pendingSpawnCanvasPos_);
            if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicateSelection();
            ImGui::Separator();
            // BREAK LINKS is the one command with no keyboard shortcut at all -- before this menu
            // the only way to disconnect a node was to click each wire and press Delete.
            if (ImGui::MenuItem("Break Links", nullptr, false, selectionHasLinks()))
                breakLinksOnSelection();
            ImGui::Separator();
            if (count == 1) {
                const bool hasBp = hasBreakpoint(rightClickNode_);
                if (ImGui::MenuItem(hasBp ? "Remove Breakpoint" : "Add Breakpoint", "F9")) {
                    debugSession().breakpoints.toggle(debugGraphName(), rightClickNode_);
                    persistBreakpoints();
                }
                if (hasBp && ImGui::MenuItem(breakpointEnabled(rightClickNode_) ? "Disable Breakpoint" : "Enable Breakpoint"))
                    setBreakpointEnabled(rightClickNode_, !breakpointEnabled(rightClickNode_));

                // Find References for whatever this node names: a variable, a function, an event, a class.
                const fmt::OcGraphNode* rn = nullptr;
                for (const auto& cand : graph_.nodes) if (cand.id == rightClickNode_) { rn = &cand; break; }
                if (rn) {
                    RefKind refKind = RefKind::Variable;
                    std::string refName;
                    const GraphNodeAttribute av = getNodeAttribute(*rn, "var");
                    const GraphNodeAttribute ac = getNodeAttribute(*rn, "call");
                    const GraphNodeAttribute an = getNodeAttribute(*rn, "name");
                    const GraphNodeAttribute ae = getNodeAttribute(*rn, "event");
                    const GraphNodeAttribute ak = getNodeAttribute(*rn, "class");
                    if (av.found && !av.value.empty())                                { refKind = RefKind::Variable; refName = av.value; }
                    else if (ac.found && !ac.value.empty())                           { refKind = RefKind::Function; refName = ac.value; }
                    else if (rn->type == "CustomEvent" && an.found && !an.value.empty()) { refKind = RefKind::Event;    refName = an.value; }
                    else if (ae.found && !ae.value.empty())                           { refKind = RefKind::Event;    refName = ae.value; }
                    else if (ak.found && !ak.value.empty())                           { refKind = RefKind::Class;    refName = ak.value; }
                    if (!refName.empty()) {
                        if (ImGui::MenuItem("Find References")) showReferences(refKind, refName, false);
                        if (ImGui::MenuItem("Find References (whole project)")) showReferences(refKind, refName, true);
                    }
                }
                ImGui::Separator();
            }
            if (ImGui::MenuItem("Collapse to Function...")) {
                collapsePopupQueued_ = true;
                std::snprintf(collapseNameBuf_, sizeof collapseNameBuf_, "%s", "NewFunction");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete", "Del")) deleteSelection();
        } else {
            // A link was right-clicked. Only two things can sensibly be done to a wire, and both
            // are the same thing -- so this arm is short on purpose rather than padded to match.
            ImGui::TextDisabled("Link");
            ImGui::Separator();
            if (ImGui::MenuItem("Delete", "Del")) deleteSelection();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##graphAddNode")) {
        // When opened by dropping a wire, say so and what's hidden -- a silently shortened list reads
        // as a missing node, not a filter (same failure the "...and N more" line below avoids).
        if (linkDropPending_) {
            ImGui::TextDisabled("connecting %s.%s (%s) -- showing types that accept it",
                                linkDropFromNode_.c_str(), linkDropFromPin_.c_str(),
                                linkDropFromType_.c_str());
            ImGui::Separator();
        }

        // SEARCH FIRST, focused on open: type, Up/Down to choose, Enter to add. Matching and ranking live
        // in GraphNodeDefs.hpp (graphPaletteSearch) so a test can drive them; the categories below are
        // the empty-box view.
        if (ImGui::IsWindowAppearing()) {
            addSearch_[0] = '\0';
            addHighlight_ = 0;
            ImGui::SetKeyboardFocusHere();
        }
        i32 navMove = 0;
        ImGui::SetNextItemWidth(360.0f * dpi);
        const bool enterPick = ImGui::InputTextWithHint(
            "##addsearch", "Search nodes...", addSearch_, sizeof addSearch_,
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory,
            [](ImGuiInputTextCallbackData* cb) {
                i32& move = *static_cast<i32*>(cb->UserData);
                if (cb->EventKey == ImGuiKey_UpArrow) move = -1;
                else if (cb->EventKey == ImGuiKey_DownArrow) move = 1;
                return 0;
            },
            &navMove);
        if (ImGui::IsItemEdited()) addHighlight_ = 0;

        if (addSearch_[0] != '\0') {
            const GraphNodeDomain searchDomain = openGraphDomain();
            const std::string_view q(addSearch_);
            // One list for everything the box can add: a comment box and this graph's functions match by
            // name too, so the search is never the one place they cannot be found.
            struct AddRow { std::string label; const char* category; const GraphNodeDesc* desc; const std::string* fn; };
            std::vector<AddRow> rows;
            if (!linkDropPending_ && detail::ciFind("Comment Box", q) != std::string_view::npos)
                rows.push_back({"Comment Box", "Graph", nullptr, nullptr});
            for (const auto& f : graph_.functions)
                if (detail::ciFind(f.name, q) != std::string_view::npos || detail::ciFind("Call Function", q) == 0)
                    rows.push_back({"Call " + f.name + (f.pure ? "  (pure)" : ""), "Function", nullptr, &f.name});
            const usize kShown = 200;
            for (const GraphNodeDesc* d : graphPaletteSearch(addSearch_, searchDomain, kShown)) {
                if (linkDropPending_ && !linkDropAccepts(*d)) continue;
                rows.push_back({d->displayName, d->category.c_str(), d, nullptr});
            }
            if (rows.empty()) {
                ImGui::TextDisabled("no node matches \"%s\"", addSearch_);
            } else {
                addHighlight_ = std::clamp(addHighlight_ + navMove, 0, static_cast<i32>(rows.size()) - 1);
                const f32 rowH = ImGui::GetTextLineHeightWithSpacing();
                const f32 listH = std::min(static_cast<f32>(rows.size()) * rowH + ImGui::GetStyle().WindowPadding.y * 2.0f,
                                           380.0f * dpi);
                i32 pick = enterPick ? addHighlight_ : -1;
                ImGui::BeginChild("##addresults", ImVec2(460.0f * dpi, listH), ImGuiChildFlags_None);
                const ImGuiIO& io = ImGui::GetIO();
                const bool mouseMoved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
                for (usize i = 0; i < rows.size(); ++i) {
                    const AddRow& r = rows[i];
                    const bool on = static_cast<i32>(i) == addHighlight_;
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable(r.label.c_str(), on, ImGuiSelectableFlags_None, ImVec2(300.0f * dpi, 0.0f)))
                        pick = static_cast<i32>(i);
                    if (ImGui::IsItemHovered() && mouseMoved) addHighlight_ = static_cast<i32>(i);
                    if (on && navMove != 0) ImGui::SetScrollHereY();
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", r.category);
                    ImGui::PopID();
                }
                ImGui::EndChild();
                // Never a silent truncation: a capped list that just stops reads as the whole answer.
                const usize total = graphPaletteSearchCount(addSearch_, searchDomain);
                if (total > kShown) ImGui::TextDisabled("...and %zu more; type more to narrow", total - kShown);
                ImGui::TextDisabled("Up/Down to choose, Enter to add");
                if (pick >= 0) {
                    const AddRow& r = rows[static_cast<usize>(pick)];
                    if (r.desc) {
                        spawnAndConnectLinkDrop(r.desc->typeId, pendingSpawnCanvasPos_);
                    } else if (r.fn) {
                        addCallNode(*r.fn, pendingSpawnCanvasPos_);
                    } else {
                        const Vec2 a = pendingSpawnCanvasPos_;
                        addComment(a, Vec2{a.x + 320.0f * dpi, a.y + 180.0f * dpi}, "Comment");
                    }
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        // The category menus are the empty-box view, hence `else` not an early return: EndChild() and
        // the details panel draw after this popup, and returning here would take the right side with it.
        else {
        ImGui::Separator();
        // Every declared function gets a "Call <name>" entry, regenerated from graph_.functions each
        // frame; it has no fixed pin shape (it takes the callee's), so it can't live in the static catalog.
        if (!graph_.functions.empty() && ImGui::BeginMenu("Call Function")) {
            for (const auto& f : graph_.functions) {
                std::string row = f.name + (f.pure ? "  (pure)" : "");
                if (ImGui::MenuItem(row.c_str())) addCallNode(f.name, pendingSpawnCanvasPos_);
            }
            ImGui::EndMenu();
        }
        // A comment box is not a node and has no pins, so it sits above the families rather than in one.
        if (ImGui::MenuItem("Comment Box", "C")) {
            const Vec2 a = pendingSpawnCanvasPos_;
            addComment(a, Vec2{a.x + 320.0f * dpi, a.y + 180.0f * dpi}, "Comment");
        }
        ImGui::Separator();
        // Only this graph's own vocabulary: a material graph has no Branch/Spawn/CharacterMove (those
        // compile to IL and call the framework; a material is per-pixel arithmetic with nothing to
        // call, so offering them would only lead to a compile error). Filtered here, the ONE place the
        // palette is built, so an empty category never appears.
        const GraphNodeDomain domain = openGraphDomain();
        std::vector<std::string> categories;
        for (const auto& d : graphNodeCatalog()) {
            if ((d.domain & domain) == 0u) continue;
            // A category whose every member refuses the pending wire isn't shown, same reason as the
            // domain filter above: an empty submenu is a dead end that looks like a place to check.
            if (linkDropPending_ && !linkDropAccepts(d)) continue;
            if (std::find(categories.begin(), categories.end(), d.category) == categories.end())
                categories.push_back(d.category);
        }
        for (const auto& cat : categories) {
            // See GraphNodeDefs.hpp's Function block: these three exist in the catalog for their
            // name and colour, and are created by the Functions panel, which knows which function
            // they belong to. Dropping a bare one produces a node with no pins and no owner.
            if (cat == "Function") continue;
            if (ImGui::BeginMenu(cat.c_str())) {
                for (const auto& d : graphNodeCatalog()) {
                    if (d.category != cat || (d.domain & domain) == 0u) continue;
                    if (linkDropPending_ && !linkDropAccepts(d)) continue;
                    // Thin glue: everything the drop actually DOES is spawnAndConnectLinkDrop (which
                    // is addNodeFromCatalog when no wire is pending), so it can be driven by a test
                    // with no ImGui context.
                    if (ImGui::MenuItem(d.displayName.c_str()))
                        spawnAndConnectLinkDrop(d.typeId, pendingSpawnCanvasPos_);
                }
                ImGui::EndMenu();
            }
        }
        }   // else: the category menus
        ImGui::EndPopup();
    } else if (linkDropPending_ && !ImGui::IsPopupOpen("##graphAddNode")) {
        // Disarmed when the popup goes away without a pick (Escape, click outside) -- otherwise the
        // gesture stays armed and the next ordinary right-click Add Node silently wires to a stale pin.
        cancelLinkDrop();
    }

    ImGui::EndChild();
    if (bottomOpen_) drawBottomPanel(canvasW, bottomH, dpi);
    ImGui::EndGroup();

    // Details panel: Variables (graph-level) above a selected node's attributes (Gap B). Thin ImGui glue
    // only -- the model lives in this file's addVariable/renameVariable/retypeVariable/
    // setVariableDefault/deleteVariable, and in GraphEditorGeometry.hpp's getNodeAttribute/
    // setNodeAttribute/removeNodeAttribute/computeAttributeRows for the per-node rows below; both
    // exercised headlessly by GraphEditorGeometryTest and GraphEditorLoadSaveTest.
    //
    // The handle mutates detailsColW_ for the NEXT frame (`detailsW` is this frame's captured value)
    // and persists it, as a DPI-independent pixel width (ActorEditor's convention), the instant the
    // drag ends -- matches ActorEditor's splitter site (ActorEditor.cpp) exactly: capture-then-size,
    // drag-after, persist-on-release.
    bool detailsReleased = false;
    splitterHandle("##graphSplit", 6.0f * dpi, &detailsColW_, avail.x, minDetails, minCanvas,
                    &detailsReleased);
    if (detailsReleased) {
        setPrefFloat(kPrefDetailsColumn, detailsColW_ / dpi);
        flushEditorPrefs();
    }
    ImGui::BeginChild("##graphDetails", ImVec2(detailsW, std::max(avail.y, 80.0f * dpi)), true);

    drawFunctionsPanel(dpi);

    // Variables panel: declare/rename/retype/delete. Lives above the per-node section, drawn regardless
    // of selection -- a variable belongs to the GRAPH, not whichever node is selected, and right after
    // opening a graph (nothing selected) is exactly when an author needs to declare one. This is also
    // the fix for the bug the task brief leads with: a freshly palette-spawned SetVar has no variable
    // to name yet, and this panel -- not a free-text field -- is where one gets created.
    if (ImGui::CollapsingHeader("Variables", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (graph_.variables.empty()) {
            ImGui::TextDisabled("No variables declared.");
        }
        // Snapshot names before iterating: a Delete on row i mutates graph_.variables mid-loop
        // (deleteVariable's pushUndo+erase), invalidating later rows' iterators/indices. Looking each
        // up FRESH by name tolerates that -- a name deleted by an earlier row is simply skipped, not
        // read through a dangling reference.
        std::vector<std::string> varNames;
        varNames.reserve(graph_.variables.size());
        for (const auto& v : graph_.variables) varNames.push_back(v.name);

        for (const std::string& vname : varNames) {
            const fmt::OcGraphVariable* vptr = nullptr;
            for (const auto& v : graph_.variables) if (v.name == vname) { vptr = &v; break; }
            if (!vptr) continue; // deleted by an earlier row's Delete button this same frame
            const fmt::OcGraphVariable& v = *vptr;

            ImGui::PushID(("##var_" + vname).c_str());
            ImGui::PushItemWidth(70.0f * dpi);

            // ---- name (rename), same activate/apply-live/deactivate shape as attrEditRowKey_/
            // attrEditBuf_ above, keyed by field+name so a delete elsewhere never bleeds into this
            // row's in-flight buffer.
            const std::string nameRowKey = "varname\x1f" + vname;
            char nameBuf[256];
            std::snprintf(nameBuf, sizeof nameBuf, "%s", (varEditRowKey_ == nameRowKey) ? varEditBuf_ : v.name.c_str());
            const bool nameChanged = ImGui::InputText("##name", nameBuf, sizeof nameBuf);
            if (ImGui::IsItemActivated()) varEditRowKey_ = nameRowKey;
            if (nameChanged && varEditRowKey_ == nameRowKey) std::snprintf(varEditBuf_, sizeof varEditBuf_, "%s", nameBuf);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                const std::string newName = varEditBuf_;
                if (newName != vname && !renameVariable(vname, newName)) {
                    showRejectionBanner("cannot rename '" + vname + "' to '" + newName +
                                         "': empty, contains whitespace, or already used by another variable");
                }
                varEditRowKey_.clear();
            } else if (ImGui::IsItemDeactivated()) {
                varEditRowKey_.clear();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rename -- updates every GetVar/SetVar that uses this variable");
            if (ImGui::BeginPopupContextItem("##varctx")) {
                if (ImGui::MenuItem("Find References")) showReferences(RefKind::Variable, vname, false);
                if (ImGui::MenuItem("Find References (whole project)")) showReferences(RefKind::Variable, vname, true);
                ImGui::EndPopup();
            }
            ImGui::SameLine();

            // ---- type (retype): a Combo, not free text -- VAR only ever declares float/int/bool
            // (see isValidVarType's own comment), so there is nothing a text field would offer that a
            // fixed 3-item list doesn't already cover, and a Combo can't typo its way into a 4th.
            static const char* kVarTypes[] = {"float", "int", "bool"};
            int typeIdx = 0;
            for (int i = 0; i < 3; ++i) if (v.type == kVarTypes[i]) { typeIdx = i; break; }
            if (ImGui::Combo("##type", &typeIdx, kVarTypes, 3)) retypeVariable(vname, kVarTypes[typeIdx]);
            ImGui::SameLine();

            const std::string defRowKey = "vardefault\x1f" + vname;
            char defBuf[256];
            std::snprintf(defBuf, sizeof defBuf, "%s", (varEditRowKey_ == defRowKey) ? varEditBuf_ : v.defaultValue.c_str());
            const bool defChanged = ImGui::InputText("##default", defBuf, sizeof defBuf);
            if (ImGui::IsItemActivated()) varEditRowKey_ = defRowKey;
            if (defChanged && varEditRowKey_ == defRowKey) std::snprintf(varEditBuf_, sizeof varEditBuf_, "%s", defBuf);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                if (!setVariableDefault(vname, varEditBuf_))
                    showRejectionBanner("cannot set default for '" + vname + "': value contains whitespace, which this format cannot represent");
                varEditRowKey_.clear();
            } else if (ImGui::IsItemDeactivated()) {
                varEditRowKey_.clear();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Default value (optional) -- what a fresh GraphHost seeds this variable with");
            ImGui::PopItemWidth();
            ImGui::SameLine();

            // ---- delete: deleteVariable itself raises the rejection banner (naming every blocking
            // node) when refused, so there is nothing further to do with its return value here.
            if (ImGui::Button("X")) deleteVariable(vname);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete -- refused while any GetVar/SetVar still uses this variable");

            // Mismatch note: nodes whose OWN 'value' pin type disagrees with the variable's CURRENT
            // type. Computed fresh every frame from the live pins -- see retypeVariable's comment for
            // why a retype never rewrites those pins, and why this is what surfaces the mismatch.
            {
                std::vector<std::string> mismatched;
                for (const auto& n : graph_.nodes) {
                    const GraphNodeAttribute a = getNodeAttribute(n, "var");
                    if (!a.found || a.value != vname) continue;
                    for (const auto& p : n.pins) {
                        if (p.name == "value" && p.type != v.type) { mismatched.push_back(n.id); break; }
                    }
                }
                if (!mismatched.empty()) {
                    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "  used as %s by a node whose pin type disagrees:", v.type.c_str());
                    std::string names;
                    for (usize i = 0; i < mismatched.size(); ++i) { if (i) names += ", "; names += mismatched[i]; }
                    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "  %s", names.c_str());
                }
            }

            ImGui::PopID();
        }

        if (ImGui::Button("+ New Variable")) {
            addVariable(makeUniqueVariableName("Var"), "float", "");
        }
    }
    ImGui::Separator();

    if (selectedNodes_.size() != 1) {
        ImGui::TextDisabled(selectedNodes_.empty() ? "Select a node to edit its attributes."
                                                     : "Select a single node to edit its attributes.");
    } else {
        const std::string& nodeId = selectedNodes_.front();
        fmt::OcGraphNode* node = nullptr;
        for (auto& n : graph_.nodes) if (n.id == nodeId) { node = &n; break; }
        if (!node) {
            ImGui::TextDisabled("Selected node no longer exists.");
        } else {
            const GraphNodeDesc* desc = findGraphNodeDesc(node->type);
            ImGui::TextUnformatted(node->id.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", node->type.c_str());
            ImGui::Separator();

            std::vector<std::pair<std::string, std::string>> declared;
            if (desc) {
                declared.reserve(desc->attributes.size());
                for (const GraphAttributeSpec& a : desc->attributes) declared.emplace_back(a.key, a.label);
            }
            const std::vector<GraphAttributeRow> rows = computeAttributeRows(*node, declared);

            if (rows.empty()) {
                ImGui::TextDisabled("This node type has no attributes.");
            }
            bool drewLeftoverHeader = false;
            for (const GraphAttributeRow& row : rows) {
                if (!row.declared && !drewLeftoverHeader) {
                    if (!declared.empty()) ImGui::Separator();
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
                    ImGui::TextDisabled("Other attributes (not recognised by this editor build, preserved on save):");
                    ImGui::PopTextWrapPos();
                    drewLeftoverHeader = true;
                }

                const std::string rowKey = node->id + "\x1f" + row.key;
                ImGui::PushID(rowKey.c_str());

                // The picker: GetVar/SetVar's var= row gets a combo instead of the free-text field below
                // (field=/class=/param=/etc. keep that path unchanged -- deliberately narrow, not a
                // key=value framework). row.key=="var" is reachable only for GetVar/SetVar
                // (GraphNodeDefs.hpp), so no separate node-type check is needed.
                if (row.declared && row.key == "var") {
                    const bool known = !row.value.empty() &&
                        std::any_of(graph_.variables.begin(), graph_.variables.end(),
                                    [&](const fmt::OcGraphVariable& v) { return v.name == row.value; });
                    std::string preview = row.value.empty() ? "(none)" : row.value;
                    if (!row.value.empty() && !known) preview += "  [undeclared!]";
                    if (ImGui::BeginCombo(row.label.c_str(), preview.c_str())) {
                        if (ImGui::Selectable("(none)", row.value.empty())) clearAttribute(node->id, "var");
                        for (const fmt::OcGraphVariable& v : graph_.variables) {
                            const bool selected = (v.name == row.value);
                            const std::string itemLabel = v.name + "  (" + v.type + ")";
                            if (ImGui::Selectable(itemLabel.c_str(), selected)) setAttribute(node->id, "var", v.name);
                        }
                        ImGui::EndCombo();
                    }
                    if (!row.value.empty() && !known) {
                        // An undeclared variable doesn't compile (Graph.Validate() refuses it) and used
                        // to give no sign anything was wrong; this names the problem and offers a
                        // one-click fix.
                        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "'%s' is not a declared variable.", row.value.c_str());
                        const std::string declareLabel = "Declare '" + row.value + "'";
                        if (ImGui::Button(declareLabel.c_str())) {
                            // Guess the type from the node's own 'value' pin (a freshly spawned
                            // GetVar/SetVar always has one) so the declared variable matches what
                            // the node is already wired to expect.
                            std::string guessType = "float";
                            for (const auto& p : node->pins) if (p.name == "value") { guessType = p.type; break; }
                            addVariable(row.value, guessType, "");
                        }
                    } else if (row.value.empty() && graph_.variables.empty()) {
                        // A freshly spawned SetVar with no variables declared yet has nothing for
                        // the combo to offer but "(none)" -- point at the panel that fixes that.
                        ImGui::TextDisabled("No variables declared -- add one in the Variables panel above.");
                    } else if (row.value.empty()) {
                        // var="" with a variable declared: neither branch above fires, so without this
                        // the combo would show "(none)" as if that compiled -- it doesn't; OcGraphParser.cs
                        // refuses a missing var= like an undeclared one (reachable via
                        // testDeleteVariableRefusesWhileReferencedThenSucceeds, which clears 'gv's var=).
                        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "No variable selected -- pick one above; this node will not compile without one.");
                    }
                    ImGui::PopID();
                    continue; // this row is fully drawn; skip the generic InputText path below
                }

                char buf[512];
                if (attrEditRowKey_ == rowKey) {
                    std::snprintf(buf, sizeof buf, "%s", attrEditBuf_);
                } else {
                    std::snprintf(buf, sizeof buf, "%s", row.value.c_str());
                }

                const bool changed = ImGui::InputText(row.label.c_str(), buf, sizeof buf);
                if (ImGui::IsItemActivated()) attrEditRowKey_ = rowKey;
                if (changed && attrEditRowKey_ == rowKey) std::snprintf(attrEditBuf_, sizeof attrEditBuf_, "%s", buf);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    const std::string newValue = attrEditBuf_;
                    if (newValue.empty()) clearAttribute(node->id, row.key);
                    else setAttribute(node->id, row.key, newValue);
                    attrEditRowKey_.clear();
                } else if (ImGui::IsItemDeactivated()) {
                    attrEditRowKey_.clear();
                }

                // The action picker: a convenience dropdown beside the free-text field, sourced from the
                // project's INPUT.SCHEME (refreshSchemeActionsIfNeeded). Reachable only for
                // InputAction/InputActionPressed/InputActionReleased/RebindAction/GetActionKey and the
                // Enhanced Input nodes (InputActionState, AddInputTrigger, AddInputModifier,
                // ClearInputConfig, OnInputAction) -- any node declaring an `action` attribute
                // (GraphNodeDefs.hpp, GraphNodeDefsGameSystems.hpp), like the var== branch above (no separate node-type check needed).
                // Free text stays the source of truth -- InputText above accepts any name (even one a
                // C# script pushed with no scheme behind it), so this flags an unrecognised name rather
                // than refusing it. Falls back to the plain InputText row above with nothing added when
                // no scheme is found -- exactly what happens if this whole block is skipped.
                if (row.declared && row.key == "action") {
                    refreshSchemeActionsIfNeeded();
                    if (schemeFileFound_) {
                        if (!schemeActionNames_.empty()) {
                            ImGui::SameLine();
                            if (ImGui::BeginCombo("##actionPick", "", ImGuiComboFlags_NoPreview)) {
                                for (const std::string& name : schemeActionNames_) {
                                    if (ImGui::Selectable(name.c_str())) setAttribute(node->id, "action", name);
                                }
                                ImGui::EndCombo();
                            }
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("Pick a declared action from %s", schemeResolvedPath_.c_str());
                        }
                        if (!row.value.empty() &&
                            std::find(schemeActionNames_.begin(), schemeActionNames_.end(), row.value) ==
                                schemeActionNames_.end()) {
                            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f),
                                "'%s' is not declared in %s.", row.value.c_str(), schemeResolvedPath_.c_str());
                        }
                    }
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
#else
    (void)dpi;
#endif
}

} // namespace aver::editor
