// The engine-side classes an extension derives from or calls: Object, RefCounted, Resource, Node, Node3D, and the Engine,
// ClassDB, OS and ProjectSettings singletons. Everything else Godot has is outside the host (G3_GDEXT.md lists it).
#include "GdextHost.hpp"

#include <algorithm>
#include <cmath>

namespace aver::gdext {

template <class T> static T ld(const void* p) { T t; std::memcpy(&t, p, sizeof t); return t; }

#define NATIVE(fn) static void fn([[maybe_unused]] HostObject* self, [[maybe_unused]] const Var* const* a, [[maybe_unused]] int n, \
                                  [[maybe_unused]] Var* r, [[maybe_unused]] GDExtensionCallError* err)

static void setRet(Var* r, V&& v) { varDestroy(r); *r = v.v; varNil(&v.v); }
static std::string argStr(const Var* v) { V t; varAssign(&t.v, v); return t.toUtf8(); }
static int64_t argInt(const Var* v) { V t; varAssign(&t.v, v); return t.toInt(); }
static bool argBool(const Var* v) { return varBooleanize(v); }
static HostObject* argObj(const Var* v) { return varObject(v); }
static const float* fl(const Var* v) { return static_cast<const float*>(varPtr(v)); }

// ---------------------------------------------------------------------------------------------------
// Object

NATIVE(objGetClass) { setRet(r, V::ofString(self->cls)); }
NATIVE(objIsClass) { setRet(r, V::ofBool(classIsA(self->def, argStr(a[0])))); }
NATIVE(objGet) { getProperty(self, argStr(a[0]), r); }
NATIVE(objSet) { setProperty(self, argStr(a[0]), a[1]); }
NATIVE(objCall) {
    callByName(self, argStr(a[0]), a + 1, n - 1, r, err);
}
NATIVE(objCallDeferred) {
    std::vector<V> copy(size_t(std::max(n, 1)));
    for (int i = 0; i < n; ++i) varAssign(&copy[size_t(i)].v, a[i]);
    const uint64_t id = self->id;
    const int count = n;
    host().deferred.push_back([copy, id, count]() {
        HostObject* o = objectById(id);
        if (!o || count < 1) return;
        std::vector<const Var*> p;
        for (int i = 1; i < count; ++i) p.push_back(copy[size_t(i)].ptr());
        V ret; GDExtensionCallError e{};
        callByName(o, copy[0].toUtf8(), p.data(), count - 1, &ret.v, &e);
    });
}
NATIVE(objHasMethod) { setRet(r, V::ofBool(findMethod(self->def, argStr(a[0])) != nullptr)); }
NATIVE(objHasSignal) { setRet(r, V::ofBool(findSignal(self->def, argStr(a[0])) != nullptr)); }
NATIVE(objConnect) { setRet(r, V::ofInt(connectSignal(self, argStr(a[0]), a[1], uint32_t(argInt(a[2]))))); }
NATIVE(objDisconnect) { disconnectSignal(self, argStr(a[0]), a[1]); }
NATIVE(objIsConnected) {
    bool found = false;
    auto it = self->conns.find(argStr(a[0]));
    if (it != self->conns.end()) for (const Conn& c : it->second) found = found || varEqual(c.callable.ptr(), a[1]);
    setRet(r, V::ofBool(found));
}
NATIVE(objEmitSignal) { setRet(r, V::ofInt(emitSignal(self, argStr(a[0]), a + 1, n - 1))); }
NATIVE(objGetInstanceId) { setRet(r, V::ofInt(int64_t(self->id))); }
NATIVE(objToString) { setRet(r, V::ofString(objectToString(self))); }
NATIVE(objNotification) { sendNotification(self, int(argInt(a[0])), argBool(a[1])); }
NATIVE(objSetMeta) { self->meta[argStr(a[0])] = V(); varAssign(&self->meta[argStr(a[0])].v, a[1]); }
NATIVE(objGetMeta) {
    auto it = self->meta.find(argStr(a[0]));
    varAssign(r, it != self->meta.end() ? it->second.ptr() : a[1]);
}
NATIVE(objHasMeta) { setRet(r, V::ofBool(self->meta.count(argStr(a[0])) > 0)); }
NATIVE(objRemoveMeta) { self->meta.erase(argStr(a[0])); }
NATIVE(objGetScript) {}
NATIVE(objSetScript) {}
NATIVE(objIsQueued) { setRet(r, V::ofBool(self->queuedFree)); }

// RefCounted

