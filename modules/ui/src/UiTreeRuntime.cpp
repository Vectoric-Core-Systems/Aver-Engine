// UiTree: the player for a layout's animations, the binding loop and the event routes.
#include "aver/ui/UiProps.hpp"
#include "aver/ui/UiTree.hpp"

#include <algorithm>
#include <cmath>

namespace aver::ui {

namespace {

// A binding path and a notified path are related when one is the other or its parent ("Player" ~ "Player.Score").
bool related(std::string_view a, std::string_view b) {
    if (a.size() > b.size()) std::swap(a, b);
    if (b.compare(0, a.size(), a) != 0) return false;
    return a.size() == b.size() || b[a.size()] == '.' || b[a.size()] == '[';
}

} // namespace

// ---- runtime registry --------------------------------------------------------------------------------

UiWidgetId UiTree::runtimeRoot(UiWidgetId id) const {
    if (runtimes_.count(id)) return id;
    for (const UiWidget* w = get(id); w; w = w->parent ? get(w->parent) : nullptr)
        if (runtimes_.count(w->id)) return w->id;
    return 0;
}

void UiTree::attachRuntime(UiWidgetId root, UiLayoutRuntime rt) {
    if (!get(root)) return;
    UiLayoutRuntime& dst = runtimes_[root];
    for (UiAnimation& a : rt.anims) dst.anims.push_back(std::move(a));
    for (UiBinding& b : rt.binds) dst.binds.push_back(std::move(b));
    for (UiEventRoute& r : rt.routes) dst.routes.push_back(std::move(r));
}

const UiLayoutRuntime* UiTree::runtimeOf(UiWidgetId root) const {
    const UiWidgetId r = runtimeRoot(root);
    const auto it = runtimes_.find(r);
    return it == runtimes_.end() ? nullptr : &it->second;
}

const UiAnimation* UiTree::findAnimation(UiWidgetId root, std::string_view name) const {
    const UiLayoutRuntime* rt = runtimeOf(root);
    if (!rt) return nullptr;
    for (const UiAnimation& a : rt->anims) if (a.name == name) return &a;
    return nullptr;
}

UiAnimation* UiTree::animOf(UiWidgetId root, std::string_view name) {
    return const_cast<UiAnimation*>(static_cast<const UiTree*>(this)->findAnimation(root, name));
}

void UiTree::dropRuntime(const std::vector<UiWidgetId>& doomed) {
    const auto gone = [&](UiWidgetId x) { return std::find(doomed.begin(), doomed.end(), x) != doomed.end(); };
    for (const UiWidgetId d : doomed) { runtimes_.erase(d); hoverSeen_.erase(d); }
    animRuns_.erase(std::remove_if(animRuns_.begin(), animRuns_.end(), [&](const AnimRun& r) { return gone(r.root); }),
                    animRuns_.end());
    // Routes and bindings of a surviving root that name a doomed widget.
    for (auto& kv : runtimes_) {
        auto& rt = kv.second;
        rt.binds.erase(std::remove_if(rt.binds.begin(), rt.binds.end(), [&](const UiBinding& b) { return gone(b.widget); }), rt.binds.end());
        rt.routes.erase(std::remove_if(rt.routes.begin(), rt.routes.end(), [&](const UiEventRoute& r) { return gone(r.widget); }), rt.routes.end());
    }
}

// ---- animation -----------------------------------------------------------------------------------------

void UiTree::applyAnim(usize index) {
    AnimRun& run = animRuns_[index];
    const UiAnimation* a = findAnimation(run.root, run.name);
    if (!a) return;
    for (usize i = 0; i < a->tracks.size() && i < run.widgets.size(); ++i) {
        UiWidget* w = get(run.widgets[i]);
        UiAnimValue v;
        if (w && uiAnimSample(a->tracks[i], run.time, v)) uiAnimApply(*w, a->tracks[i].prop, v);
    }
}

bool UiTree::animPlay(UiWidgetId rootIn, std::string_view name, const UiAnimPlay& opt) {
    const UiWidgetId root = runtimeRoot(rootIn);
    const UiAnimation* a = findAnimation(root, name);
    if (!a) return false;
    animRuns_.erase(std::remove_if(animRuns_.begin(), animRuns_.end(), [&](const AnimRun& r) { return r.root == root && r.name == name; }),
                    animRuns_.end());
    AnimRun run;
    run.root = root;
    run.name = std::string(name);
    run.speed = std::max(0.0f, opt.speed);
    run.loops = std::max(0, opt.loops);
    run.pingPong = opt.pingPong;
    run.dir = opt.reverse ? -1.0f : 1.0f;
    run.time = opt.reverse ? a->length() : 0.0f;
    for (const UiAnimTrack& t : a->tracks) run.widgets.push_back(find(t.widget, root));
    animRuns_.push_back(std::move(run));
    applyAnim(animRuns_.size() - 1);
    return true;
}

bool UiTree::animStop(UiWidgetId rootIn, std::string_view name, bool release) {
    const UiWidgetId root = runtimeRoot(rootIn);
    const UiAnimation* a = findAnimation(root, name);
    if (!a) return false;
    animRuns_.erase(std::remove_if(animRuns_.begin(), animRuns_.end(), [&](const AnimRun& r) { return r.root == root && r.name == name; }),
                    animRuns_.end());
    if (release)
        for (const UiAnimTrack& t : a->tracks)
            if (UiWidget* w = get(find(t.widget, root))) uiAnimRelease(*w, t.prop);
    return true;
}

bool UiTree::animSeek(UiWidgetId rootIn, std::string_view name, f32 time) {
    const UiWidgetId root = runtimeRoot(rootIn);
    const UiAnimation* a = findAnimation(root, name);
    if (!a) return false;
    usize i = 0;
    while (i < animRuns_.size() && !(animRuns_[i].root == root && animRuns_[i].name == name)) ++i;
    if (i == animRuns_.size()) {
        AnimRun run;
        run.root = root;
        run.name = std::string(name);
        run.paused = true;
        for (const UiAnimTrack& t : a->tracks) run.widgets.push_back(find(t.widget, root));
        animRuns_.push_back(std::move(run));
    }
    animRuns_[i].time = std::clamp(time, 0.0f, a->length());
    applyAnim(i);
    return true;
}

bool UiTree::animReverse(UiWidgetId rootIn, std::string_view name) {
    const UiWidgetId root = runtimeRoot(rootIn);
    const UiAnimation* a = findAnimation(root, name);
    if (!a) return false;
    for (usize i = 0; i < animRuns_.size(); ++i) {
        if (animRuns_[i].root == root && animRuns_[i].name == name) {
            animRuns_[i].dir = -animRuns_[i].dir;
            animRuns_[i].paused = false;
            return true;
        }
    }
    UiAnimPlay p;
    p.reverse = true;
    return animPlay(root, name, p);
}

bool UiTree::animPause(UiWidgetId rootIn, std::string_view name, bool paused) {
    const UiWidgetId root = runtimeRoot(rootIn);
    for (AnimRun& r : animRuns_)
        if (r.root == root && r.name == name) { r.paused = paused; return true; }
    return false;
}

bool UiTree::animPlaying(UiWidgetId rootIn, std::string_view name) const {
    const UiWidgetId root = runtimeRoot(rootIn);
    for (const AnimRun& r : animRuns_)
        if (r.root == root && r.name == name) return !r.paused;
    return false;
}

f32 UiTree::animTime(UiWidgetId rootIn, std::string_view name) const {
    const UiWidgetId root = runtimeRoot(rootIn);
    for (const AnimRun& r : animRuns_)
        if (r.root == root && r.name == name) return r.time;
    return -1.0f;
}

void UiTree::animAdvance(f32 dt) {
    if (animRuns_.empty()) return;
    std::vector<UiEvent> finished;
    for (usize i = 0; i < animRuns_.size();) {
        AnimRun& r = animRuns_[i];
        const UiAnimation* a = findAnimation(r.root, r.name);
        if (!a || !get(r.root)) { animRuns_.erase(animRuns_.begin() + static_cast<std::ptrdiff_t>(i)); continue; }
        if (r.paused) { ++i; continue; }
        const f32 len = a->length();
        r.time += r.dir * r.speed * std::max(0.0f, dt);
        bool end = false;
        for (int guard = 0; guard < 256; ++guard) {
            if (len <= 0.0f) { r.time = 0.0f; ++r.done; end = true; break; }
            if (r.dir > 0.0f && r.time >= len) {
                ++r.done;
                if (r.loops != 0 && r.done >= r.loops) { r.time = len; end = true; break; }
                if (r.pingPong) { r.time = 2.0f * len - r.time; r.dir = -1.0f; } else r.time -= len;
            } else if (r.dir < 0.0f && r.time <= 0.0f) {
                ++r.done;
                if (r.loops != 0 && r.done >= r.loops) { r.time = 0.0f; end = true; break; }
                if (r.pingPong) { r.time = -r.time; r.dir = 1.0f; } else r.time += len;
            } else {
                break;
            }
            if (guard == 255) r.time = std::clamp(r.time, 0.0f, len);
        }
        applyAnim(i);
        if (end) {
            UiEvent e;
            e.type = UiEventType::AnimationFinished;
            e.widget = r.root;
            e.text = r.name;
            e.value = r.time;
            e.index = r.done;
            finished.push_back(std::move(e));
            animRuns_.erase(animRuns_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    for (UiEvent& e : finished) emit(std::move(e));
}

// ---- bindings --------------------------------------------------------------------------------------------

void UiTree::bindingsChanged(std::string_view path) {
    for (auto& kv : runtimes_)
        for (UiBinding& b : kv.second.binds)
            if (path.empty() || related(b.path, path)) b.dirty = true;
}

bool UiTree::bind(UiWidgetId widget, std::string_view property, UiBindSource source, std::string_view path,
                  std::string_view format, bool onChange) {
    const UiWidgetId root = rootOf(widget);
    if (!root || !uiFindProperty(property) || path.empty()) return false;
    UiBinding b;
    b.widget = widget;
    b.property = std::string(property);
    b.source = source;
    b.path = std::string(path);
    b.format = std::string(format);
    b.onChange = onChange;
    runtimes_[root].binds.push_back(std::move(b));
    return true;
}

usize UiTree::bindingCount(UiWidgetId root) const {
    const UiLayoutRuntime* rt = runtimeOf(root);
    return rt ? rt->binds.size() : 0;
}

void UiTree::refreshBindings() {
    if (!bindResolver_) return;
    for (auto& kv : runtimes_) {
        for (UiBinding& b : kv.second.binds) {
            if (b.onChange && !b.dirty) continue;
            b.dirty = false;
            UiWidget* w = get(b.widget);
            if (!w) continue;
            UiBindValue v;
            if (!bindResolver_(b.source, b.path, v) || v.kind == UiBindValue::Kind::None) continue;
            const std::string text = uiFormatBound(v, b.format);
            if (b.hasLast && text == b.last) continue;
            b.hasLast = true;
            b.last = text;
            if (b.property == "text") {
                w->text = text;
                w->caret = static_cast<i32>(uiUtf8Length(w->text));
            } else {
                uiSetProp(*w, b.property, uiQuote(text));
            }
        }
    }
}

// ---- event routes ----------------------------------------------------------------------------------------

bool UiTree::addRoute(UiEventRoute route) {
    const UiWidgetId root = rootOf(route.widget);
    if (!root) return false;
    runtimes_[root].routes.push_back(std::move(route));
    return true;
}

bool UiTree::pollRouted(UiRoutedEvent& out) {
    if (routed_.empty()) return false;
    out = std::move(routed_.front());
    routed_.pop_front();
    return true;
}

void UiTree::routeEvent(const UiEvent& ev) {
    if (runtimes_.empty()) return;
    const auto it = runtimes_.find(runtimeRoot(ev.widget));
    if (it == runtimes_.end()) return;
    std::vector<UiEventRoute> hits;   // copied: a command route emits, which may attach or drop runtimes
    for (const UiEventRoute& r : it->second.routes) {
        if (r.type != static_cast<u8>(ev.type) || r.widget != ev.widget) continue;
        if (ev.type == UiEventType::Hovered && r.hoverEnter != (ev.value != 0.0f)) continue;
        hits.push_back(r);
    }
    for (const UiEventRoute& r : hits) {
        if (r.action == UiRouteAction::Command) {
            UiEvent c;
            c.type = UiEventType::Command;
            c.widget = ev.widget;
            c.text = r.target;
            c.value = ev.value;
            c.index = ev.index;
            emit(std::move(c));
        } else {
            UiRoutedEvent q;
            q.action = r.action;
            q.target = r.target;
            q.args = r.args;
            q.widget = ev.widget;
            q.eventType = static_cast<u8>(ev.type);
            q.value = ev.value;
            q.index = ev.index;
            q.text = ev.text;
            routed_.push_back(std::move(q));
            while (routed_.size() > 256) routed_.pop_front();
        }
    }
}

void UiTree::detectHover() {
    if (runtimes_.empty()) return;
    std::vector<UiWidgetId> seen;
    for (auto& kv : runtimes_) {
        for (const UiEventRoute& r : kv.second.routes) {
            if (r.type != static_cast<u8>(UiEventType::Hovered)) continue;
            if (std::find(seen.begin(), seen.end(), r.widget) != seen.end()) continue;
            seen.push_back(r.widget);
            const UiWidget* w = get(r.widget);
            const bool now = w && w->hovered;
            bool& was = hoverSeen_[r.widget];
            if (now == was) continue;
            was = now;
            UiEvent e;
            e.type = UiEventType::Hovered;
            e.widget = r.widget;
            e.value = now ? 1.0f : 0.0f;
            emit(std::move(e));
        }
    }
}

} // namespace aver::ui
