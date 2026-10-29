// The sequencer's track and section editing (SequenceTrackOps.hpp).
#include "SequenceTrackOps.hpp"

#if AVER_MODULE_SCENE

#include "aver/formats/OcSeq.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace aver::editor {

using fmt::OcSeqKey;
using fmt::OcSeqTrack;
using fmt::OcSeqTrackKind;

namespace {

constexpr f64 kEps = 1e-9;

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

usize insertSorted(std::vector<OcSeqKey>& keys, OcSeqKey k) {
    const auto it = std::upper_bound(keys.begin(), keys.end(), k.t, [](f64 t, const OcSeqKey& a) { return t < a.t; });
    // Two statements -- see SequenceCurveOps.cpp's insertSorted for the evaluation-order bug one hides.
    const auto at = keys.insert(it, std::move(k));
    return static_cast<usize>(at - keys.begin());
}

i32 targetOf(scene::Entity e) { return static_cast<i32>(static_cast<u32>(e)); }
scene::Entity entityOfTarget(i32 t) { return t > 0 ? static_cast<scene::Entity>(static_cast<u32>(t)) : scene::kInvalidEntity; }

std::string trackWhat(const OcSeqTrack& tr) {
    std::string s = seqKindLabel(tr.kind);
    if (!tr.property.empty()) s += " " + tr.property;
    else if (!tr.name.empty()) s += " " + tr.name;
    if (!tr.bind.empty()) s += " (" + tr.bind + ")";
    return s;
}

} // namespace

const char* seqKindLabel(OcSeqTrackKind kind) {
    switch (kind) {
        case OcSeqTrackKind::Transform:   return "Transform";
        case OcSeqTrackKind::Camera:      return "Camera";
        case OcSeqTrackKind::Material:    return "Emissive";
        case OcSeqTrackKind::Property:    return "Property";
        case OcSeqTrackKind::Event:       return "Event";
        case OcSeqTrackKind::Audio:       return "Audio";
        case OcSeqTrackKind::Animation:   return "Animation";
        case OcSeqTrackKind::Visibility:  return "Visibility";
        case OcSeqTrackKind::CameraCut:   return "Camera Cuts";
        case OcSeqTrackKind::SubSequence: return "Sub-sequence";
    }
    return "Track";
}

char seqKindLetter(OcSeqTrackKind kind) {
    switch (kind) {
        case OcSeqTrackKind::Transform:   return 'T';
        case OcSeqTrackKind::Camera:      return 'C';
        case OcSeqTrackKind::Material:    return 'E';
        case OcSeqTrackKind::Property:    return 'P';
        case OcSeqTrackKind::Event:       return 'V';
        case OcSeqTrackKind::Audio:       return 'A';
        case OcSeqTrackKind::Animation:   return 'N';
        case OcSeqTrackKind::Visibility:  return 'H';
        case OcSeqTrackKind::CameraCut:   return 'X';
        case OcSeqTrackKind::SubSequence: return 'S';
    }
    return '?';
}

bool seqKindIsSection(OcSeqTrackKind kind) {
    return kind == OcSeqTrackKind::Audio || kind == OcSeqTrackKind::Animation || kind == OcSeqTrackKind::SubSequence;
}

std::string seqUniqueCameraName(const fmt::OcSequence& seq) {
    const auto taken = [&](const std::string& n) {
        for (const OcSeqTrack& tr : seq.tracks)
            if (tr.kind == OcSeqTrackKind::Camera && (tr.name.empty() ? std::string("Camera") : tr.name) == n) return true;
        return false;
    };
    if (!taken("Camera")) return "Camera";
    for (int i = 2;; ++i) {
        const std::string n = "Camera " + std::to_string(i);
        if (!taken(n)) return n;
    }
}