NATIVE(rcInitRef) { self->refcount = 1; setRet(r, V::ofBool(true)); }
NATIVE(rcReference) {
    ++self->refcount;
    if (self->instance && self->def && self->def->f.reference) self->def->f.reference(self->instance);
    setRet(r, V::ofBool(true));
}
NATIVE(rcUnreference) {
    if (self->instance && self->def && self->def->f.unreference) self->def->f.unreference(self->instance);
    --self->refcount;
    setRet(r, V::ofBool(self->refcount <= 0));
}
NATIVE(rcGetCount) { setRet(r, V::ofInt(self->refcount)); }

// Resource

NATIVE(resSetPath) { self->resPath = argStr(a[0]); }
NATIVE(resGetPath) { setRet(r, V::ofString(self->resPath)); }
NATIVE(resSetName) { self->resName = argStr(a[0]); }
NATIVE(resGetName) { setRet(r, V::ofString(self->resName)); }
NATIVE(resEmitChanged) { emitSignal(self, "changed", nullptr, 0); }

// ---------------------------------------------------------------------------------------------------
// Node

static std::string uniqueChildName(HostObject* parent, const std::string& wanted, HostObject* except) {
    auto taken = [&](const std::string& s) {
        for (HostObject* c : parent->children) if (c != except && c->name == s) return true;
        return false;
    };
    if (!wanted.empty() && !taken(wanted)) return wanted;
    const std::string base = wanted.empty() ? "@" + std::string(except ? except->cls : "Node") + "@" : wanted + "@";
    for (int i = 2;; ++i) {
        std::string s = base + std::to_string(i);
        if (!taken(s)) return s;
    }
}

NATIVE(nodeGetName) { setRet(r, V::ofName(self->name)); }
NATIVE(nodeSetName) {
    std::string nm;
    for (char c : argStr(a[0])) if (c != '.' && c != ':' && c != '/' && c != '@' && c != '%') nm += c;
    if (self->parent) nm = uniqueChildName(self->parent, nm, self);
    if (nm == self->name) return;
    self->name = nm;
    emitSignal(self, "renamed", nullptr, 0);
}
NATIVE(nodeAddChild) {
    HostObject* c = argObj(a[0]);
    if (!c || c == self) { *err = {GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT, 0, 0}; hostLog(2, "add_child: invalid child"); return; }
    if (c->parent) { hostLog(2, "add_child: '" + c->name + "' already has a parent"); return; }
    for (HostObject* p = self; p; p = p->parent) if (p == c) { hostLog(2, "add_child: would make a cycle"); return; }
    c->name = uniqueChildName(self, c->name, c);
    c->parent = self;
    self->children.push_back(c);
    self->pendingAdds.push_back(c);
    V child = V::ofObject(c);
    const Var* args[1] = {child.ptr()};
    emitSignal(self, "child_entered_tree", args, 1);
    emitSignal(self, "child_order_changed", nullptr, 0);
}
NATIVE(nodeRemoveChild) {
    HostObject* c = argObj(a[0]);
    if (!c || c->parent != self) return;
    V child = V::ofObject(c);
    const Var* args[1] = {child.ptr()};
    emitSignal(self, "child_exiting_tree", args, 1);
    self->children.erase(std::remove(self->children.begin(), self->children.end(), c), self->children.end());
    self->pendingAdds.erase(std::remove(self->pendingAdds.begin(), self->pendingAdds.end(), c), self->pendingAdds.end());
    c->parent = nullptr;
    emitSignal(self, "child_order_changed", nullptr, 0);
}
NATIVE(nodeGetChild) {
    int64_t i = argInt(a[0]);
    if (i < 0) i += int64_t(self->children.size());
    if (i < 0 || i >= int64_t(self->children.size())) { hostLog(2, "get_child: index out of range"); return; }
    setRet(r, V::ofObject(self->children[size_t(i)]));
}
NATIVE(nodeGetChildCount) { setRet(r, V::ofInt(int64_t(self->children.size()))); }
NATIVE(nodeGetChildren) {
    V arr = makeArray();
    for (HostObject* c : self->children) {
        V o = V::ofObject(c);
        ld<ArrayData*>(arr.v.d)->items.emplace_back();
        varCopy(&ld<ArrayData*>(arr.v.d)->items.back(), o.ptr());
    }
    setRet(r, std::move(arr));
}
NATIVE(nodeGetParent) { setRet(r, V::ofObject(self->parent)); }

