// UiTree: structure, queries, events and the programmatic setters.
#include "aver/ui/UiTree.hpp"

#include <algorithm>

namespace aver::ui {

UiTree::UiTree() : theme_(uiBuiltinTheme("dark")) {}

// ---- structure -----------------------------------------------------------------------------------

UiWidgetId UiTree::create(UiWidgetKind kind, UiWidgetId parent, std::string_view name) {
    UiWidgetProps p = uiDefaultProps(kind);
    p.name = std::string(name);
    if (parent == 0) p.anchors = UiAnchors{0, 0, 1, 1};
    return createFrom(p, parent);
}

UiWidgetId UiTree::createFrom(const UiWidgetProps& props, UiWidgetId parent) {
    UiWidget* par = nullptr;
    if (parent != 0) {
        par = get(parent);
        if (!par) return 0;
    }
    auto w = std::make_unique<UiWidget>();
    static_cast<UiWidgetProps&>(*w) = props;
    w->id = nextId_++;
    w->seq = nextSeq_++;
    w->parent = parent;
    const UiWidgetId id = w->id;
    widgets_[id] = std::move(w);
    if (par) par->children.push_back(id);
    else roots_.push_back(id);
    return id;
}

UiWidget* UiTree::get(UiWidgetId id) {
    const auto it = widgets_.find(id);
    return it == widgets_.end() ? nullptr : it->second.get();
}

const UiWidget* UiTree::get(UiWidgetId id) const {
    const auto it = widgets_.find(id);
    return it == widgets_.end() ? nullptr : it->second.get();
}

namespace {

void collectSubtree(const UiTree& t, UiWidgetId id, std::vector<UiWidgetId>& out) {
    out.push_back(id);
    if (const UiWidget* w = t.get(id)) for (const UiWidgetId c : w->children) collectSubtree(t, c, out);
}

} // namespace

bool UiTree::destroy(UiWidgetId id) {
    UiWidget* w = get(id);
    if (!w) return false;

    std::vector<UiWidgetId> doomed;
    collectSubtree(*this, id, doomed);
    const auto inDoomed = [&](UiWidgetId x) { return std::find(doomed.begin(), doomed.end(), x) != doomed.end(); };
    if (inDoomed(focus_)) focus_ = 0;
    if (inDoomed(captured_)) captured_ = 0;
    if (inDoomed(pressed_)) pressed_ = 0;
    if (inDoomed(scope_)) scope_ = 0;

    if (w->parent != 0) {
        if (UiWidget* p = get(w->parent)) p->children.erase(std::remove(p->children.begin(), p->children.end(), id), p->children.end());
    } else {
        roots_.erase(std::remove(roots_.begin(), roots_.end(), id), roots_.end());
    }
    for (const UiWidgetId d : doomed) widgets_.erase(d);
    return true;
}

bool UiTree::reparent(UiWidgetId id, UiWidgetId newParent, i32 index) {
    UiWidget* w = get(id);
    if (!w) return false;
    if (newParent != 0) {
        if (!get(newParent)) return false;
        std::vector<UiWidgetId> sub;
        collectSubtree(*this, id, sub);
        if (std::find(sub.begin(), sub.end(), newParent) != sub.end()) return false;
    }

    if (w->parent != 0) {
        UiWidget* p = get(w->parent);
        p->children.erase(std::remove(p->children.begin(), p->children.end(), id), p->children.end());
    } else {
        roots_.erase(std::remove(roots_.begin(), roots_.end(), id), roots_.end());
    }
    w->parent = newParent;
    std::vector<UiWidgetId>& list = newParent != 0 ? get(newParent)->children : roots_;
    if (index < 0 || static_cast<usize>(index) >= list.size()) list.push_back(id);
    else list.insert(list.begin() + index, id);
    return true;
}

void UiTree::clear() {
    widgets_.clear();
    roots_.clear();
    focus_ = scope_ = captured_ = pressed_ = 0;
    pending_.clear();
    queue_.clear();
    state_ = {};
}

UiWidgetId UiTree::find(std::string_view name, UiWidgetId under) const {
    std::vector<UiWidgetId> stack;
    if (under != 0) stack.push_back(under);
    else for (usize i = roots_.size(); i-- > 0;) stack.push_back(roots_[i]);
    while (!stack.empty()) {
        const UiWidgetId id = stack.back();
        stack.pop_back();
        const UiWidget* w = get(id);
        if (!w) continue;
        if (w->name == name) return id;
        for (usize i = w->children.size(); i-- > 0;) stack.push_back(w->children[i]);
    }
    return 0;
}

UiWidgetId UiTree::rootOf(UiWidgetId id) const {
    const UiWidget* w = get(id);
    while (w && w->parent != 0) w = get(w->parent);
    return w ? w->id : 0;
}

void UiTree::visit(UiWidgetId under, const std::function<void(UiWidget&)>& fn) {
    std::vector<UiWidgetId> stack;
    if (under != 0) stack.push_back(under);
    else for (usize i = roots_.size(); i-- > 0;) stack.push_back(roots_[i]);
    while (!stack.empty()) {
        const UiWidgetId id = stack.back();
        stack.pop_back();
        UiWidget* w = get(id);
        if (!w) continue;
        fn(*w);
        for (usize i = w->children.size(); i-- > 0;) stack.push_back(w->children[i]);
    }
}

bool UiTree::isVisible(UiWidgetId id) const {
    for (const UiWidget* w = get(id); w; w = w->parent ? get(w->parent) : nullptr)
        if (!w->visible) return false;
    return get(id) != nullptr;
}

bool UiTree::isEnabled(UiWidgetId id) const {
    for (const UiWidget* w = get(id); w; w = w->parent ? get(w->parent) : nullptr)
        if (!w->enabled) return false;
    return get(id) != nullptr;
}

std::vector<UiWidgetId> UiTree::sortedRoots() const {
    std::vector<UiWidgetId> out = roots_;
    std::stable_sort(out.begin(), out.end(), [this](UiWidgetId a, UiWidgetId b) {
        const UiWidget* x = get(a);
        const UiWidget* y = get(b);
        if (x->layer != y->layer) return static_cast<int>(x->layer) < static_cast<int>(y->layer);
        if (x->zOrder != y->zOrder) return x->zOrder < y->zOrder;
        return x->seq < y->seq;
    });
    return out;
}

UiWidgetId UiTree::navScope() const {
    const std::vector<UiWidgetId> order = sortedRoots();
    for (usize i = order.size(); i-- > 0;) {
        const UiWidget* w = get(order[i]);
        if (w->visible && w->modal) return w->id;
    }
    for (usize i = order.size(); i-- > 0;) {
        const UiWidget* w = get(order[i]);
        if (w->visible && w->inputMode == UiInputMode::Menu) return w->id;
    }
    return 0;
}

// ---- events --------------------------------------------------------------------------------------

bool UiTree::pollEvent(UiEvent& out) {
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

usize UiTree::addListener(std::function<void(const UiEvent&)> fn) {
    const usize token = nextListener_++;
    listeners_.emplace_back(token, std::move(fn));
    return token;
}

void UiTree::removeListener(usize token) {
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(),
                                    [token](const auto& l) { return l.first == token; }),
                     listeners_.end());
}