OcSeqKey seqNeutralKey(OcSeqTrackKind kind, f64 t) {
    OcSeqKey k;
    k.t = t;
    switch (kind) {
        case OcSeqTrackKind::Visibility:  k.v[0] = 1.0; k.v[1] = 1.0; k.interp = fmt::OcSeqInterp::Step; break;
        case OcSeqTrackKind::Material:    k.v[0] = k.v[1] = k.v[2] = k.v[3] = 1.0; break;
        case OcSeqTrackKind::Event:       k.interp = fmt::OcSeqInterp::Step; break;
        case OcSeqTrackKind::CameraCut:   k.interp = fmt::OcSeqInterp::Step; break;
        case OcSeqTrackKind::Audio:
        case OcSeqTrackKind::Animation:
        case OcSeqTrackKind::SubSequence: k.interp = fmt::OcSeqInterp::Step; k.dur = 1.0; break;
        default: break;
    }
    return k;
}

OcSeqTrack seqNewTrack(OcSeqTrackKind kind) {
    OcSeqTrack tr;
    tr.kind = kind;
    return tr;
}

int seqCutTrack(const fmt::OcSequence& seq) {
    for (usize i = 0; i < seq.tracks.size(); ++i)
        if (seq.tracks[i].kind == OcSeqTrackKind::CameraCut) return static_cast<int>(i);
    return -1;
}

int seqAddTrack(fmt::OcSequence& seq, OcSeqTrack track) {
    if (track.kind == OcSeqTrackKind::CameraCut) {
        const int existing = seqCutTrack(seq);
        if (existing >= 0) return existing;
    }
    if (track.kind == OcSeqTrackKind::Camera) {
        bool clash = track.name.empty();
        for (const OcSeqTrack& tr : seq.tracks)
            if (tr.kind == OcSeqTrackKind::Camera && !track.name.empty() && tr.name == track.name) clash = true;
        if (clash) track.name = seqUniqueCameraName(seq);
    }
    seq.tracks.push_back(std::move(track));
    return static_cast<int>(seq.tracks.size()) - 1;
}

bool seqRenameCamera(fmt::OcSequence& seq, int trackIndex, const std::string& newName) {
    if (trackIndex < 0 || static_cast<usize>(trackIndex) >= seq.tracks.size() || newName.empty()) return false;
    OcSeqTrack& cam = seq.tracks[static_cast<usize>(trackIndex)];
    if (cam.kind != OcSeqTrackKind::Camera) return false;
    for (usize i = 0; i < seq.tracks.size(); ++i)
        if (static_cast<int>(i) != trackIndex && seq.tracks[i].kind == OcSeqTrackKind::Camera && seq.tracks[i].name == newName)
            return false;
    const std::string old = cam.name.empty() ? std::string("Camera") : cam.name;
    for (OcSeqTrack& tr : seq.tracks)
        if (tr.kind == OcSeqTrackKind::CameraCut)
            for (OcSeqKey& k : tr.keys)
                if (k.text == old) k.text = newName;
    cam.name = newName;
    return true;
}

std::unordered_set<scene::Entity> seqDrivenEntities(const fmt::OcSequence& editing) {
    std::unordered_set<scene::Entity> out;
    for (const OcSeqTrack& tr : editing.tracks)
        if (!tr.mute && tr.target > 0) out.insert(entityOfTarget(tr.target));
    return out;
}

// ---- property fields ----------------------------------------------------------------------------

u32 seqFieldArity(scene::FieldKind kind) {
    switch (kind) {
        case scene::FieldKind::Vec3: return 3;
        case scene::FieldKind::Quat: return 4;
        default:                     return 1;
    }
}

bool seqFieldSequenceable(const scene::FieldDesc& f) {
    if (f.readOnly) return false;
    switch (f.kind) {
        case scene::FieldKind::F32: case scene::FieldKind::Vec3: case scene::FieldKind::Quat:
        case scene::FieldKind::I32: case scene::FieldKind::Bool: return true;
        default: return false;
    }
}

std::vector<SeqFieldInfo> seqSequenceableFields(scene::World& w, scene::Entity e, std::string_view filter) {
    std::vector<SeqFieldInfo> out;
    const std::string needle = lower(filter);
    const bool onEntity = e != scene::kInvalidEntity && w.valid(e);
    for (u32 ci = 0; ci < w.componentCount(); ++ci) {
        const u32 type = w.componentAt(ci);
        if (onEntity && !w.hasComponent(e, type)) continue;
        for (u32 fi = 0; fi < w.fieldCount(type); ++fi) {
            const scene::FieldDesc* d = w.field(w.fieldAt(type, fi));
            if (!d || !seqFieldSequenceable(*d)) continue;
            if (!needle.empty() && lower(d->qualified).find(needle) == std::string::npos) continue;
            out.push_back({d->qualified, d->kind, seqFieldArity(d->kind)});
        }
    }
    return out;
}

