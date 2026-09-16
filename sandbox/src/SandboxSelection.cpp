// Editor: multi-selection, transforms, edit commands, reparenting, destroy, undo and redo.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"

namespace aver {
#if AVER_MODULE_SCENE
// THE INVARIANT IS A VALIDITY TEST, NOT A CHORE FOR FORTY CALL SITES.
//
// sel_/selEntity_ are assigned directly in about forty places -- viewport pick, undo, paste,
// spawn, drag-drop, Play, the Details panel's Select buttons, several delete paths. Teaching
// every one of them to also maintain multiSel_ would work exactly until somebody adds the
// forty-first, and the failure is silent and nasty: a stale set means a later Delete removes
// objects the user cannot see highlighted.
//
// So the set is only BELIEVED while it still contains the anchor. Anything that moves the anchor
// on its own therefore collapses the selection to one, which is the correct and safe default for
// every one of those sites -- picking in the viewport, undoing, pasting and spawning all mean
// "this one thing is now selected". The outliner's own multi paths keep the anchor inside the
// set, so they stay multi.
bool SandboxApp::multiStale() const {
    return !multiSel_.empty() &&
           std::find(multiSel_.begin(), multiSel_.end(), selEntity_) == multiSel_.end();
}

bool SandboxApp::multiIsSelected(scene::Entity e) const {
    if (multiStale()) return false;
    return std::find(multiSel_.begin(), multiSel_.end(), e) != multiSel_.end();
}

// Drops a set the anchor has left. Called once a frame so a stale set cannot come back to life
// later by the anchor happening to land on one of its members again.
void SandboxApp::multiSyncToAnchor() { if (multiStale()) multiSel_.clear(); }

void SandboxApp::multiClear() { multiSel_.clear(); }

// Plain click: this one becomes the whole selection.
void SandboxApp::multiSetSingle(scene::Entity e) {
    multiSel_.assign(1, e);
    sel_ = kSelScene; selEntity_ = e;
}

// Ctrl+click: add or remove one, and keep the anchor pointing at something that is still selected.
void SandboxApp::multiToggle(scene::Entity e) {
    const auto it = std::find(multiSel_.begin(), multiSel_.end(), e);
    if (it != multiSel_.end()) {
        multiSel_.erase(it);
        if (selEntity_ == e) {
            // The anchor was just deselected. Hand it to whatever is left rather than leaving it
            // pointing at something the user can no longer see highlighted.
            if (multiSel_.empty()) { sel_ = -1; selEntity_ = scene::kInvalidEntity; }
            else selEntity_ = multiSel_.back();
        }
        return;
    }
    multiSel_.push_back(e);
    sel_ = kSelScene; selEntity_ = e;
}

// Shift+click: everything between the anchor and this row, in the order the rows are DRAWN.
// Visible order, not creation order -- a range selection means "what I can see between these
// two", and the outliner's order is a filtered, sorted tree walk that matches nothing else.
void SandboxApp::multiRange(scene::Entity to) {
    const auto& ord = outlinerOrder_;
    const auto a = std::find(ord.begin(), ord.end(), selEntity_);
    const auto b = std::find(ord.begin(), ord.end(), to);
    if (a == ord.end() || b == ord.end()) { multiSetSingle(to); return; }
    auto lo = a, hi = b;
    if (lo > hi) std::swap(lo, hi);
    multiSel_.clear();
    for (auto i = lo; i <= hi; ++i) multiSel_.push_back(*i);
    // The anchor STAYS where it was, so a second shift-click re-ranges from the same origin
    // instead of walking the anchor along with it -- which is what every file browser does.
    sel_ = kSelScene;
    if (!multiIsSelected(selEntity_)) selEntity_ = to;
}

// The live selection, skipping anything destroyed since it was selected. Callers must not assume
// multiSel_ itself is clean: an entity can be deleted by a script, by streaming, or by an undo.
std::vector<scene::Entity> SandboxApp::selectedEntities() const {
    std::vector<scene::Entity> out;
    const scene::World& w = scene::World::instance();
    if (!multiStale())
        for (const scene::Entity e : multiSel_) if (w.valid(e)) out.push_back(e);
    if (out.empty() && sel_ == kSelScene && w.valid(selEntity_)) out.push_back(selEntity_);
    return out;
}

// Selects every row the World Outliner currently lists. The anchor becomes the FIRST row rather
// than the last, so a following shift-click ranges downward from the top the way a person expects
// after "select all" -- multiSetSingle + multiToggle would otherwise leave the anchor on whatever
// happened to be added last.
void SandboxApp::selectAllInOutliner() {
    if (outlinerOrder_.empty()) return;
    multiSetSingle(outlinerOrder_.front());
    for (usize i = 1; i < outlinerOrder_.size(); ++i) multiToggle(outlinerOrder_[i]);
    sel_ = kSelScene; selEntity_ = outlinerOrder_.front();
}

#endif

bool SandboxApp::movableSelected() const { return sel_ >= 0 && sel_ < (int)objects_.size(); }

// True while a play session owns the input, so editor interaction must stand down.
bool SandboxApp::gameHasInput() const { return playSessionActive() && !releasedByUser_; }

// True when the selection is a thing in either world, rather than a sun/sky/post pseudo-entry.
bool SandboxApp::anySelected() const {
#if AVER_MODULE_SCENE
    if (sel_ == kSelScene) return scene::World::instance().valid(selEntity_);
#endif
    return movableSelected();
}

// Reads the selection's transform, IN WORLD SPACE. False when nothing transformable is selected.
//
// WORLD, NOT LOCAL, AND EVERY CALLER NEEDED IT TO BE. This pair returned CLocal verbatim while
// the gizmo draws at the returned position, hit-tests its handles by projecting it, and the drag
// moves it by a world-space delta -- so selecting a child of an entity at (5000,0,0) drew the
// gizmo 50 m from its own mesh, and dragging it moved the child by the mouse delta measured
// against a manipulator that was somewhere else entirely. The Details panel and the undo capture
// read the same pair, so making only the gizmo world-space would have left the three disagreeing
// about what a number means, which is worse than all three being wrong the same way.
//
// Both halves convert, so the round trip is exact for a root (the parent matrix is identity) and
// the change is a no-op for every level that has no hierarchy in it -- which today is all of them.
bool SandboxApp::selectedXform(EditXform& x) const {
#if AVER_MODULE_SCENE
    if (sel_ == kSelScene) {
        scene::World& w = scene::World::instance();
        if (!w.valid(selEntity_)) return false;
        const auto* loc = w.component<scene::CLocal>(selEntity_, scene::kComponentLocal);
        if (!loc) return false;
        const scene::Entity par = w.parent(selEntity_);
        if (par == scene::kInvalidEntity) {
            x.pos = loc->xf.position;
            x.rotDeg = eulerDegFromQuat(loc->xf.rotation);
            x.scale = loc->xf.scale;
            return true;
        }
        const Transform pw = worldTransformOf(w, par);
        x.pos    = pw.position + pw.rotation.rotate(Vec3{loc->xf.position.x * pw.scale.x,
                                                        loc->xf.position.y * pw.scale.y,
                                                        loc->xf.position.z * pw.scale.z});
        x.rotDeg = eulerDegFromQuat(pw.rotation * loc->xf.rotation);
        x.scale  = Vec3{pw.scale.x * loc->xf.scale.x, pw.scale.y * loc->xf.scale.y,
                        pw.scale.z * loc->xf.scale.z};
        return true;
    }
#endif
    if (!movableSelected()) return false;
    const MeshObj& o = objects_[sel_];
    x.pos = o.pos; x.rotDeg = o.rotDeg; x.scale = o.scale;
    return true;
}

// Writes the selection's transform, taking it IN WORLD SPACE. The exact inverse of the read.
void SandboxApp::setSelectedXform(const EditXform& x) {
#if AVER_MODULE_SCENE
    if (sel_ == kSelScene) {
        scene::World& w = scene::World::instance();
        if (!w.valid(selEntity_)) return;
        Transform xf;
        xf.position = x.pos;
        xf.rotation = quatFromEulerDeg(x.rotDeg);
        xf.scale    = x.scale;
        w.setLocalTransform(selEntity_, localFromWorldFor(w, selEntity_, x));
        return;
    }
#endif
    if (!movableSelected()) return;
    MeshObj& o = objects_[sel_];
    o.pos = x.pos; o.rotDeg = x.rotDeg; o.scale = x.scale;
}

#if AVER_MODULE_SCENE
// The LOCAL transform an entity needs in order to sit at a given WORLD one, given its parent.
//
// THE ONE CONVERSION, because there were nearly two. setSelectedXform carried this inline while
// applyXformTo -- the function undo and redo actually call -- wrote its stored transform straight
// into setLocalTransform. Once selectedXform started returning WORLD, that made undo of any
// transform edit on a CHILD write a world position into CLocal: the child jumps to its own world
// coordinates reinterpreted as an offset from its parent. Invisible on a root, where the two
// frames coincide, which is why every suite and all twenty gates passed over it.
 Transform SandboxApp::localFromWorldFor(scene::World& w, scene::Entity e, const EditXform& x) {
    Transform xf;
    xf.position = x.pos;
    xf.rotation = quatFromEulerDeg(x.rotDeg);
    xf.scale    = x.scale;
    const scene::Entity par = w.valid(e) ? w.parent(e) : scene::kInvalidEntity;
    if (par == scene::kInvalidEntity) return xf;
    const Transform pw = worldTransformOf(w, par);
    // DIVIDING BY THE PARENT'S SCALE, and refusing to when it is zero. A zero component is legal
    // to author (a flattened parent) and would otherwise produce an infinity that propagates into
    // CLocal and then into every descendant's world matrix.
    const Vec3 inv{pw.scale.x != 0.0f ? 1.0f / pw.scale.x : 1.0f,
                   pw.scale.y != 0.0f ? 1.0f / pw.scale.y : 1.0f,
                   pw.scale.z != 0.0f ? 1.0f / pw.scale.z : 1.0f};
    // The CONJUGATE, which inverts a UNIT quaternion. Every rotation reaching this point comes
    // from quatFromEulerDeg or a composition of such, so it is unit by construction.
    const Quat pinv{-pw.rotation.x, -pw.rotation.y, -pw.rotation.z, pw.rotation.w};
    const Vec3 d = xf.position - pw.position;
    const Vec3 r = pinv.rotate(d);
    xf.position = Vec3{r.x * inv.x, r.y * inv.y, r.z * inv.z};
    xf.rotation = pinv * xf.rotation;
    xf.scale    = Vec3{xf.scale.x * inv.x, xf.scale.y * inv.y, xf.scale.z * inv.z};
    return xf;
}

// An entity's world transform as position/rotation/scale.
//
// COMPOSED UP THE CHAIN rather than decomposed from CWorld's Mat4: recovering a rotation and a
// scale from a matrix is only well-defined when the scale is uniform and positive, and this
// editor can author neither. Chains here are a handful of links deep at most.
 Transform SandboxApp::worldTransformOf(scene::World& w, scene::Entity e) {
    Transform out;
    if (!w.valid(e)) return out;
    const auto* loc = w.component<scene::CLocal>(e, scene::kComponentLocal);
    if (!loc) return out;
    const scene::Entity par = w.parent(e);
    if (par == scene::kInvalidEntity) return loc->xf;
    const Transform pw = worldTransformOf(w, par);
    out.position = pw.position + pw.rotation.rotate(Vec3{loc->xf.position.x * pw.scale.x,
                                                        loc->xf.position.y * pw.scale.y,
                                                        loc->xf.position.z * pw.scale.z});
    out.rotation = pw.rotation * loc->xf.rotation;
    out.scale    = Vec3{pw.scale.x * loc->xf.scale.x, pw.scale.y * loc->xf.scale.y,
                        pw.scale.z * loc->xf.scale.z};
    return out;
}

#endif

// Returns the edit id bound to an entity, minting one on first use.
// AvId, NOT scene::Entity: this function (and entityForEdit/rebindEdit below) is called only from
// #if AVER_MODULE_SCENE call sites, but the definitions are unguarded, so with SCENE off the
// parameter type needs to exist without it. scene::Entity is a bare alias for AvId, so this is a zero-behaviour-change retype.
SandboxApp::EditId SandboxApp::editIdFor(AvId e) {
    const u32 key = static_cast<u32>(e);
    if (const auto it = entityToEdit_.find(key); it != entityToEdit_.end()) return it->second;
    const EditId id = nextEditId_++;
    entityToEdit_[key] = id;
    editToEntity_[id]  = e;
    return id;
}

// Returns the entity an edit id names, or kInvalidId.
AvId SandboxApp::entityForEdit(EditId id) const {
    const auto it = editToEntity_.find(id);
    return it == editToEntity_.end() ? kInvalidId : it->second;
}

// Points an existing edit id at a newly created entity.
void SandboxApp::rebindEdit(EditId id, AvId e) {
    if (const auto old = editToEntity_.find(id); old != editToEntity_.end())
        entityToEdit_.erase(static_cast<u32>(old->second));
    editToEntity_[id] = e;
    entityToEdit_[static_cast<u32>(e)] = id;
}

#if AVER_MODULE_PBR
// Puts a whole MaterialDesc back and tells the library it moved.
//
// NAME IS PRESERVED FROM THE LIVE DESC, not restored from the snapshot: the name is the material's
// IDENTITY (surfaceMaterials_ and every .ocmat TEX record key on it), not part of what the panel
// edits, and writing a stale one back would rename a material as a side effect of undoing a
// roughness slider.
void SandboxApp::applyMaterialDesc(pbr::MaterialHandle h, const pbr::MaterialDesc& want) {
    pbr::MaterialDesc* d = pbr::MaterialLibrary::get().mutableDesc(h);
    if (!d) return;                       // the material went away; nothing to put back
    std::string keepName = d->name;
    *d = want;
    d->name = std::move(keepName);
    pbr::MaterialLibrary::get().touch(h);
}

#endif

#if AVER_MODULE_SCENE
// Sets an entity's OUTLINER LABEL -- entityLabels_, not scene::CName.
//
// WHY THE LABEL AND NOT THE NAME. CName is the ASSET the placement names ("Meshes/cube.ocmesh"),
// which is what saveLevel writes back as the PLACE record's asset and what resolves the mesh.
// Renaming that would repoint the placement at a file that does not exist. entityLabels_ is the
// editor's own display string, shown in the Outliner and the Details panel, and it is the thing
// a person means by "call this one Doorway".
//
// NOT PERSISTED YET, and that is worth knowing before relying on it: the .ocworld PLACE grammar
// has no field for a display name, so a label survives the session and not the save. Renaming
// being impossible was still the worse of the two -- an unnamed pile of "Cube 4" is unworkable
// long before persistence matters.
void SandboxApp::applyEntityLabel(EditId id, const std::string& label) {
    const scene::Entity e = entityForEdit(id);
    if (e == scene::kInvalidEntity || !scene::World::instance().valid(e)) return;
    if (label.empty()) entityLabels_.erase(static_cast<u32>(e));
    else               entityLabels_[static_cast<u32>(e)] = label;
}

// Renames the selected entity, as one undoable command. No-ops when the name did not change, so
// committing an unedited field does not put a do-nothing entry on the stack.
void SandboxApp::renameEntity(scene::Entity e, const std::string& to) {
    if (e == scene::kInvalidEntity || !scene::World::instance().valid(e)) return;
    std::string from;
    if (const auto it = entityLabels_.find(static_cast<u32>(e)); it != entityLabels_.end())
        from = it->second;
    if (from == to) return;
    EditCmd c;
    c.kind = EditCmd::Kind::Rename;
    c.id = editIdFor(e);
    c.renameBefore = from;
    c.renameAfter = to;
    applyEntityLabel(c.id, to);
    pushEdit(std::move(c));
}

// Puts a removed component back, byte-exact, from the RemoveComponent payload captured just
// before it was dropped -- undo's own half. addComponent hands back zero-filled storage (see
// EditorEntitySnapshot::instantiateEntity's identical restore loop), so the bytes are written over
// it here rather than trusted to already be right.
void SandboxApp::restoreComponent(EditId id, const editor::EntitySnapshot::Comp& comp) {
    const scene::Entity e = entityForEdit(id);
    if (e == scene::kInvalidEntity || !scene::World::instance().valid(e)) return;
    scene::World& w = scene::World::instance();
    void* dst = w.addComponent(e, comp.type);
    // A size mismatch means the component's layout changed since it was removed -- unreachable
    // within one editor session, but refused rather than written past what addComponent handed
    // back, the same refusal instantiateEntity makes for the identical case.
    if (!dst || w.componentSize(comp.type) != comp.bytes.size()) return;
    std::memcpy(dst, comp.bytes.data(), comp.bytes.size());
}

// Drops a component from an entity by edit id -- redo's own half, and also what
// removeComponentFromSelection calls to perform the removal it then pushes an undo entry for.
void SandboxApp::removeComponentRaw(EditId id, u32 type) {
    const scene::Entity e = entityForEdit(id);
    if (e == scene::kInvalidEntity || !scene::World::instance().valid(e)) return;
    scene::World::instance().removeComponent(e, type);
}

// Removes one component from the selected entity as one undoable command. Mirrors renameEntity's
// shape: capture the before-state, apply, push. THE BEFORE-STATE IS THE WHOLE COMPONENT, byte-
// exact (EntitySnapshot::Comp, the same shape captureEntity's own loop produces), because undo has
// to put back whatever was actually there, not just one flag.
bool SandboxApp::removeComponentFromSelection(u32 type) {
    scene::World& w = scene::World::instance();
    if (!w.valid(selEntity_) || !w.hasComponent(selEntity_, type)) return false;
    const void* src = w.getComponent(selEntity_, type);
    const usize size = w.componentSize(type);
    if (!src || size == 0) return false;
    EditCmd c;
    c.kind = EditCmd::Kind::RemoveComponent;
    c.id = editIdFor(selEntity_);
    c.removedComponent.type = type;
    c.removedComponent.bytes.resize(size);
    std::memcpy(c.removedComponent.bytes.data(), src, size);
    removeComponentRaw(c.id, type);
    pushEdit(std::move(c));
    return true;
}

#endif

void SandboxApp::pushEdit(EditCmd c) {
    c.serial = ++editSerialNext_;
    undoStack_.push_back(std::move(c));
    redoStack_.clear();
    if (undoStack_.size() > kUndoDepth) undoStack_.erase(undoStack_.begin());
}

// Records the selection's transform before a gesture. False when nothing is selected.
bool SandboxApp::beginTransformEdit() {
    if (!selectedXform(editBefore_)) return false;
    editBeforeValid_ = true;
    // The rest of the selection, captured BEFORE the drag starts moving anything. The mover runs
    // per frame and applies an incremental delta, so by the time the gesture ends the original
    // is gone -- there is no later moment this could be read.
    multiMoveBefore_.clear();
#if AVER_MODULE_SCENE
    forEachMultiMoved([this](scene::Entity e, const Transform& xf) {
        multiMoveBefore_.push_back({editIdFor(e), xf});
    });
#endif
    return true;
}

// Closes the gesture and pushes a transform command, unless nothing actually moved.
void SandboxApp::endTransformEdit() {
    if (!editBeforeValid_) return;
    editBeforeValid_ = false;

    // The rest of the set first, so the "did anything move at all" test below can consider them.
    std::vector<EditCmd::AlsoMoved> also;
#if AVER_MODULE_SCENE
    if (!multiMoveBefore_.empty()) {
        scene::World& w = scene::World::instance();
        for (const auto& [id, beforeLocal] : multiMoveBefore_) {
            const scene::Entity e = entityForEdit(id);
            if (e == scene::kInvalidEntity || !w.valid(e)) continue;
            const auto* loc = w.component<scene::CLocal>(e, scene::kComponentLocal);
            if (!loc) continue;
            if (nearlySameTransform(beforeLocal, loc->xf)) continue;
            also.push_back({id, beforeLocal, loc->xf});
        }
        multiMoveBefore_.clear();
    }
#endif

    EditXform now;
    if (!selectedXform(now)) return;
    // BOTH HALVES, because either can be the only thing that moved. An anchor pinned by a snap
    // while the rest of the set slid is a real gesture, and returning early on the anchor alone
    // would drop the whole record for it.
    if (nearlySameXform(editBefore_, now) && also.empty()) return;
    EditCmd c;
    c.kind = EditCmd::Kind::Transform;
    c.before = editBefore_; c.after = now;
    c.alsoMoved = std::move(also);
#if AVER_MODULE_SCENE
    if (sel_ == kSelScene) c.id = editIdFor(selEntity_); else
#endif
    c.objIndex = sel_;
    pushEdit(std::move(c));
}

// True when two transforms match to within 1e-4 on every component.
 bool SandboxApp::nearlySameXform(const EditXform& a, const EditXform& b) {
    auto same = [](const Vec3& p, const Vec3& q) {
        return std::fabs(p.x-q.x) < 1e-4f && std::fabs(p.y-q.y) < 1e-4f && std::fabs(p.z-q.z) < 1e-4f;
    };
    return same(a.pos,b.pos) && same(a.rotDeg,b.rotDeg) && same(a.scale,b.scale);
}

// Applies a transform command's stored value to whichever target it names, and selects it.
// True when two LOCAL transforms match to within 1e-4 on every component. The world-space twin
// of this is nearlySameXform; both exist because a gesture that ends where it began should push
// no command at all, and floating point makes "==" the wrong test for that.
 bool SandboxApp::nearlySameTransform(const Transform& a, const Transform& b) {
    auto v = [](const Vec3& p, const Vec3& q) {
        return std::fabs(p.x-q.x) < 1e-4f && std::fabs(p.y-q.y) < 1e-4f && std::fabs(p.z-q.z) < 1e-4f;
    };
    return v(a.position, b.position) && v(a.scale, b.scale) &&
           std::fabs(a.rotation.x-b.rotation.x) < 1e-4f && std::fabs(a.rotation.y-b.rotation.y) < 1e-4f &&
           std::fabs(a.rotation.z-b.rotation.z) < 1e-4f && std::fabs(a.rotation.w-b.rotation.w) < 1e-4f;
}

// `undoing` picks which end of each alsoMoved pair to write. The anchor's own end is already
// chosen by the caller through `x`; this parameter exists because the rest of the set is stored
// as a before/after PAIR rather than a single value, so it cannot infer the direction from `x`.
void SandboxApp::applyXformTo(const EditCmd& c, const EditXform& x, bool undoing) {
#if AVER_MODULE_SCENE
    // The rest of a multi-selection drag, restored in the same step as the anchor -- see
    // EditCmd::alsoMoved for why this is one command and not N.
    if (!c.alsoMoved.empty()) {
        scene::World& w = scene::World::instance();
        for (const EditCmd::AlsoMoved& m : c.alsoMoved) {
            const scene::Entity e = entityForEdit(m.id);
            if (e == scene::kInvalidEntity || !w.valid(e)) continue;
            w.setLocalTransform(e, undoing ? m.beforeLocal : m.afterLocal);
        }
    }
    if (c.id) {
        const scene::Entity e = entityForEdit(c.id);
        scene::World& w = scene::World::instance();
        if (e == scene::kInvalidEntity || !w.valid(e)) return;
        // WORLD IN, LOCAL OUT. A Transform command's before/after come from selectedXform,
        // which is world-space, and this used to assign them to CLocal unconverted.
        w.setLocalTransform(e, localFromWorldFor(w, e, x));
        sel_ = kSelScene; selEntity_ = e;
        return;
    }
#endif
    if (c.objIndex >= 0 && c.objIndex < (int)objects_.size()) {
        MeshObj& o = objects_[c.objIndex];
        o.pos = x.pos; o.rotDeg = x.rotDeg; o.scale = x.scale;
        // Unguarded: this is the non-scene MeshObj fallback, reached with SCENE off too.
        sel_ = c.objIndex; selEntity_ = kInvalidId;
    }
}

#if AVER_MODULE_SCENE
// Describes a live entity fully enough to rebuild it after a destroy: its outliner label, its
// transform, its physics-body half-extent if any, and -- via captureEntity() -- its asset name,
// persisted object id, and every OTHER component it carries. Shared by Delete (undo), Copy and Duplicate.
SandboxApp::EditCmd SandboxApp::describeEntity(scene::Entity e) {
    scene::World& w = scene::World::instance();
    EditCmd c;
    c.id = editIdFor(e);
    if (const auto it = entityLabels_.find(static_cast<u32>(e)); it != entityLabels_.end()) c.label = it->second;
    if (const auto* loc = w.component<scene::CLocal>(e, scene::kComponentLocal)) {
        c.after.pos = loc->xf.position;
        c.after.rotDeg = eulerDegFromQuat(loc->xf.rotation);
        c.after.scale = loc->xf.scale;
    }
    c.snap = editor::captureEntity(w, e);
    if (const scene::Entity par = w.parent(e); par != scene::kInvalidEntity)
        c.parentId = editIdFor(par);
#if AVER_MODULE_PHYSICS
    if (const auto it = entityBodies_.find(static_cast<u32>(e)); it != entityBodies_.end()) {
        c.hadBody = true;
        c.bodyHalf = c.after.scale;
    }
#endif
    // ABSENT MEANS "the default", exactly as saveLevel reads these maps: an entity created in the
    // editor has no entry and collides. Recording the absence as the default is what makes the
    // restore below a no-op for those, rather than inventing an entry they never had.
    if (const auto it = entityCollide_.find(static_cast<u32>(e)); it != entityCollide_.end())
        c.hadCollide = it->second;
    if (const auto it = entitySnapZ_.find(static_cast<u32>(e)); it != entitySnapZ_.end()) {
        c.hadSnapZ = true;
        // entitySnapZ_ is f64 to round-trip the LEVEL FORMAT's own f64 placement z VERBATIM (see
        // its own declaration comment); EditCmd::snapZ is f32, matching every other undo-system
        // field. Narrowing here is intended and safe -- a snap-to-ground z OFFSET in centimetres
        // has nothing near f32's ~7-decimal-digit precision to lose -- and scoped to undo/redo of
        // a delete, never to the load/save path that comment cares about bit-exactness for.
        c.snapZ    = static_cast<f32>(it->second);
    }
    return c;
}

// Builds a scene entity from a snapshot at `xf`, wires it into the editor's bookkeeping, selects
// it, and returns it. Shared tail for recreateFrom()/pasteClipboard()/duplicateSelection() --
// generalized from CMeshRenderer-only via EditorEntitySnapshot.hpp's instantiateEntity().
// restoreObjectId: true for recreateFrom (the SAME logical entity must return with the SAME
// identity), false for Paste/Duplicate (a NEW entity must NOT clone the source's objectId).
// spawnCube()/spawnFromAssetDrop() do NOT go through this: no prior snapshot to instantiate FROM,
// and refactoring their working create tails is out of scope here.
scene::Entity SandboxApp::spawnEntityFrom(const editor::EntitySnapshot& snap, const EditXform& xf,
                               const std::string& label, bool hadBody, const Vec3& bodyHalf,
                               bool restoreObjectId,
                               scene::Entity parent,
                               bool collide, bool hasSnapZ, f32 snapZ) {
    scene::World& w = scene::World::instance();
    Transform t; t.position = xf.pos; t.rotation = quatFromEulerDeg(xf.rotDeg); t.scale = xf.scale;
    const scene::Entity e = editor::instantiateEntity(w, snap, t, parent, restoreObjectId);
    if (e == scene::kInvalidEntity) {
        AVER_WARN("[Editor] the world refused to create '{}'", snap.asset);
        return e;
    }
    levelEntities_.push_back(e);
    if (!label.empty()) entityLabels_[static_cast<u32>(e)] = label;
    // The two non-component flags, put back. Only when they differ from the default, so an entity
    // that never had an entry does not gain one -- saveLevel treats present-and-true the same as
    // absent, but an editor-created entity gaining a map entry would be a difference for no reason.
    if (!collide) entityCollide_[static_cast<u32>(e)] = false;
    if (hasSnapZ) entitySnapZ_[static_cast<u32>(e)] = snapZ;
#if AVER_MODULE_PHYSICS
    if (hadBody && aver_phys_ready()) {
        const int32_t body = aver_phys_add_static_box(t.position.x, t.position.y, t.position.z,
                                                      bodyHalf.x, bodyHalf.y, bodyHalf.z);
        if (body) aver_phys_set_entity(body, static_cast<int32_t>(e));
        levelBodies_.push_back(body);
        entityBodies_[static_cast<u32>(e)] = body;
    }
#endif
    sel_ = kSelScene; selEntity_ = e;
    return e;
}

// Rebuilds an entity a command destroyed and rebinds its EditId to the new handle -- the ONE
// caller of spawnEntityFrom that must preserve the original EditId rather than mint a fresh one,
// since a later redo/undo of the SAME command needs to keep finding the same logical entity.
void SandboxApp::recreateFrom(const EditCmd& c) {
    // RESTORED UNDER WHAT IT HUNG FROM. A parent that has since been destroyed itself resolves
    // to kInvalidEntity and the entity comes back as a root -- losing the relationship, which is
    // recoverable by hand, rather than losing the object, which is not.
    scene::Entity par = scene::kInvalidEntity;
#if AVER_MODULE_SCENE
    if (c.parentId) {
        par = entityForEdit(c.parentId);
        if (!scene::World::instance().valid(par)) par = scene::kInvalidEntity;
    }
#endif
    const scene::Entity e = spawnEntityFrom(c.snap, c.after, c.label, c.hadBody, c.bodyHalf,
                                            true, par, c.hadCollide, c.hadSnapZ, c.snapZ);
    if (e == scene::kInvalidEntity) return;
    rebindEdit(c.id, e);

#if AVER_MODULE_SCENE
    spawnSubtreeUnder(e, c.subtree, /*restoreIds=*/true);
    // spawnEntityFrom selects whatever it made last, so the selection has to be put back on the
    // entity the command is actually about.
    sel_ = kSelScene; selEntity_ = e;
#endif
}

#endif

#if AVER_MODULE_SCENE
#if AVER_MODULE_SCENE
// Rebuilds a captured subtree under `root`. Parents precede children in `subtree`, so each
// node's parent handle is already live by the time it is read -- captureSubtree guarantees that
// ordering, and it is the only reason this can be a single forward pass.
//
// A NODE WHOSE PARENT IS MISSING is attached to `root` rather than dropped. Losing a
// relationship is recoverable by hand; losing the object is not.
//
// restoreIds SEPARATES AN UNDO FROM A COPY, and getting it wrong is not cosmetic. An undo is
// putting the SAME logical entities back, so each node's original EditId must be rebound to the
// new handle or every later command naming them addresses nothing. A paste or a duplicate is
// making NEW entities; rebinding there would point the ORIGINAL's id at the copy, so undoing the
// paste would delete the thing that was copied. Same reason restoreObjectId is false for a copy:
// two entities must not share one object id.
void SandboxApp::spawnSubtreeUnder(scene::Entity root, const std::vector<EditCmd::DestroyedNode>& subtree,
                       bool restoreIds) {
    std::vector<scene::Entity> made;
    made.reserve(subtree.size());
    for (const EditCmd::DestroyedNode& n : subtree) {
        scene::Entity par = root;
        if (n.parent >= 0 && n.parent < static_cast<i32>(made.size()))
            par = made[static_cast<usize>(n.parent)];
        const scene::Entity ce =
            spawnEntityFrom(n.snap, n.xf, n.label, n.hadBody, n.bodyHalf, restoreIds, par,
                            n.hadCollide, n.hadSnapZ, n.snapZ);
        made.push_back(ce);
        if (restoreIds && ce != scene::kInvalidEntity) rebindEdit(n.id, ce);
    }
}

// True when `maybeAncestor` is `e` itself or anywhere up its parent chain.
//
// THE DESIGN IS GraphEditor::componentIsAncestorOf's, not its code: bounded by the entity count
// for the same reason that one is bounded by the component count -- it runs MID-EDIT, in the
// frame a drop is being evaluated, which is exactly when a cycle would exist if this were the
// thing allowing it. World::setParent has its own cycle refusal, but the UI must never OFFER an
// illegal target, so the test has to be reachable from outside the scene module.
 bool SandboxApp::outlinerIsAncestorOf(const scene::World& w, scene::Entity maybeAncestor, scene::Entity e) {
    if (maybeAncestor == e) return true;
    scene::Entity at = e;
    for (u32 guard = 0; guard < w.count() && at != scene::kInvalidEntity; ++guard) {
        at = w.parent(at);
        if (at == maybeAncestor) return true;
    }
    return false;
}

// Whether the level's own save would carry this entity -- levelEntities_ ONLY, deliberately not
// levelClassInstances_. editor::appendClassPlacements copies the authored placement and
// overwrites position/rotation/scale alone; it never touches `parent`, so a class instance's
// parent is not round-tripped by a save whatever the live world says. Calling one "level owned"
// here would let a reparent through as legal that is provably lost on the next reload.
bool SandboxApp::isLevelOwned(scene::Entity e) const {
    return std::find(levelEntities_.begin(), levelEntities_.end(), e) != levelEntities_.end();
}

SandboxApp::ReparentLegality SandboxApp::reparentLegality(scene::Entity dragged, scene::Entity newParent) const {
    const scene::World& w = scene::World::instance();
    if (newParent != scene::kInvalidEntity && outlinerIsAncestorOf(w, dragged, newParent))
        return ReparentLegality::SelfOrDescendant;
    if (!isLevelOwned(dragged) ||
        (newParent != scene::kInvalidEntity && !isLevelOwned(newParent)))
        return ReparentLegality::OffLevel;
    return ReparentLegality::Ok;
}

// Reparents `child` under `newParent` (kInvalidEntity = make it a root) and pushes one undo
// entry. GUARD THEN MUTATE: a refused drop must not leave a phantom command on the stack.
void SandboxApp::pushReparent(scene::Entity child, scene::Entity newParent) {
    scene::World& w = scene::World::instance();
    if (!w.valid(child)) return;
    if (reparentLegality(child, newParent) != ReparentLegality::Ok) return;
    const scene::Entity oldParent = w.parent(child);
    if (oldParent == newParent) return;   // dropped back where it was; no undo-stack noise

    EditCmd c;
    c.kind = EditCmd::Kind::Reparent;
    c.id = editIdFor(child);
    c.reparentOldParentId = oldParent != scene::kInvalidEntity ? editIdFor(oldParent) : 0;
    if (const auto* loc = w.component<scene::CLocal>(child, scene::kComponentLocal)) {
        c.before.pos = loc->xf.position;
        c.before.rotDeg = eulerDegFromQuat(loc->xf.rotation);
        c.before.scale = loc->xf.scale;
    }

    // keepWorld = TRUE for the live drop: this is somebody dragging, and the object must not
    // jump out from under the mouse. Undo and redo then REPLAY the captured local values rather
    // than running keepWorld a second time -- the decompose is not guaranteed bit-exact across
    // repeated cycles, and replaying stored ground truth is what applyXformTo and recreateFrom
    // already do.
    //
    // AND THE RETURN VALUE IS CHECKED, which no existing caller of setParent does. A refusal
    // here means the world declined the move; pushing a command for it would put an entry on
    // the stack whose undo restores a state that never happened.
    if (!w.setParent(child, newParent, true)) return;

    if (const auto* loc = w.component<scene::CLocal>(child, scene::kComponentLocal)) {
        c.after.pos = loc->xf.position;
        c.after.rotDeg = eulerDegFromQuat(loc->xf.rotation);
        c.after.scale = loc->xf.scale;
    }
    c.reparentNewParentId = newParent != scene::kInvalidEntity ? editIdFor(newParent) : 0;
    sel_ = kSelScene; selEntity_ = child;
    pushEdit(std::move(c));
}

// Undo and redo share this: re-parent WITHOUT keepWorld, then stamp the exact local transform
// captured at that end of the edit. keepWorld would recompute one instead, which is how a
// repeated undo/redo cycle drifts.
//
// A PARENT THAT NO LONGER EXISTS resolves to root rather than aborting -- losing the
// relationship is recoverable by hand, leaving the entity somewhere nobody asked for is not.
void SandboxApp::applyReparentTo(const EditCmd& c, EditId parentEditId, const EditXform& xf) {
    scene::World& w = scene::World::instance();
    const scene::Entity e = static_cast<scene::Entity>(entityForEdit(c.id));
    if (e == scene::kInvalidEntity || !w.valid(e)) return;
    scene::Entity parent = scene::kInvalidEntity;
    if (parentEditId) {
        parent = static_cast<scene::Entity>(entityForEdit(parentEditId));
        if (!w.valid(parent)) parent = scene::kInvalidEntity;
    }
    w.setParent(e, parent, false);
    Transform t;
    t.position = xf.pos;
    t.rotation = quatFromEulerDeg(xf.rotDeg);
    t.scale    = xf.scale;
    w.setLocalTransform(e, t);
    sel_ = kSelScene; selEntity_ = e;
}

// Fills `c.subtree` with `e`'s descendants, parents before children, for a Destroy.
//
// CALLED FROM THE DELETE PATH ONLY, not from describeEntity: a Transform command has no use for
// this and snapshotting a whole subtree per drag would put real cost on the common case.
void SandboxApp::captureSubtree(EditCmd& c, scene::Entity e) {
    scene::World& w = scene::World::instance();
    std::vector<scene::Entity> nodes;
    collectSubtree(w, e, nodes);
    // nodes[0] is `e` itself, which the command already carries.
    std::unordered_map<u32, i32> indexOf;
    for (usize i = 1; i < nodes.size(); ++i) {
        const scene::Entity d = nodes[i];
        EditCmd::DestroyedNode n;
        n.id = editIdFor(d);
        const scene::Entity par = w.parent(d);
        const auto it = indexOf.find(static_cast<u32>(par));
        n.parent = it == indexOf.end() ? -1 : it->second;
        if (const auto* loc = w.component<scene::CLocal>(d, scene::kComponentLocal)) {
            n.xf.pos = loc->xf.position;
            n.xf.rotDeg = eulerDegFromQuat(loc->xf.rotation);
            n.xf.scale = loc->xf.scale;
        }
        if (const auto lb = entityLabels_.find(static_cast<u32>(d)); lb != entityLabels_.end())
            n.label = lb->second;
        n.snap = editor::captureEntity(w, d);
        // Same two non-component flags the root carries; a subtree restored without them has the
        // identical silent-solidify problem one level down.
        if (const auto ci = entityCollide_.find(static_cast<u32>(d)); ci != entityCollide_.end())
            n.hadCollide = ci->second;
        if (const auto si = entitySnapZ_.find(static_cast<u32>(d)); si != entitySnapZ_.end()) {
            n.hadSnapZ = true;
            // See captureEntity's own comment on the identical f64->f32 narrowing just above:
            // intended and safe, DestroyedNode::snapZ being f32 like every other undo-system field.
            n.snapZ    = static_cast<f32>(si->second);
        }
#  if AVER_MODULE_PHYSICS
        if (entityBodies_.find(static_cast<u32>(d)) != entityBodies_.end()) {
            n.hadBody = true;
            n.bodyHalf = n.xf.scale;
        }
#  endif
        indexOf.emplace(static_cast<u32>(d), static_cast<i32>(c.subtree.size()));
        c.subtree.push_back(std::move(n));
    }
}

#endif
#endif

#if AVER_MODULE_SCENE
// Removes an entity and everything the editor hung off it, including its static body.
//
// THE WHOLE SUBTREE, because World::destroy retires the whole subtree and this function used to
// forget only the entity it was handed. Every child's static body, label and collide flag stayed
// behind: the body went on colliding with nothing visible, and entityBodies_ kept a key for an
// entity id the world had retired and would eventually reissue. BodyRegistry's own header names
// THIS FUNCTION as the live offender and exists to stop it recurring -- see its detachSubtree.
//
// Unreachable until now only because nothing in the editor could make a child. A level file can,
// as of this branch, so it is reachable by opening one and pressing Delete.
//
// COLLECTED BEFORE THE DESTROY, not after: World::destroy queues the subtree for retirement and
// the hierarchy links are gone by the time it returns, so a walk afterwards finds nothing and
// reports a clean job it did not do.
void SandboxApp::destroyEntity(scene::Entity e) {
    scene::World& w = scene::World::instance();
    if (!w.valid(e)) return;

    std::vector<scene::Entity> doomed;
    collectSubtree(w, e, doomed);

    w.destroy(e);
    for (const scene::Entity d : doomed) {
        levelEntities_.erase(std::remove(levelEntities_.begin(), levelEntities_.end(), d),
                             levelEntities_.end());
        entityLabels_.erase(static_cast<u32>(d));
        entityCollide_.erase(static_cast<u32>(d));
        entitySnapZ_.erase(static_cast<u32>(d));
        // Deleting the marker must clear the cache, or Add > Player Start keeps refusing to add
        // one on the grounds that the level already has the entity that was just destroyed.
        if (d == playerStart_) playerStart_ = scene::kInvalidEntity;
        // AND THE SELECTION, if it named this entity. anySelected() validates the handle so a
        // stale one reads as "nothing selected", but selEntity_ kept pointing at a dead entity --
        // and every path that destroys without going through deleteSelection (the foliage eraser,
        // an undone Create, a level reload) left it that way. Cleared where the destruction
        // actually happens, so no caller can forget.
        if (sel_ == kSelScene && d == selEntity_) { sel_ = -1; selEntity_ = scene::kInvalidEntity; }
#if AVER_MODULE_PHYSICS
        if (const auto it = entityBodies_.find(static_cast<u32>(d)); it != entityBodies_.end()) {
            aver_phys_remove_body(it->second);
            levelBodies_.erase(std::remove(levelBodies_.begin(), levelBodies_.end(), it->second),
                               levelBodies_.end());
            entityBodies_.erase(it);
        }
#endif
    }
}

// Appends `e` and every descendant, parents before children.
//
// ITERATIVE, and `next` is read BEFORE the recursion in every walk of this shape in the tree
// (BodyRegistry::detachSubtree does the same) -- a child's sibling link is not safe to read
// after that child has been visited.
 void SandboxApp::collectSubtree(const scene::World& w, scene::Entity e,
                           std::vector<scene::Entity>& out) {
    if (!w.valid(e)) return;
    out.push_back(e);
    for (scene::Entity c = w.firstChild(e); c != scene::kInvalidEntity;) {
        const scene::Entity next = w.nextSibling(c);
        collectSubtree(w, c, out);
        c = next;
    }
}

#endif

// The serial at the top of the undo stack, or 0 for an untouched document. This IS the
// document's state as far as saving is concerned.
u64 SandboxApp::currentEditMark() const { return undoStack_.empty() ? 0ull : undoStack_.back().serial; }

bool SandboxApp::canUndo() const { return !undoStack_.empty(); }

bool SandboxApp::canRedo() const { return !redoStack_.empty(); }

// Reverses the newest command and moves it to the redo stack.
// CreateObj/DestroyObj address objects_ directly by the index captured when pushed: undo/redo only
// pop the stack's back() (strict LIFO), so any later entry is undone first and the vector is
// always back to the state it was captured in. This depends on one invariant: nothing outside the
// undo path may insert/erase objects_ without also clearing undoStack_/redoStack_.
// Keeps reflBeaconIndex_ pointing at the beacon when objects_ shifts underneath it.
// reflBeaconIndex_ is captured ONCE and never maintained: an erase BELOW it slid every later entry
// down one, so the schedule at the draw site drove SOME OTHER object's `visible` flag while the
// real beacon sat frozen -- the bounds check kept it in range so it never crashed, just measured
// the wrong thing.
// Erasing the beacon itself invalidates the index rather than sliding it: -1 is already "nothing
// to drive".
void SandboxApp::objectsErasedAt(int index) {
    if (reflBeaconIndex_ < 0) return;
    if (index == reflBeaconIndex_)     reflBeaconIndex_ = -1;
    else if (index <  reflBeaconIndex_) --reflBeaconIndex_;
}

void SandboxApp::objectsInsertedAt(int index) {
    if (reflBeaconIndex_ >= 0 && index <= reflBeaconIndex_) ++reflBeaconIndex_;
}

#if AVER_MODULE_LANDSCAPE && AVER_MODULE_SCENE
// Destroys everything a foliage stroke created. Used by undo on a paint stroke and by redo on an
// erase stroke -- the two are the same operation seen from opposite ends.
void SandboxApp::foliageBatchDestroy(const EditCmd& c) {
    for (EditId id : c.batchIds) {
        const scene::Entity ent = entityForEdit(id);
        if (ent != scene::kInvalidEntity) destroyEntity(ent);
    }
    sel_ = -1; selEntity_ = scene::kInvalidEntity;
}

// Recreates everything a foliage stroke destroyed, rebinding each snapshot to its old edit id so
// a second undo can find the entities again.
void SandboxApp::foliageBatchRecreate(EditCmd& c) {
    for (size_t i = 0; i < c.batchSnaps.size(); ++i) {
        EditCmd one;
        one.kind = EditCmd::Kind::Create;
        one.snap = c.batchSnaps[i];
        one.id   = i < c.batchIds.size() ? c.batchIds[i] : 0;
        recreateFrom(one);
        // recreateFrom rebinds the edit id to the NEW entity; carry that back so the batch stays
        // addressable across repeated undo/redo cycles rather than working exactly once.
        if (i < c.batchIds.size()) c.batchIds[i] = one.id;
    }
    sel_ = -1; selEntity_ = scene::kInvalidEntity;
}

#endif

void SandboxApp::undo() {
    // The Player Start's handle changes when an undo recreates it; see refreshPlayerStart.
    struct RefreshOnExit { SandboxApp* a; ~RefreshOnExit() { a->refreshPlayerStart(); } } ro{this};
    if (undoStack_.empty()) return;
    EditCmd c = undoStack_.back(); undoStack_.pop_back();
    switch (c.kind) {
        case EditCmd::Kind::Transform: applyXformTo(c, c.before, /*undoing=*/true); break;
#if AVER_MODULE_SCENE
        case EditCmd::Kind::Create:    destroyEntity(entityForEdit(c.id));
                                       sel_ = -1; selEntity_ = scene::kInvalidEntity; break;
        case EditCmd::Kind::Destroy:   recreateFrom(c); break;
        case EditCmd::Kind::Reparent:  applyReparentTo(c, c.reparentOldParentId, c.before); break;
#endif
#if AVER_MODULE_PBR
        case EditCmd::Kind::Material:  applyMaterialDesc(c.matHandle, c.matBefore); break;
#endif
#if AVER_MODULE_SCENE
        case EditCmd::Kind::Rename:    applyEntityLabel(c.id, c.renameBefore); break;
        case EditCmd::Kind::RemoveComponent: restoreComponent(c.id, c.removedComponent); break;
#endif
        case EditCmd::Kind::CreateObj:   // undo a create: take it back out
            if (c.objIndex >= 0 && c.objIndex < (int)objects_.size()) {
                objects_.erase(objects_.begin() + c.objIndex);
                objectsErasedAt(c.objIndex);
            }
            sel_ = -1; selEntity_ = kInvalidId;
            break;
        case EditCmd::Kind::DestroyObj:  // undo a destroy: put it back at its old index
            if (c.objIndex >= 0 && c.objIndex <= (int)objects_.size()) {
                objects_.insert(objects_.begin() + c.objIndex, c.objSnapshot);
                objectsInsertedAt(c.objIndex);
            }
            sel_ = c.objIndex; selEntity_ = kInvalidId;
            break;
#if AVER_MODULE_LANDSCAPE
        case EditCmd::Kind::LandscapeStroke: applyLandscapeRect(c, c.landBefore); break;
#endif
#if AVER_MODULE_LANDSCAPE && AVER_MODULE_SCENE
        // Undo of a PAINT removes what it made; undo of an ERASE puts it back.
        case EditCmd::Kind::FoliageStroke:
            if (c.batchWasErase) foliageBatchRecreate(c); else foliageBatchDestroy(c);
            break;
#endif
        default: break;   // Create/Destroy (scene) fall here when AVER_MODULE_SCENE is off
    }
    redoStack_.push_back(std::move(c));
}

// Re-applies the newest undone command and moves it back to the undo stack. See undo()'s own
// comment for why CreateObj/DestroyObj's raw objIndex addressing is safe.
void SandboxApp::redo() {
    struct RefreshOnExit { SandboxApp* a; ~RefreshOnExit() { a->refreshPlayerStart(); } } ro{this};
    if (redoStack_.empty()) return;
    EditCmd c = redoStack_.back(); redoStack_.pop_back();
    switch (c.kind) {
        case EditCmd::Kind::Transform: applyXformTo(c, c.after, /*undoing=*/false); break;
#if AVER_MODULE_SCENE
        case EditCmd::Kind::Create:    recreateFrom(c); break;
        case EditCmd::Kind::Destroy:   destroyEntity(entityForEdit(c.id));
                                       sel_ = -1; selEntity_ = scene::kInvalidEntity; break;
        case EditCmd::Kind::Reparent:  applyReparentTo(c, c.reparentNewParentId, c.after); break;
#endif
#if AVER_MODULE_PBR
        case EditCmd::Kind::Material:  applyMaterialDesc(c.matHandle, c.matAfter); break;
#endif
#if AVER_MODULE_SCENE
        case EditCmd::Kind::Rename:    applyEntityLabel(c.id, c.renameAfter); break;
        case EditCmd::Kind::RemoveComponent: removeComponentRaw(c.id, c.removedComponent.type); break;
#endif
        case EditCmd::Kind::CreateObj:   // redo a create: put it back
            if (c.objIndex >= 0 && c.objIndex <= (int)objects_.size())
                objects_.insert(objects_.begin() + c.objIndex, c.objSnapshot);
            sel_ = c.objIndex; selEntity_ = kInvalidId;
            break;
        case EditCmd::Kind::DestroyObj:  // redo a destroy: take it back out
            if (c.objIndex >= 0 && c.objIndex < (int)objects_.size()) {
                objects_.erase(objects_.begin() + c.objIndex);
                objectsErasedAt(c.objIndex);
            }
            sel_ = -1; selEntity_ = kInvalidId;
            break;
#if AVER_MODULE_LANDSCAPE
        case EditCmd::Kind::LandscapeStroke: applyLandscapeRect(c, c.landAfter); break;
#endif
#if AVER_MODULE_LANDSCAPE && AVER_MODULE_SCENE
        case EditCmd::Kind::FoliageStroke:
            if (c.batchWasErase) foliageBatchDestroy(c); else foliageBatchRecreate(c);
            break;
#endif
        default: break;   // Create/Destroy (scene) fall here when AVER_MODULE_SCENE is off
    }
    undoStack_.push_back(std::move(c));
}

// Returns what the status bar calls the current selection.
std::string SandboxApp::selectionLabel() const {
#if AVER_MODULE_SCENE
    if (sel_ == kSelScene && scene::World::instance().valid(selEntity_)) {
        const auto it = entityLabels_.find(static_cast<u32>(selEntity_));
        if (it != entityLabels_.end()) return it->second;
        const std::string nm = scene::World::instance().name(selEntity_);
        return nm.empty() ? ("Entity " + std::to_string((u32)selEntity_)) : nm;
    }
#endif
    if (movableSelected()) return objects_[sel_].name;
    if (sel_ == -2) return "Directional Light (Sun)";
    if (sel_ == -3) return "Sky + Atmosphere";
    if (sel_ == -4) return "Post Process";
    return "nothing selected";
}

// Returns the selection's largest half-extent in cm, for framing and gizmo sizing.
f32 SandboxApp::selectedRadius() const {
    EditXform x;
    if (!selectedXform(x)) return kEditorCubeHalf;
    const f32 s = std::fmax(std::fabs(x.scale.x), std::fmax(std::fabs(x.scale.y), std::fabs(x.scale.z)));
#if AVER_MODULE_SCENE
    if (sel_ == kSelScene) {
        // THE MESH'S OWN BOUNDS, NOT ITS SCALE. This returned `max(1, s)` -- the largest scale
        // COMPONENT -- and read no geometry at all. Almost every authored entity has scale 1, so
        // this was 1.0 for essentially the whole scene, and focusOnSelection's
        // `d = max(50, r/tan(30 deg)*1.6)` collapsed to a flat 50 cm. Press F on an imported
        // building and the camera lands inside it. F is the most-pressed navigation key in any
        // editor; it was wrong for every object bigger than a crate.
        //
        // The bounds were already there and already used three lines down by the placeholder
        // branch, and by pick() for its ray test -- CMeshRenderer carries aabbMin/aabbMax in
        // MESH space, so the half-extent is scaled into world space here the same way.
        scene::World& w = scene::World::instance();
        if (w.valid(selEntity_)) {
            if (const auto* mr = w.component<scene::CMeshRenderer>(selEntity_, scene::kComponentMeshRenderer)) {
                const f32 ex = (mr->aabbMax[0] - mr->aabbMin[0]) * std::fabs(x.scale.x);
                const f32 ey = (mr->aabbMax[1] - mr->aabbMin[1]) * std::fabs(x.scale.y);
                const f32 ez = (mr->aabbMax[2] - mr->aabbMin[2]) * std::fabs(x.scale.z);
                const f32 half = 0.5f * std::fmax(ex, std::fmax(ey, ez));
                // A ZERO EXTENT IS NOT A TINY OBJECT, it is a mesh whose bounds were never
                // filled in -- fall through to the scale rather than framing a point.
                if (half > 1e-3f) return std::fmax(1.0f, half);
            }
        }
        return std::fmax(1.0f, s);
    }
#endif
    if (movableSelected()) {
        const MeshObj& o = objects_[sel_];
        const Vec3 e{o.aabbMax.x - o.aabbMin.x, o.aabbMax.y - o.aabbMin.y, o.aabbMax.z - o.aabbMin.z};
        return std::fmax(1.0f, 0.5f * std::fmax(e.x, std::fmax(e.y, e.z)) * s);
    }
    return std::fmax(1.0f, s);
}

} // namespace aver