std::string nodePath(HostObject* o) {
    std::vector<std::string> parts;
    for (HostObject* p = o; p; p = p->parent) parts.push_back(p->name);
    std::string s = o->inTree ? "/root" : "";
    for (size_t i = parts.size(); i-- > 0;) s += (s.empty() ? "" : "/") + parts[i];
    return s;
}

HostObject* nodeAtPath(HostObject* from, const std::string& path) {
    HostObject* cur = from;
    size_t i = 0;
    if (!path.empty() && path[0] == '/') {
        while (cur->parent) cur = cur->parent;
        i = 1;
        if (path.compare(1, 4, "root") == 0 && (path.size() == 5 || path[5] == '/')) i = path.size() > 5 ? 6 : 5;
    }
    while (i <= path.size() && cur) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string tok = path.substr(i, j - i);
        i = j + 1;
        if (tok.empty() || tok == ".") continue;
        if (tok == "..") { cur = cur->parent; continue; }
        HostObject* next = nullptr;
        for (HostObject* c : cur->children) if (c->name == tok) { next = c; break; }
        cur = next;
    }
    return cur;
}
NATIVE(nodeGetPath) { setRet(r, V::ofString(nodePath(self))); r->type = T_NODE_PATH; }
NATIVE(nodeGetNode) {
    HostObject* o = nodeAtPath(self, argStr(a[0]));
    if (!o) hostLog(2, "get_node: node not found: '" + argStr(a[0]) + "' (relative to '" + self->name + "')");
    setRet(r, V::ofObject(o));
}
NATIVE(nodeGetNodeOrNull) { setRet(r, V::ofObject(nodeAtPath(self, argStr(a[0])))); }
NATIVE(nodeHasNode) { setRet(r, V::ofBool(nodeAtPath(self, argStr(a[0])) != nullptr)); }
NATIVE(nodeFindChild) {
    const std::string pat = argStr(a[0]);
    const bool recursive = argBool(a[1]);
    auto match = [&](const std::string& s) {
        if (pat == "*") return true;
        size_t star = pat.find('*');
        if (star == std::string::npos) return s == pat;
        return s.size() >= pat.size() - 1 && s.compare(0, star, pat, 0, star) == 0 && s.compare(s.size() - (pat.size() - star - 1), pat.size() - star - 1, pat, star + 1, std::string::npos) == 0;
    };
    std::vector<HostObject*> stack(self->children.rbegin(), self->children.rend());
    while (!stack.empty()) {
        HostObject* c = stack.back(); stack.pop_back();
        if (match(c->name)) { setRet(r, V::ofObject(c)); return; }
        if (recursive) for (auto it = c->children.rbegin(); it != c->children.rend(); ++it) stack.push_back(*it);
    }
}
NATIVE(nodeIsInsideTree) { setRet(r, V::ofBool(self->inTree)); }
NATIVE(nodeGetTree) {}
NATIVE(nodeGetIndex) {
    int64_t idx = -1;
    if (self->parent) for (size_t i = 0; i < self->parent->children.size(); ++i) if (self->parent->children[i] == self) idx = int64_t(i);
    setRet(r, V::ofInt(idx));
}
NATIVE(nodeMoveChild) {
    HostObject* c = argObj(a[0]);
    if (!c || c->parent != self) return;
    int64_t to = argInt(a[1]);
    auto& ch = self->children;
    if (to < 0) to += int64_t(ch.size());
    if (to < 0 || to >= int64_t(ch.size())) return;
    ch.erase(std::remove(ch.begin(), ch.end(), c), ch.end());
    ch.insert(ch.begin() + to, c);
    emitSignal(self, "child_order_changed", nullptr, 0);
}
NATIVE(nodeGetOwner) { setRet(r, V::ofObject(self->owner)); }
NATIVE(nodeSetOwner) { self->owner = argObj(a[0]); }
NATIVE(nodeAddToGroup) {
    std::string g = argStr(a[0]);
    if (std::find(self->groups.begin(), self->groups.end(), g) == self->groups.end()) self->groups.push_back(g);
}
NATIVE(nodeRemoveFromGroup) { std::string g = argStr(a[0]); self->groups.erase(std::remove(self->groups.begin(), self->groups.end(), g), self->groups.end()); }
NATIVE(nodeIsInGroup) { std::string g = argStr(a[0]); setRet(r, V::ofBool(std::find(self->groups.begin(), self->groups.end(), g) != self->groups.end())); }
NATIVE(nodeGetGroups) {
    V arr = makeArray();
    for (const std::string& g : self->groups) {
        V s = V::ofName(g);
        ld<ArrayData*>(arr.v.d)->items.emplace_back();
        varCopy(&ld<ArrayData*>(arr.v.d)->items.back(), s.ptr());
    }
    setRet(r, std::move(arr));
}
NATIVE(nodeQueueFree) {
    if (self->queuedFree) return;
    self->queuedFree = true;
    const uint64_t id = self->id;
    host().deferred.push_back([id]() { if (HostObject* o = objectById(id)) destroyObject(o); });
}
NATIVE(nodeSetProcess) { self->process = argBool(a[0]); }
NATIVE(nodeIsProcessing) { setRet(r, V::ofBool(self->process)); }
NATIVE(nodeSetPhysicsProcess) { self->physics = argBool(a[0]); }
NATIVE(nodeIsPhysicsProcessing) { setRet(r, V::ofBool(self->physics)); }
NATIVE(nodeSetProcessMode) { self->processMode = int(argInt(a[0])); }
NATIVE(nodeGetProcessMode) { setRet(r, V::ofInt(self->processMode)); }
NATIVE(nodeIsReady) { setRet(r, V::ofBool(self->ready)); }