bool seqKeyFromField(scene::World& w, scene::Entity e, const std::string& qualified, OcSeqKey& key) {
    const scene::FieldDesc* d = w.field(w.fieldId(qualified));
    if (!d || !seqFieldSequenceable(*d) || !w.valid(e)) return false;
    const void* comp = w.getComponent(e, d->component);
    if (!comp) return false;
    const u8* src = static_cast<const u8*>(comp) + d->offset;
    switch (d->kind) {
        case scene::FieldKind::F32:  { f32 x; std::memcpy(&x, src, sizeof x); key.v[0] = x; break; }
        case scene::FieldKind::Vec3: { f32 x[3]; std::memcpy(x, src, sizeof x); for (int i = 0; i < 3; ++i) key.v[i] = x[i]; break; }
        case scene::FieldKind::Quat: { f32 x[4]; std::memcpy(x, src, sizeof x); for (int i = 0; i < 4; ++i) key.v[i] = x[i]; break; }
        case scene::FieldKind::I32:  { i32 x; std::memcpy(&x, src, sizeof x); key.v[0] = x; break; }
        case scene::FieldKind::Bool: { i32 x; std::memcpy(&x, src, sizeof x); key.v[0] = x != 0 ? 1.0 : 0.0; break; }
        default: return false;
    }
    if (d->kind == scene::FieldKind::I32 || d->kind == scene::FieldKind::Bool) key.interp = fmt::OcSeqInterp::Step;
    return true;
}

// ---- sections -----------------------------------------------------------------------------------

int seqAddSection(OcSeqTrack& tr, f64 start, f64 dur, const std::string& text, f64 length) {
    start = std::max(start, 0.0);
    if (length > 0.0) start = std::min(start, std::max(0.0, length - kSeqMinSection));
    dur = std::max(dur, kSeqMinSection);
    // A section already starting here is replaced.
    tr.keys.erase(std::remove_if(tr.keys.begin(), tr.keys.end(),
                                 [&](const OcSeqKey& k) { return std::fabs(k.t - start) < 1e-6; }),
                  tr.keys.end());
    // The one before ends where this begins.
    for (usize i = tr.keys.size(); i-- > 0;) {
        if (tr.keys[i].t >= start) continue;
        if (tr.keys[i].dur > 0.0 && tr.keys[i].t + tr.keys[i].dur > start) tr.keys[i].dur = start - tr.keys[i].t;
        break;
    }
    // This one stops where the next begins.
    for (const OcSeqKey& k : tr.keys)
        if (k.t > start) { dur = std::min(dur, k.t - start); break; }
    if (length > 0.0) dur = std::min(dur, length - start);
    if (dur < kSeqMinSection - kEps) return -1;
    OcSeqKey k = seqNeutralKey(tr.kind, start);
    k.dur = dur;
    k.text = text;
    return static_cast<int>(insertSorted(tr.keys, std::move(k)));
}

bool seqMoveSection(OcSeqTrack& tr, int& index, f64 newStart, f64 length) {
    if (index < 0 || static_cast<usize>(index) >= tr.keys.size()) return false;
    const OcSeqKey& cur = tr.keys[static_cast<usize>(index)];
    const f64 dur = cur.dur;
    newStart = std::max(newStart, 0.0);
    if (length > 0.0) newStart = std::min(newStart, std::max(0.0, length - dur));
    for (usize j = 0; j < tr.keys.size(); ++j) {
        if (static_cast<int>(j) == index || tr.keys[j].dur <= 0.0) continue;
        const f64 a = tr.keys[j].t, b = tr.keys[j].t + tr.keys[j].dur;
        if (newStart < b - kEps && a < newStart + dur - kEps) return false;
    }
    OcSeqKey k = std::move(tr.keys[static_cast<usize>(index)]);
    tr.keys.erase(tr.keys.begin() + index);
    k.t = newStart;
    index = static_cast<int>(insertSorted(tr.keys, std::move(k)));
    return true;
}