void UiTree::emit(UiEvent ev) {
    if (ev.type == UiEventType::Command) runBuiltinCommand(ev.text, ev.widget);
    pending_.push_back(std::move(ev));
}

bool UiTree::runBuiltinCommand(std::string_view text, UiWidgetId source) {
    if (text.rfind("ui.", 0) != 0) return false;
    const usize colon = text.find(':');
    const std::string_view verb = text.substr(0, colon);
    const std::string_view arg = colon == std::string_view::npos ? std::string_view{} : text.substr(colon + 1);
    if (verb != "ui.close" && verb != "ui.open" && verb != "ui.toggle") return false;

    UiWidgetId target = arg.empty() ? rootOf(source) : find(arg);
    UiWidget* w = get(target);
    if (!w) return true;
    if (verb == "ui.close") w->visible = false;
    else if (verb == "ui.open") w->visible = true;
    else w->visible = !w->visible;
    return true;
}

void UiTree::dispatch() {
    for (int guard = 0; guard < 8 && !pending_.empty(); ++guard) {
        std::vector<UiEvent> batch;
        batch.swap(pending_);
        const auto listeners = listeners_;
        for (UiEvent& ev : batch) {
            for (const auto& l : listeners) l.second(ev);
            queue_.push_back(std::move(ev));
            while (queue_.size() > 1024) queue_.pop_front();
        }
    }
}

// ---- programmatic setters ----------------------------------------------------------------------------

void UiTree::setVisible(UiWidgetId id, bool visible) {
    if (UiWidget* w = get(id)) w->visible = visible;
}

void UiTree::setText(UiWidgetId id, std::string_view text) {
    UiWidget* w = get(id);
    if (!w) return;
    w->text = std::string(text);
    w->caret = static_cast<i32>(uiUtf8Length(w->text));
}

void UiTree::setValue(UiWidgetId id, f32 value) {
    UiWidget* w = get(id);
    if (!w) return;
    if (w->maxValue > w->minValue) value = std::clamp(value, w->minValue, w->maxValue);
    w->value = value;
}

void UiTree::setChecked(UiWidgetId id, bool checked) {
    if (UiWidget* w = get(id)) w->checked = checked;
}

void UiTree::setSelected(UiWidgetId id, i32 index) {
    UiWidget* w = get(id);
    if (!w) return;
    const i32 n = static_cast<i32>(w->items.size());
    w->selected = n == 0 ? 0 : std::clamp(index, 0, n - 1);
}

void UiTree::scrollIntoView(UiWidgetId id) {
    const UiWidget* target = get(id);
    if (!target) return;
    for (UiWidget* a = target->parent ? get(target->parent) : nullptr; a; a = a->parent ? get(a->parent) : nullptr) {
        if (a->kind != UiWidgetKind::Scroll && a->kind != UiWidgetKind::List) continue;
        const UiRect inner = a->rect.deflate(paddingOf(*a, styleOf(*a)));
        if (target->rect.y < inner.y) a->scrollY -= inner.y - target->rect.y;
        else if (target->rect.bottom() > inner.bottom()) a->scrollY += target->rect.bottom() - inner.bottom();
        if (target->rect.x < inner.x) a->scrollX -= inner.x - target->rect.x;
        else if (target->rect.right() > inner.right()) a->scrollX += target->rect.right() - inner.right();
        a->scrollY = std::max(0.0f, a->scrollY);
        a->scrollX = std::max(0.0f, a->scrollX);
    }
}

} // namespace aver::ui