void treeEnter(HostObject* o, bool recursive) {
    if (!o || o->freed || o->inTree) return;
    o->inTree = true;
    sendNotification(o, 10, false);                      // NOTIFICATION_ENTER_TREE
    callVirtual(o, "_enter_tree", nullptr, nullptr);
    emitSignal(o, "tree_entered", nullptr, 0);
    if (recursive) for (HostObject* c : std::vector<HostObject*>(o->children)) treeEnter(c, true);
}

void treeReady(HostObject* o, bool recursive) {
    if (!o || o->freed) return;
    if (recursive) for (HostObject* c : std::vector<HostObject*>(o->children)) treeReady(c, true);
    if (o->ready) return;
    o->ready = true;
    if (findVirtual(o, "_process")) o->process = true;
    if (findVirtual(o, "_physics_process")) o->physics = true;
    sendNotification(o, 13, false);                      // NOTIFICATION_READY
    callVirtual(o, "_ready", nullptr, nullptr);
    emitSignal(o, "ready", nullptr, 0);
}

void treeExit(HostObject* o, bool recursive) {
    if (!o || o->freed || !o->inTree) return;
    if (recursive) {
        std::vector<HostObject*> kids(o->children);
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) treeExit(*it, true);
    }
    emitSignal(o, "tree_exiting", nullptr, 0);
    sendNotification(o, 11, false);                      // NOTIFICATION_EXIT_TREE
    callVirtual(o, "_exit_tree", nullptr, nullptr);
    o->inTree = false;
    o->ready = false;
    emitSignal(o, "tree_exited", nullptr, 0);
}

void treeProcess(HostObject* o, double delta, bool physics, bool recursive) {
    if (!o || o->freed || !o->inTree) return;
    if (physics ? o->physics : o->process) {
        const void* args[1] = {&delta};
        callVirtual(o, physics ? "_physics_process" : "_process", args, nullptr);
    }
    if (recursive) for (HostObject* c : std::vector<HostObject*>(o->children)) treeProcess(c, delta, physics, true);
}

// ---------------------------------------------------------------------------------------------------
// Node3D (a local transform; the global one composes the Node3D ancestors)

namespace {
struct M3 { double m[3][3]; };

M3 rows(const float* f) { M3 r; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r.m[i][j] = f[i * 3 + j]; return r; }
void putRows(float* f, const M3& r) { for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) f[i * 3 + j] = float(r.m[i][j]); }
M3 mul(const M3& a, const M3& b) {
    M3 r;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}
double colLen(const M3& b, int c) { return std::sqrt(b.m[0][c] * b.m[0][c] + b.m[1][c] * b.m[1][c] + b.m[2][c] * b.m[2][c]); }

M3 fromEulerYXZ(double x, double y, double z) {
    const double cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y), cz = std::cos(z), sz = std::sin(z);
    M3 rx{{{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}}}, ry{{{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}}}, rz{{{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}}};
    return mul(mul(ry, rx), rz);
}