bool seqTrimSectionStart(OcSeqTrack& tr, int index, f64 newStart) {
    if (index < 0 || static_cast<usize>(index) >= tr.keys.size()) return false;
    OcSeqKey& k = tr.keys[static_cast<usize>(index)];
    const f64 end = k.t + k.dur;
    f64 floorT = 0.0;
    if (index > 0) {
        const OcSeqKey& p = tr.keys[static_cast<usize>(index) - 1];
        floorT = p.dur > 0.0 ? p.t + p.dur : p.t;
    }
    if (newStart < floorT - kEps || newStart > end - kSeqMinSection + kEps) return false;
    const f64 delta = newStart - k.t;
    if (tr.kind == OcSeqTrackKind::Animation) k.v[0] += delta * k.v[6];   // the clip offset follows the cut
    k.t = newStart;
    k.dur = end - newStart;
    return true;
}

bool seqTrimSectionEnd(OcSeqTrack& tr, int index, f64 newEnd, f64 length) {
    if (index < 0 || static_cast<usize>(index) >= tr.keys.size()) return false;
    OcSeqKey& k = tr.keys[static_cast<usize>(index)];
    if (newEnd < k.t + kSeqMinSection - kEps) return false;
    if (static_cast<usize>(index) + 1 < tr.keys.size() && newEnd > tr.keys[static_cast<usize>(index) + 1].t + kEps) return false;
    if (length > 0.0 && newEnd > length + kEps) return false;
    k.dur = newEnd - k.t;
    return true;
}

bool seqSectionsOverlap(const OcSeqTrack& tr) {
    for (usize i = 0; i + 1 < tr.keys.size(); ++i)
        if (tr.keys[i].dur > 0.0 && tr.keys[i].t + tr.keys[i].dur > tr.keys[i + 1].t + kEps) return true;
    return false;
}

// ---- assets -------------------------------------------------------------------------------------

SeqAssetKind seqClassifyAsset(const std::string& path) {
    const usize dot = path.rfind('.');
    if (dot == std::string::npos) return SeqAssetKind::None;
    const std::string ext = lower(std::string_view(path).substr(dot));
    if (ext == ".wav" || ext == ".mp3" || ext == ".m4a" || ext == ".wma" || ext == ".flac" || ext == ".ogg" || ext == ".oga")
        return SeqAssetKind::Sound;
    if (ext == ".ocanim") return SeqAssetKind::Clip;
    if (ext == ".ocmontage") return SeqAssetKind::Montage;
    if (ext == ".ocseq") return SeqAssetKind::Sequence;
    return SeqAssetKind::None;
}

bool seqLaneAccepts(OcSeqTrackKind kind, SeqAssetKind a) {
    switch (kind) {
        case OcSeqTrackKind::Audio:       return a == SeqAssetKind::Sound;
        case OcSeqTrackKind::Animation:   return a == SeqAssetKind::Clip || a == SeqAssetKind::Montage;
        case OcSeqTrackKind::SubSequence: return a == SeqAssetKind::Sequence;
        default:                          return false;
    }
}

OcSeqTrackKind seqTrackKindForAsset(SeqAssetKind a) {
    switch (a) {
        case SeqAssetKind::Sound:    return OcSeqTrackKind::Audio;
        case SeqAssetKind::Clip:
        case SeqAssetKind::Montage:  return OcSeqTrackKind::Animation;
        case SeqAssetKind::Sequence: return OcSeqTrackKind::SubSequence;
        default:                     return OcSeqTrackKind::Event;
    }
}

// ---- editing form <-> file form -----------------------------------------------------------------

SeqBindState seqBindState(const OcSeqTrack& tr, bool entityAlive) {
    const bool wants = fmt::seqTrackNeedsBinding(tr.kind) || tr.target > 0 || !tr.bind.empty();
    if (!wants) return SeqBindState::Free;
    if (tr.target > 0 && entityAlive) return SeqBindState::Placement;
    if (!tr.bind.empty()) return SeqBindState::Named;
    return SeqBindState::Unbound;
}

