// The bus graph: Godot's AudioServer bus model (list order, sends, solo chains, effects, peaks) over the mixer's blocks.
#include "aver/audio/BusGraph.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>

namespace aver::audio {
namespace {
const std::string kEmpty;
constexpr f32 kPeakOffset = 1.0e-10f;   // Godot's AUDIO_PEAK_OFFSET
} // namespace

BusGraph::BusGraph() = default;
BusGraph::~BusGraph() = default;

void BusGraph::init(u32 sampleRate) {
    shutdown();
    Guard g(lock_);
    rate_ = sampleRate;
    auto master = std::make_unique<Bus>();
    master->name = "Master";
    master->send.clear();
    buses_.push_back(std::move(master));
    nextEffectId_ = 1;
}

void BusGraph::shutdown() {
    Guard g(lock_);
    buses_.clear();
    active_.store(false, std::memory_order_release);
    rate_ = 0;
}

std::string BusGraph::uniqueName(const std::string& want, const Bus* except) const {
    auto taken = [&](const std::string& n) {
        for (const auto& b : buses_) if (b.get() != except && b->name == n) return true;
        return false;
    };
    if (!taken(want)) return want;
    // Godot: "New Bus", "New Bus 2", "New Bus 3" ...
    for (int attempt = 2;; ++attempt) {
        std::string n = want + " " + std::to_string(attempt);
        if (!taken(n)) return n;
    }
}

void BusGraph::resolveLinks() {
    const i32 n = busCount();
    auto find = [&](const std::string& name) {
        for (i32 i = 0; i < n; ++i) if (buses_[static_cast<usize>(i)]->name == name) return i;
        return static_cast<i32>(-1);
    };
    for (i32 i = 0; i < n; ++i) {
        Bus& b = *buses_[static_cast<usize>(i)];
        const i32 s = i == 0 ? -1 : find(b.send);
        b.sendIdx = (s >= 0 && s < i) ? s : 0;                      // Godot: an invalid send goes to Master
        for (auto& slot : b.effects) slot->fx->sidechainBus.store(slot->sidechain.empty() ? -1 : find(slot->sidechain), std::memory_order_relaxed);
    }
}

i32 BusGraph::addBus(i32 at) {
    if (rate_ == 0 || busCount() >= static_cast<i32>(kMaxBuses)) return -1;
    auto bus = std::make_unique<Bus>();
    Guard g(lock_);
    bus->name = uniqueName("New Bus", nullptr);
    i32 pos = at;
    if (pos < 0 || pos >= busCount()) pos = busCount();
    else if (pos == 0) pos = busCount() > 1 ? 1 : busCount();
    buses_.insert(buses_.begin() + pos, std::move(bus));
    resolveLinks();
    return pos;
}

bool BusGraph::removeBus(i32 idx) {
    if (idx <= 0 || !valid(idx)) return false;
    std::unique_ptr<Bus> dead;
    {
        Guard g(lock_);
        dead = std::move(buses_[static_cast<usize>(idx)]);
        buses_.erase(buses_.begin() + idx);
        resolveLinks();
    }
    return true;     // `dead` (and its effects) are freed here, outside the lock
}

bool BusGraph::moveBus(i32 from, i32 to) {
    const i32 n = busCount();
    if (from <= 0 || from >= n) return false;
    if (to < 0) to = n;
    if (to < 1 || to > n) return false;
    Guard g(lock_);
    auto bus = std::move(buses_[static_cast<usize>(from)]);
    buses_.erase(buses_.begin() + from);
    if (to > from) --to;
    buses_.insert(buses_.begin() + to, std::move(bus));
    resolveLinks();
    return true;
}

void BusGraph::setBusCount(i32 count) {
    if (count < 1) count = 1;
    if (count > static_cast<i32>(kMaxBuses)) count = static_cast<i32>(kMaxBuses);
    while (busCount() < count) addBus(-1);
    while (busCount() > count) removeBus(busCount() - 1);
}

i32 BusGraph::findBus(const std::string& name) const {
    for (i32 i = 0; i < busCount(); ++i) if (buses_[static_cast<usize>(i)]->name == name) return i;
    return -1;
}

const std::string& BusGraph::busName(i32 idx) const { return valid(idx) ? buses_[static_cast<usize>(idx)]->name : kEmpty; }

void BusGraph::setBusName(i32 idx, const std::string& name) {
    if (idx <= 0 || !valid(idx) || name.empty()) return;
    Guard g(lock_);
    Bus& b = *buses_[static_cast<usize>(idx)];
    b.name = uniqueName(name, &b);
    // Godot keeps sends by name: a rename leaves the buses that sent here pointing at the old name.
    resolveLinks();
}

void BusGraph::setSend(i32 idx, const std::string& sendName) {
    if (idx <= 0 || !valid(idx)) return;
    Guard g(lock_);
    buses_[static_cast<usize>(idx)]->send = sendName;
    resolveLinks();
}

const std::string& BusGraph::sendName(i32 idx) const { return valid(idx) ? buses_[static_cast<usize>(idx)]->send : kEmpty; }
i32 BusGraph::sendIndex(i32 idx) const { return idx > 0 && valid(idx) ? buses_[static_cast<usize>(idx)]->sendIdx : -1; }

void BusGraph::setVolumeDb(i32 idx, f32 db) { if (valid(idx)) buses_[static_cast<usize>(idx)]->volumeDb.store(db, std::memory_order_relaxed); }
f32  BusGraph::volumeDb(i32 idx) const { return valid(idx) ? buses_[static_cast<usize>(idx)]->volumeDb.load(std::memory_order_relaxed) : 0.0f; }
void BusGraph::setMute(i32 idx, bool m) { if (valid(idx)) buses_[static_cast<usize>(idx)]->mute.store(m, std::memory_order_relaxed); }
bool BusGraph::mute(i32 idx) const { return valid(idx) && buses_[static_cast<usize>(idx)]->mute.load(std::memory_order_relaxed); }
void BusGraph::setSolo(i32 idx, bool s) { if (valid(idx)) buses_[static_cast<usize>(idx)]->solo.store(s, std::memory_order_relaxed); }
bool BusGraph::solo(i32 idx) const { return valid(idx) && buses_[static_cast<usize>(idx)]->solo.load(std::memory_order_relaxed); }
void BusGraph::setBypassEffects(i32 idx, bool b) { if (valid(idx)) buses_[static_cast<usize>(idx)]->bypass.store(b, std::memory_order_relaxed); }
bool BusGraph::bypassEffects(i32 idx) const { return valid(idx) && buses_[static_cast<usize>(idx)]->bypass.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------- effects

u32 BusGraph::addEffect(i32 bus, EffectKind kind, i32 at) {
    if (!valid(bus) || rate_ == 0) return 0;
    Bus& b = *buses_[static_cast<usize>(bus)];
    if (b.effects.size() >= kMaxEffects) return 0;
    auto slot = std::make_unique<Slot>();
    slot->fx = Effect::create(kind, rate_);
    if (!slot->fx) return 0;
    Guard g(lock_);
    slot->id = nextEffectId_++;
    const u32 id = slot->id;
    i32 pos = at;
    if (pos < 0 || pos > static_cast<i32>(b.effects.size())) pos = static_cast<i32>(b.effects.size());
    b.effects.insert(b.effects.begin() + pos, std::move(slot));
    return id;
}

bool BusGraph::removeEffect(i32 bus, i32 effectIdx) {
    if (!valid(bus)) return false;
    Bus& b = *buses_[static_cast<usize>(bus)];
    if (effectIdx < 0 || effectIdx >= static_cast<i32>(b.effects.size())) return false;
    std::unique_ptr<Slot> dead;
    {
        Guard g(lock_);
        dead = std::move(b.effects[static_cast<usize>(effectIdx)]);
        b.effects.erase(b.effects.begin() + effectIdx);
    }
    return true;
}

bool BusGraph::moveEffect(i32 bus, i32 from, i32 to) {
    if (!valid(bus)) return false;
    Bus& b = *buses_[static_cast<usize>(bus)];
    const i32 n = static_cast<i32>(b.effects.size());
    if (from < 0 || from >= n || to < 0 || to >= n) return false;
    Guard g(lock_);
    auto slot = std::move(b.effects[static_cast<usize>(from)]);
    b.effects.erase(b.effects.begin() + from);
    b.effects.insert(b.effects.begin() + to, std::move(slot));
    return true;
}

i32 BusGraph::effectCount(i32 bus) const { return valid(bus) ? static_cast<i32>(buses_[static_cast<usize>(bus)]->effects.size()) : 0; }

u32 BusGraph::effectId(i32 bus, i32 effectIdx) const {
    if (!valid(bus)) return 0;
    const auto& e = buses_[static_cast<usize>(bus)]->effects;
    return effectIdx >= 0 && effectIdx < static_cast<i32>(e.size()) ? e[static_cast<usize>(effectIdx)]->id : 0;
}

Effect* BusGraph::effectAt(i32 bus, i32 effectIdx) const {
    if (!valid(bus)) return nullptr;
    const auto& e = buses_[static_cast<usize>(bus)]->effects;
    return effectIdx >= 0 && effectIdx < static_cast<i32>(e.size()) ? e[static_cast<usize>(effectIdx)]->fx.get() : nullptr;
}

bool BusGraph::findEffect(u32 id, i32& bus, i32& effectIdx) const {
    if (id == 0) return false;
    for (i32 i = 0; i < busCount(); ++i) {
        const auto& e = buses_[static_cast<usize>(i)]->effects;
        for (usize k = 0; k < e.size(); ++k)
            if (e[k]->id == id) { bus = i; effectIdx = static_cast<i32>(k); return true; }
    }
    return false;
}

Effect* BusGraph::effect(u32 id) const {
    i32 b, e;
    return findEffect(id, b, e) ? effectAt(b, e) : nullptr;
}

void BusGraph::setEffectEnabled(i32 bus, i32 effectIdx, bool e) {
    if (!valid(bus)) return;
    auto& v = buses_[static_cast<usize>(bus)]->effects;
    if (effectIdx >= 0 && effectIdx < static_cast<i32>(v.size())) v[static_cast<usize>(effectIdx)]->enabled.store(e, std::memory_order_relaxed);
}

bool BusGraph::effectEnabled(i32 bus, i32 effectIdx) const {
    if (!valid(bus)) return false;
    const auto& v = buses_[static_cast<usize>(bus)]->effects;
    return effectIdx >= 0 && effectIdx < static_cast<i32>(v.size()) && v[static_cast<usize>(effectIdx)]->enabled.load(std::memory_order_relaxed);
}

void BusGraph::setSidechain(i32 bus, i32 effectIdx, const std::string& busName) {
    if (!valid(bus)) return;
    auto& v = buses_[static_cast<usize>(bus)]->effects;
    if (effectIdx < 0 || effectIdx >= static_cast<i32>(v.size())) return;
    Guard g(lock_);
    v[static_cast<usize>(effectIdx)]->sidechain = busName;
    resolveLinks();
}

const std::string& BusGraph::sidechain(i32 bus, i32 effectIdx) const {
    if (!valid(bus)) return kEmpty;
    const auto& v = buses_[static_cast<usize>(bus)]->effects;
    return effectIdx >= 0 && effectIdx < static_cast<i32>(v.size()) ? v[static_cast<usize>(effectIdx)]->sidechain : kEmpty;
}

f32 BusGraph::peakDb(i32 idx, i32 channel) const {
    if (!valid(idx) || channel < 0 || channel > 1) return -200.0f;
    const f32 p = buses_[static_cast<usize>(idx)]->peak[channel].load(std::memory_order_relaxed);
    return linToDb(p + kPeakOffset);
}

// ---------------------------------------------------------------- layouts

u32 BusGraph::applyLayout(const BusLayout& layout) {
    if (rate_ == 0) return 0;
    u32 skipped = 0;
    std::vector<std::unique_ptr<Bus>> fresh;
    u32 nextId = 0;
    {
        Guard g(lock_);
        nextId = nextEffectId_;
    }
    auto build = [&](const BusLayoutBus& lb, bool master) {
        auto b = std::make_unique<Bus>();
        b->name = master ? "Master" : lb.name;
        for (const auto& o : fresh) if (!master && o->name == b->name) { b->name += " " + std::to_string(fresh.size() + 1); break; }
        b->send = master ? std::string() : lb.send;
        b->volumeDb.store(lb.volumeDb);
        b->mute.store(lb.mute);
        b->solo.store(lb.solo);
        b->bypass.store(lb.bypass);
        for (const BusLayoutEffect& le : lb.effects) {
            const EffectKind k = Effect::kindFromClassName(le.className.c_str());
            auto slot = std::make_unique<Slot>();
            slot->fx = k == EffectKind::Count ? nullptr : Effect::create(k, rate_);
            if (!slot->fx) { ++skipped; continue; }
            for (const auto& p : le.params) slot->fx->setParamByName(p.first.c_str(), p.second);
            slot->enabled.store(le.enabled);
            slot->sidechain = le.sidechain;
            slot->id = nextId++;
            b->effects.push_back(std::move(slot));
        }
        fresh.push_back(std::move(b));
    };
    if (layout.buses.empty()) build(BusLayoutBus{}, true);
    for (usize i = 0; i < layout.buses.size() && fresh.size() < kMaxBuses; ++i) build(layout.buses[i], i == 0);
    Guard g(lock_);
    buses_.swap(fresh);
    nextEffectId_ = nextId;
    resolveLinks();
    return skipped;     // `fresh` now holds the old buses and is destroyed after the lock is released
}

BusLayout BusGraph::exportLayout() const {
    BusLayout out;
    for (const auto& bp : buses_) {
        BusLayoutBus lb;
        lb.name = bp->name;
        lb.send = bp->send;
        lb.volumeDb = bp->volumeDb.load();
        lb.mute = bp->mute.load();
        lb.solo = bp->solo.load();
        lb.bypass = bp->bypass.load();
        for (const auto& s : bp->effects) {
            BusLayoutEffect le;
            le.className = Effect::godotClassName(s->fx->kind());
            le.enabled = s->enabled.load();
            le.sidechain = s->sidechain;
            for (u32 i = 0; i < s->fx->paramCount(); ++i) le.params.emplace_back(s->fx->paramInfo(i).name, s->fx->param(i));
            lb.effects.push_back(std::move(le));
        }
        out.buses.push_back(std::move(lb));
    }
    return out;
}

// ---------------------------------------------------------------- the audio thread

bool BusGraph::begin() { return rate_ != 0 && active() && lock_.tryLock(); }
void BusGraph::end() { lock_.unlock(); }

f32* BusGraph::busBuffer(i32 idx) {
    if (idx < 0 || idx >= busCount()) idx = 0;
    return buses_[static_cast<usize>(idx)]->buf.data();
}

void BusGraph::clear(u32 frames) {
    for (auto& b : buses_) std::fill(b->buf.begin(), b->buf.begin() + static_cast<usize>(frames) * 2, 0.0f);
}

void BusGraph::process(u32 frames) {
    if (frames == 0 || frames > kBlock) return;
    const i32 n = busCount();
    bool soloMode = false;
    for (auto& b : buses_) {
        b->soloed = false;
        if (b->solo.load(std::memory_order_relaxed)) soloMode = true;
    }
    if (soloMode) {
        // Soloing a bus keeps every bus it sends through, up to Master, audible (Godot marks the chain).
        for (i32 i = 0; i < n; ++i) {
            if (!buses_[static_cast<usize>(i)]->solo.load(std::memory_order_relaxed)) continue;
            i32 k = i;
            for (i32 guard = 0; guard <= n; ++guard) {
                buses_[static_cast<usize>(k)]->soloed = true;
                if (k == 0) break;
                k = buses_[static_cast<usize>(k)]->sendIdx;
            }
        }
    }

    for (i32 i = n - 1; i >= 0; --i) {
        Bus& b = *buses_[static_cast<usize>(i)];
        f32* buf = b.buf.data();
        if (!b.bypass.load(std::memory_order_relaxed)) {
            for (auto& slot : b.effects) {
                if (!slot->enabled.load(std::memory_order_relaxed)) continue;
                const i32 sc = slot->fx->sidechainBus.load(std::memory_order_relaxed);
                const f32* scBuf = (sc > i && sc < n) ? buses_[static_cast<usize>(sc)]->buf.data() : nullptr;
                slot->fx->process(buf, frames, scBuf);
            }
        }
        f32 gain = dbToLin(b.volumeDb.load(std::memory_order_relaxed));
        if (soloMode) { if (!b.soloed) gain = 0.0f; }
        else if (b.mute.load(std::memory_order_relaxed)) gain = 0.0f;
        const f32 step = (gain - b.lastGain) / static_cast<f32>(frames);
        f32 g = b.lastGain, pl = 0.0f, pr = 0.0f;
        for (u32 f = 0; f < frames; ++f) {
            g += step;
            buf[2 * f] *= g;
            buf[2 * f + 1] *= g;
            pl = std::max(pl, std::fabs(buf[2 * f]));
            pr = std::max(pr, std::fabs(buf[2 * f + 1]));
        }
        b.lastGain = gain;
        b.peak[0].store(pl, std::memory_order_relaxed);
        b.peak[1].store(pr, std::memory_order_relaxed);
        if (i > 0) {
            f32* dst = buses_[static_cast<usize>(b.sendIdx)]->buf.data();
            for (u32 f = 0; f < frames * 2; ++f) dst[f] += buf[f];
        }
    }
}

} // namespace aver::audio