void eulerYXZ(const M3& b, double out[3]) {
    const double m12 = b.m[1][2];
    const double eps = 1e-5, half = 1.5707963267948966;
    if (m12 < 1 - eps) {
        if (m12 > -(1 - eps)) {
            if (b.m[1][0] == 0 && b.m[0][1] == 0 && b.m[0][2] == 0 && b.m[2][0] == 0 && b.m[0][0] == 1) {
                out[0] = std::atan2(-m12, b.m[1][1]); out[1] = 0; out[2] = 0;
            } else {
                out[0] = std::asin(-m12); out[1] = std::atan2(b.m[0][2], b.m[2][2]); out[2] = std::atan2(b.m[1][0], b.m[1][1]);
            }
        } else { out[0] = half; out[1] = std::atan2(b.m[0][1], b.m[0][0]); out[2] = 0; }
    } else { out[0] = -half; out[1] = -std::atan2(b.m[0][1], b.m[0][0]); out[2] = 0; }
}
}

void transformPush(HostObject* o, const float* xform12) {
    if (!o) return;
    if (std::memcmp(o->xform, xform12, sizeof o->xform) == 0) return;
    std::memcpy(o->xform, xform12, sizeof o->xform);
    ++o->xformVersion;
}

static void globalOf(HostObject* o, float out[12]) {
    std::memcpy(out, o->xform, sizeof o->xform);
    for (HostObject* p = o->parent; p && classIsA(p->def, "Node3D"); p = p->parent) {
        M3 pb = rows(p->xform), lb = rows(out);
        const double lo[3] = {out[9], out[10], out[11]};
        M3 g = mul(pb, lb);
        float res[12];
        putRows(res, g);
        for (int i = 0; i < 3; ++i)
            res[9 + i] = float(p->xform[9 + i] + pb.m[i][0] * lo[0] + pb.m[i][1] * lo[1] + pb.m[i][2] * lo[2]);
        std::memcpy(out, res, sizeof res);
    }
}

static V vec3(double x, double y, double z) { return V::ofVec(T_VECTOR3, {float(x), float(y), float(z)}); }
static V xformV(const float* f) { return V::ofNative(T_TRANSFORM3D, f); }

NATIVE(n3SetPosition) { std::memcpy(self->xform + 9, fl(a[0]), 12); ++self->xformVersion; }
NATIVE(n3GetPosition) { setRet(r, vec3(self->xform[9], self->xform[10], self->xform[11])); }
NATIVE(n3SetScale) {
    M3 b = rows(self->xform);
    const float* s = fl(a[0]);
    for (int c = 0; c < 3; ++c) { double l = colLen(b, c); for (int i = 0; i < 3; ++i) b.m[i][c] = l > 0 ? b.m[i][c] / l * s[c] : 0; }
    putRows(self->xform, b);
    ++self->xformVersion;
}
NATIVE(n3GetScale) { M3 b = rows(self->xform); setRet(r, vec3(colLen(b, 0), colLen(b, 1), colLen(b, 2))); }
NATIVE(n3SetRotation) {
    M3 b = rows(self->xform);
    const double sx = colLen(b, 0), sy = colLen(b, 1), sz = colLen(b, 2);
    const float* e = fl(a[0]);
    M3 rot = fromEulerYXZ(e[0], e[1], e[2]);
    for (int i = 0; i < 3; ++i) { rot.m[i][0] *= sx; rot.m[i][1] *= sy; rot.m[i][2] *= sz; }
    putRows(self->xform, rot);
    ++self->xformVersion;
}
NATIVE(n3GetRotation) {
    M3 b = rows(self->xform);
    for (int c = 0; c < 3; ++c) { double l = colLen(b, c); for (int i = 0; i < 3; ++i) b.m[i][c] = l > 0 ? b.m[i][c] / l : 0; }
    double e[3];
    eulerYXZ(b, e);
    setRet(r, vec3(e[0], e[1], e[2]));
}
NATIVE(n3SetTransform) { std::memcpy(self->xform, varPtr(a[0]), sizeof self->xform); ++self->xformVersion; }
NATIVE(n3GetTransform) { setRet(r, xformV(self->xform)); }
NATIVE(n3GetGlobalTransform) { float g[12]; globalOf(self, g); setRet(r, xformV(g)); }
NATIVE(n3GetGlobalPosition) { float g[12]; globalOf(self, g); setRet(r, vec3(g[9], g[10], g[11])); }
NATIVE(n3Translate) { const float* d = fl(a[0]); for (int i = 0; i < 3; ++i) self->xform[9 + i] += d[i]; ++self->xformVersion; }
NATIVE(n3SetVisible) { self->visible = argBool(a[0]); emitSignal(self, "visibility_changed", nullptr, 0); }
NATIVE(n3IsVisible) { setRet(r, V::ofBool(self->visible)); }