fmt::OcSequence seqFromFile(const fmt::OcSequence& file, const std::vector<scene::Entity>& placementEntities,
                            const std::function<scene::Entity(const std::string&)>& findByName,
                            std::vector<std::string>* dropped) {
    scene::World& w = scene::World::instance();
    fmt::OcSequence out = file;
    out.tracks.clear();
    for (const OcSeqTrack& tr : file.tracks) {
        OcSeqTrack c = tr;
        std::stable_sort(c.keys.begin(), c.keys.end(), [](const OcSeqKey& a, const OcSeqKey& b) { return a.t < b.t; });
        const bool wants = fmt::seqTrackNeedsBinding(tr.kind) || tr.target >= 0 || !tr.bind.empty();
        if (!wants) { c.target = -1; out.tracks.push_back(std::move(c)); continue; }
        scene::Entity e = scene::kInvalidEntity;
        if (!tr.bind.empty() && findByName) e = findByName(tr.bind);
        if ((e == scene::kInvalidEntity || !w.valid(e)) && tr.target >= 0 &&
            static_cast<usize>(tr.target) < placementEntities.size())
            e = placementEntities[static_cast<usize>(tr.target)];
        if (e != scene::kInvalidEntity && w.valid(e)) {
            c.target = targetOf(e);
        } else if (!tr.bind.empty()) {
            c.target = -1;   // kept, unresolved: it shows red and a later rename can fix it
        } else {
            if (dropped) dropped->push_back(trackWhat(tr) + ": its placement is gone");
            continue;
        }
        out.tracks.push_back(std::move(c));
    }
    return out;
}

fmt::OcSequence seqToFile(const fmt::OcSequence& editing, const std::unordered_map<u32, i32>& slotOf,
                          const std::function<std::string(scene::Entity)>& bindName, std::vector<std::string>* dropped) {
    fmt::OcSequence out = editing;
    out.tracks.clear();
    for (const OcSeqTrack& tr : editing.tracks) {
        OcSeqTrack c = tr;
        if (tr.target > 0) {
            const auto it = slotOf.find(static_cast<u32>(tr.target));
            if (it != slotOf.end()) {
                c.target = it->second;
                c.bind.clear();
            } else {
                const std::string name = bindName ? bindName(entityOfTarget(tr.target)) : std::string();
                if (name.empty()) {
                    if (dropped) dropped->push_back(trackWhat(tr) + ": its actor is gone or has no name");
                    continue;
                }
                c.target = -1;
                c.bind = name;
            }
        } else {
            c.target = -1;
        }
        out.tracks.push_back(std::move(c));
    }
    return out;
}

bool seqToOcseq(const fmt::OcSequence& editing, const std::function<std::string(scene::Entity)>& bindName,
                const std::function<int(const std::string&)>& nameCount, fmt::OcSequence& out,
                std::vector<std::string>* problems) {
    bool ok = true;
    fmt::OcSequence r = editing;
    r.tracks.clear();
    for (const OcSeqTrack& tr : editing.tracks) {
        OcSeqTrack c = tr;
        if (tr.target > 0) {
            const std::string name = bindName ? bindName(entityOfTarget(tr.target)) : std::string();
            if (name.empty()) {
                ok = false;
                if (problems) problems->push_back(trackWhat(tr) + ": the actor has no name");
            } else if (nameCount && nameCount(name) > 1) {
                ok = false;
                if (problems) problems->push_back(trackWhat(tr) + ": the name '" + name + "' is shared by several actors");
            }
            c.bind = name;
        } else if (fmt::seqTrackNeedsBinding(tr.kind) && tr.bind.empty()) {
            ok = false;
            if (problems) problems->push_back(trackWhat(tr) + ": no actor");
        }
        c.target = -1;
        r.tracks.push_back(std::move(c));
    }
    if (ok) out = std::move(r);
    return ok;
}

void seqSplitStreamed(fmt::OcSequence& seq, std::vector<OcSeqTrack>& aside) {
    std::vector<OcSeqTrack> keep;
    for (OcSeqTrack& tr : seq.tracks) (tr.target >= 0 ? aside : keep).push_back(std::move(tr));
    seq.tracks = std::move(keep);
}

} // namespace aver::editor

#endif