// ---------------------------------------------------------------------------------------------------
// singletons

NATIVE(engIsEditorHint) { setRet(r, V::ofBool(false)); }
NATIVE(engZero) { setRet(r, V::ofInt(0)); }
NATIVE(engPhysicsTicks) { setRet(r, V::ofInt(60)); }
NATIVE(engFps) { setRet(r, V::ofFloat(60.0)); }
NATIVE(engVersionInfo) {
    V d = makeDict();
    auto put = [&](const char* k, V&& v) { V key = V::ofString(k); varAssign(dictSlot(ld<DictData*>(d.v.d), key.ptr()), v.ptr()); };
    put("major", V::ofInt(4)); put("minor", V::ofInt(5)); put("patch", V::ofInt(0)); put("hex", V::ofInt(0x040500));
    put("status", V::ofString("stable")); put("build", V::ofString("aver.fabric")); put("string", V::ofString("Godot Engine v4.5.stable (Aver Fabric host)"));
    setRet(r, std::move(d));
}
NATIVE(engHasSingleton) { setRet(r, V::ofBool(host().singletons.count(argStr(a[0])) > 0)); }
NATIVE(engGetSingleton) { auto it = host().singletons.find(argStr(a[0])); if (it != host().singletons.end()) setRet(r, V::ofObject(it->second)); }

NATIVE(cdbClassExists) { setRet(r, V::ofBool(findClass(argStr(a[0])) != nullptr)); }
NATIVE(cdbCanInstantiate) { ClassDef* c = findClass(argStr(a[0])); setRet(r, V::ofBool(c && !c->isVirtual && !c->isAbstract)); }
NATIVE(cdbInstantiate) { setRet(r, V::ofObject(instantiate(argStr(a[0])))); }
NATIVE(cdbGetParent) { ClassDef* c = findClass(argStr(a[0])); setRet(r, V::ofName(c ? c->parent : "")); }
NATIVE(cdbIsParent) { ClassDef* c = findClass(argStr(a[0])); setRet(r, V::ofBool(c && classIsA(c, argStr(a[1])))); }

NATIVE(osGetName) {
#if defined(_WIN32)
    setRet(r, V::ofString("Windows"));
#elif defined(__APPLE__)
    setRet(r, V::ofString("macOS"));
#else
    setRet(r, V::ofString("Linux"));
#endif
}
NATIVE(osIsDebug) {
#if defined(NDEBUG)
    setRet(r, V::ofBool(false));
#else
    setRet(r, V::ofBool(true));
#endif
}
NATIVE(psGetSetting) { varAssign(r, a[1]); }
NATIVE(psHasSetting) { setRet(r, V::ofBool(false)); }

// ---------------------------------------------------------------------------------------------------

static void withDefaults(MethodDef* m, std::initializer_list<V> d) { for (const V& v : d) m->defaults.push_back(v); }

static void addSignal(ClassDef* c, const char* name, std::initializer_list<const char*> argTypes = {}) {
    SignalDef s;
    s.name = name;
    int i = 0;
    for (const char* t : argTypes) { ArgInfo a; a.type = typeFromName(t); a.name = "arg" + std::to_string(i++); s.args.push_back(a); }
    c->signals.push_back(s);
}

static void addProp(ClassDef* c, const char* name, const char* type, const char* setter, const char* getter) {
    PropDef p;
    p.info.name = name;
    p.info.type = typeFromName(type);
    p.setter = setter;
    p.getter = getter;
    c->props.push_back(p);
}

void ensureEngineClasses() {
    Host& h = host();
    if (h.engineReady) return;
    h.engineReady = true;

    ClassDef* object = defineEngineClass("Object", "", false);
    defineNative(object, "get_class", "", "String", objGetClass);
    defineNative(object, "is_class", "String", "bool", objIsClass);
    defineNative(object, "get", "StringName", "Variant", objGet);
    defineNative(object, "set", "StringName Variant", "", objSet);
    defineNative(object, "call", "StringName", "Variant", objCall, true);
    defineNative(object, "call_deferred", "StringName", "Variant", objCallDeferred, true);
    defineNative(object, "has_method", "StringName", "bool", objHasMethod);
    defineNative(object, "has_signal", "StringName", "bool", objHasSignal);
    withDefaults(defineNative(object, "connect", "StringName Callable int", "int", objConnect), {V::ofInt(0)});
    defineNative(object, "disconnect", "StringName Callable", "", objDisconnect);
    defineNative(object, "is_connected", "StringName Callable", "bool", objIsConnected);
    defineNative(object, "emit_signal", "StringName", "int", objEmitSignal, true);
    defineNative(object, "get_instance_id", "", "int", objGetInstanceId);
    defineNative(object, "to_string", "", "String", objToString);
    withDefaults(defineNative(object, "notification", "int bool", "", objNotification), {V::ofBool(false)});
    defineNative(object, "set_meta", "StringName Variant", "", objSetMeta);
    withDefaults(defineNative(object, "get_meta", "StringName Variant", "Variant", objGetMeta), {V()});
    defineNative(object, "has_meta", "StringName", "bool", objHasMeta);
    defineNative(object, "remove_meta", "StringName", "", objRemoveMeta);
    defineNative(object, "get_script", "", "Variant", objGetScript);
    defineNative(object, "set_script", "Variant", "", objSetScript);
    defineNative(object, "is_queued_for_deletion", "", "bool", objIsQueued);
    addSignal(object, "script_changed");
    addSignal(object, "property_list_changed");

    ClassDef* rc = defineEngineClass("RefCounted", "Object", true);
    defineNative(rc, "init_ref", "", "bool", rcInitRef);
    defineNative(rc, "reference", "", "bool", rcReference);
    defineNative(rc, "unreference", "", "bool", rcUnreference);
    defineNative(rc, "get_reference_count", "", "int", rcGetCount);

    ClassDef* res = defineEngineClass("Resource", "RefCounted", true);
    defineNative(res, "set_path", "String", "", resSetPath);
    defineNative(res, "get_path", "", "String", resGetPath);
    defineNative(res, "take_over_path", "String", "", resSetPath);
    defineNative(res, "set_name", "String", "", resSetName);
    defineNative(res, "get_name", "", "String", resGetName);
    defineNative(res, "emit_changed", "", "", resEmitChanged);
    addSignal(res, "changed");
    addProp(res, "resource_path", "String", "set_path", "get_path");
    addProp(res, "resource_name", "String", "set_name", "get_name");

    ClassDef* node = defineEngineClass("Node", "Object", false);
    defineNative(node, "get_name", "", "StringName", nodeGetName);
    defineNative(node, "set_name", "StringName", "", nodeSetName);
    withDefaults(defineNative(node, "add_child", "Node bool int", "", nodeAddChild), {V::ofBool(false), V::ofInt(0)});
    defineNative(node, "remove_child", "Node", "", nodeRemoveChild);
    withDefaults(defineNative(node, "get_child", "int bool", "Node", nodeGetChild), {V::ofBool(false)});
    withDefaults(defineNative(node, "get_child_count", "bool", "int", nodeGetChildCount), {V::ofBool(false)});
    withDefaults(defineNative(node, "get_children", "bool", "Array", nodeGetChildren), {V::ofBool(false)});
    defineNative(node, "get_parent", "", "Node", nodeGetParent);
    defineNative(node, "get_path", "", "NodePath", nodeGetPath);
    defineNative(node, "get_node", "NodePath", "Node", nodeGetNode);
    defineNative(node, "get_node_or_null", "NodePath", "Node", nodeGetNodeOrNull);
    defineNative(node, "has_node", "NodePath", "bool", nodeHasNode);
    withDefaults(defineNative(node, "find_child", "String bool bool", "Node", nodeFindChild), {V::ofBool(true), V::ofBool(true)});
    defineNative(node, "is_inside_tree", "", "bool", nodeIsInsideTree);
    defineNative(node, "get_tree", "", "Object", nodeGetTree);
    withDefaults(defineNative(node, "get_index", "bool", "int", nodeGetIndex), {V::ofBool(false)});
    defineNative(node, "move_child", "Node int", "", nodeMoveChild);
    defineNative(node, "get_owner", "", "Node", nodeGetOwner);
    defineNative(node, "set_owner", "Node", "", nodeSetOwner);
    withDefaults(defineNative(node, "add_to_group", "StringName bool", "", nodeAddToGroup), {V::ofBool(false)});
    defineNative(node, "remove_from_group", "StringName", "", nodeRemoveFromGroup);
    defineNative(node, "is_in_group", "StringName", "bool", nodeIsInGroup);
    defineNative(node, "get_groups", "", "Array", nodeGetGroups);
    defineNative(node, "queue_free", "", "", nodeQueueFree);
    defineNative(node, "set_process", "bool", "", nodeSetProcess);
    defineNative(node, "is_processing", "", "bool", nodeIsProcessing);
    defineNative(node, "set_physics_process", "bool", "", nodeSetPhysicsProcess);
    defineNative(node, "is_physics_processing", "", "bool", nodeIsPhysicsProcessing);
    defineNative(node, "set_process_mode", "int", "", nodeSetProcessMode);
    defineNative(node, "get_process_mode", "", "int", nodeGetProcessMode);
    defineNative(node, "is_node_ready", "", "bool", nodeIsReady);
    for (const char* s : {"ready", "renamed", "tree_entered", "tree_exiting", "tree_exited", "child_order_changed"}) addSignal(node, s);
    addSignal(node, "child_entered_tree", {"Node"});
    addSignal(node, "child_exiting_tree", {"Node"});
    addProp(node, "name", "StringName", "set_name", "get_name");
    addProp(node, "process_mode", "int", "set_process_mode", "get_process_mode");

    ClassDef* n3 = defineEngineClass("Node3D", "Node", false);
    defineNative(n3, "set_position", "Vector3", "", n3SetPosition);
    defineNative(n3, "get_position", "", "Vector3", n3GetPosition);
    defineNative(n3, "set_rotation", "Vector3", "", n3SetRotation);
    defineNative(n3, "get_rotation", "", "Vector3", n3GetRotation);
    defineNative(n3, "set_scale", "Vector3", "", n3SetScale);
    defineNative(n3, "get_scale", "", "Vector3", n3GetScale);
    defineNative(n3, "set_transform", "Transform3D", "", n3SetTransform);
    defineNative(n3, "get_transform", "", "Transform3D", n3GetTransform);
    defineNative(n3, "get_global_transform", "", "Transform3D", n3GetGlobalTransform);
    defineNative(n3, "get_global_position", "", "Vector3", n3GetGlobalPosition);
    defineNative(n3, "translate", "Vector3", "", n3Translate);
    defineNative(n3, "set_visible", "bool", "", n3SetVisible);
    defineNative(n3, "is_visible", "", "bool", n3IsVisible);
    addSignal(n3, "visibility_changed");
    addProp(n3, "position", "Vector3", "set_position", "get_position");
    addProp(n3, "rotation", "Vector3", "set_rotation", "get_rotation");
    addProp(n3, "scale", "Vector3", "set_scale", "get_scale");
    addProp(n3, "transform", "Transform3D", "set_transform", "get_transform");
    addProp(n3, "visible", "bool", "set_visible", "is_visible");

    ClassDef* engine = defineEngineClass("Engine", "Object", false);
    engine->isAbstract = true;
    defineNative(engine, "is_editor_hint", "", "bool", engIsEditorHint);
    defineNative(engine, "get_frames_drawn", "", "int", engZero);
    defineNative(engine, "get_process_frames", "", "int", engZero);
    defineNative(engine, "get_physics_frames", "", "int", engZero);
    defineNative(engine, "get_physics_ticks_per_second", "", "int", engPhysicsTicks);
    defineNative(engine, "get_frames_per_second", "", "float", engFps);
    defineNative(engine, "get_version_info", "", "Dictionary", engVersionInfo);
    defineNative(engine, "has_singleton", "StringName", "bool", engHasSingleton);
    defineNative(engine, "get_singleton", "StringName", "Object", engGetSingleton);

    ClassDef* cdb = defineEngineClass("ClassDB", "Object", false);
    cdb->isAbstract = true;
    defineNative(cdb, "class_exists", "StringName", "bool", cdbClassExists);
    defineNative(cdb, "can_instantiate", "StringName", "bool", cdbCanInstantiate);
    defineNative(cdb, "instantiate", "StringName", "Variant", cdbInstantiate);
    defineNative(cdb, "get_parent_class", "StringName", "StringName", cdbGetParent);
    defineNative(cdb, "is_parent_class", "StringName StringName", "bool", cdbIsParent);

    ClassDef* os = defineEngineClass("OS", "Object", false);
    os->isAbstract = true;
    defineNative(os, "get_name", "", "String", osGetName);
    defineNative(os, "is_debug_build", "", "bool", osIsDebug);

    ClassDef* ps = defineEngineClass("ProjectSettings", "Object", false);
    ps->isAbstract = true;
    withDefaults(defineNative(ps, "get_setting", "String Variant", "Variant", psGetSetting), {V()});
    defineNative(ps, "has_setting", "String", "bool", psHasSetting);

    for (ClassDef* c : {engine, cdb, os, ps}) h.singletons[c->name] = newObject(c);
}

} // namespace aver::gdext
