// Godot Animation and AnimationLibrary resources to .ocanim property tracks (docs/plans/fabric/G5_PARITY.md, A1).
//
// A Godot animation stores its tracks as `tracks/N/<field>` properties: a type, a path, interpolation flags and a `keys` blob whose layout
// depends on the type. Each is decoded into an fmt::OcPropTrack. Position, rotation and scale tracks go through the shared axis map
// (metres to centimetres, +Y up to +Z up); a value track keeps Godot's numbers because the runtime writes them to a Godot-named property.
#include "FabricInternal.hpp"

#include "aver/formats/AxisMap.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>

namespace aver::fabric {

namespace {

using fmt::OcPropKey;
using fmt::OcPropKind;
using fmt::OcPropTrack;
using fmt::OcPropTrackType;
using fmt::OcPropValue;

const Var* getProp(const std::vector<gd::GodotProperty>& props, const std::string& name) {
    for (const gd::GodotProperty& p : props)
        if (p.name == name) return &p.value;
    return nullptr;
}

OcPropValue valueOf(const Var& v) {
    OcPropValue r;
    const auto nums = [&](OcPropKind k, int n) {
        r.kind = k;
        for (int i = 0; i < n; ++i) r.n[i] = v.num(static_cast<usize>(i));
    };
    switch (v.type) {
        case VType::Nil: break;
        case VType::Bool: r.kind = OcPropKind::Bool; r.n[0] = v.b ? 1 : 0; break;
        case VType::Int: r.kind = OcPropKind::Int; r.n[0] = static_cast<double>(v.i); break;
        case VType::Float: r.kind = OcPropKind::Float; r.n[0] = v.f; break;
        case VType::Vector2: nums(OcPropKind::Vec2, 2); break;
        case VType::Vector3: nums(OcPropKind::Vec3, 3); break;
        case VType::Vector4: nums(OcPropKind::Vec4, 4); break;
        case VType::Quaternion: nums(OcPropKind::Quat, 4); break;
        case VType::Color: nums(OcPropKind::Color, 4); break;
        case VType::String:
        case VType::StringName: r.kind = OcPropKind::String; r.s = v.s; break;
        default: r.kind = OcPropKind::Text; r.s = gd::godotVariantToText(v); break;
    }
    return r;
}

double at(const std::vector<f64>& v, usize i, double def) { return i < v.size() ? v[i] : def; }

std::string trackTypeName(OcPropTrackType t) {
    switch (t) {
        case OcPropTrackType::Value: return "value";
        case OcPropTrackType::Position3D: return "position_3d";
        case OcPropTrackType::Rotation3D: return "rotation_3d";
        case OcPropTrackType::Scale3D: return "scale_3d";
        case OcPropTrackType::BlendShape: return "blend_shape";
        case OcPropTrackType::Method: return "method";
        case OcPropTrackType::Bezier: return "bezier";
        case OcPropTrackType::Audio: return "audio";
        case OcPropTrackType::Animation: return "animation";
    }
    return "?";
}

}  // namespace

std::string Converter::animationAsset(const std::string& cacheKey, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc,
                                      const std::string& nameHint, const Where& w) {
    if (const auto it = animationAssets_.find(cacheKey); it != animationAssets_.end()) return it->second;
    animationAssets_[cacheKey] = "";

    fmt::OcAnimation clip;
    clip.propClip = true;
    clip.duration = static_cast<float>(std::max(0.0, numOf(getProp(props, "length"), 1.0)));
    const int loop = static_cast<int>(numOf(getProp(props, "loop_mode"), 0));
    clip.propLoop = loop == 1 ? fmt::OcPropLoop::Linear : loop == 2 ? fmt::OcPropLoop::PingPong : fmt::OcPropLoop::None;
    clip.propStep = static_cast<float>(numOf(getProp(props, "step"), 1.0 / 30.0));

    // tracks/<n>/<field>
    std::map<int, std::map<std::string, const gd::GodotProperty*>> tracks;
    for (const gd::GodotProperty& p : props) {
        if (p.name.rfind("tracks/", 0) != 0) continue;
        const size_t slash = p.name.find('/', 7);
        if (slash == std::string::npos) continue;
        const int idx = std::atoi(p.name.substr(7, slash - 7).c_str());
        tracks[idx][p.name.substr(slash + 1)] = &p;
    }

    const auto note = [&](int idx, const std::string& what, Rating r, const std::string& reason, int line) {
        report_.add("resource", doc.file, w.node, "Animation", nameHint + ":tracks/" + std::to_string(idx) + " " + what, r, reason, line);
    };
    size_t converted = 0, approximated = 0, dropped = 0;

    for (const auto& [idx, fields] : tracks) {
        const auto field = [&](const char* name) -> const gd::GodotProperty* {
            const auto it = fields.find(name);
            return it == fields.end() ? nullptr : it->second;
        };
        const gd::GodotProperty* typeP = field("type");
        const std::string typeName = typeP ? typeP->value.asString() : "";
        const int line = typeP ? typeP->line : 0;
        OcPropTrack t;
        if (typeName == "value") t.type = OcPropTrackType::Value;
        else if (typeName == "position_3d") t.type = OcPropTrackType::Position3D;
        else if (typeName == "rotation_3d") t.type = OcPropTrackType::Rotation3D;
        else if (typeName == "scale_3d") t.type = OcPropTrackType::Scale3D;
        else if (typeName == "blend_shape") t.type = OcPropTrackType::BlendShape;
        else if (typeName == "method") t.type = OcPropTrackType::Method;
        else if (typeName == "bezier") t.type = OcPropTrackType::Bezier;
        else if (typeName == "audio") t.type = OcPropTrackType::Audio;
        else if (typeName == "animation") t.type = OcPropTrackType::Animation;
        else {
            note(idx, "type " + typeName, Rating::Unsupported, "an unknown track type", line);
            ++dropped;
            continue;
        }
        if (const auto* p = field("path")) t.path = p->value.asString();
        t.interp = static_cast<fmt::OcPropInterp>(std::clamp(static_cast<int>(numOf(field("interp") ? &field("interp")->value : nullptr, 1)), 0, 4));
        t.loopWrap = boolOf(field("loop_wrap") ? &field("loop_wrap")->value : nullptr, true);
        t.enabled = boolOf(field("enabled") ? &field("enabled")->value : nullptr, true);
        t.imported = boolOf(field("imported") ? &field("imported")->value : nullptr, false);
        const Var* keys = field("keys") ? &field("keys")->value : nullptr;
        const std::string where = typeName + " " + t.path;

        // The path's property part ("Node:prop") tells a bone track from a node track.
        const size_t colon = t.path.find(':');
        const bool boneTrack = colon != std::string::npos && colon + 1 < t.path.size() &&
                               (t.type == OcPropTrackType::Position3D || t.type == OcPropTrackType::Rotation3D || t.type == OcPropTrackType::Scale3D);
        Rating rating = Rating::Converted;
        std::string why = "decoded into an .ocanim property track";

        const Where kw{doc.file, w.node, "Animation"};
        const std::string propName = nameHint + ".tracks/" + std::to_string(idx);
        if (keys) {
            switch (t.type) {
                case OcPropTrackType::Position3D:
                case OcPropTrackType::Scale3D:
                case OcPropTrackType::Rotation3D:
                case OcPropTrackType::BlendShape: {
                    const usize stride = t.type == OcPropTrackType::Rotation3D ? 6 : t.type == OcPropTrackType::BlendShape ? 3 : 5;
                    const std::vector<f64>& n = keys->nums;
                    for (usize k = 0; k + stride <= n.size(); k += stride) {
                        OcPropKey key;
                        key.time = n[k];
                        key.transition = n[k + 1];
                        if (t.type == OcPropTrackType::Position3D) {
                            const Vec3 p = fmt::axis::position(static_cast<f32>(n[k + 2]), static_cast<f32>(n[k + 3]), static_cast<f32>(n[k + 4]));
                            key.value.kind = OcPropKind::Vec3; key.value.n[0] = p.x; key.value.n[1] = p.y; key.value.n[2] = p.z;
                        } else if (t.type == OcPropTrackType::Scale3D) {
                            const Vec3 s = fmt::axis::scale(static_cast<f32>(n[k + 2]), static_cast<f32>(n[k + 3]), static_cast<f32>(n[k + 4]));
                            key.value.kind = OcPropKind::Vec3; key.value.n[0] = s.x; key.value.n[1] = s.y; key.value.n[2] = s.z;
                        } else if (t.type == OcPropTrackType::Rotation3D) {
                            const Quat q = fmt::axis::rotation(static_cast<f32>(n[k + 2]), static_cast<f32>(n[k + 3]), static_cast<f32>(n[k + 4]), static_cast<f32>(n[k + 5]));
                            key.value.kind = OcPropKind::Quat; key.value.n[0] = q.x; key.value.n[1] = q.y; key.value.n[2] = q.z; key.value.n[3] = q.w;
                        } else {
                            key.value.kind = OcPropKind::Float;
                            key.value.n[0] = n[k + 2];
                        }
                        t.keys.push_back(std::move(key));
                    }
                    break;
                }
                case OcPropTrackType::Value: {
                    const Var* times = keys->find("times");
                    const Var* trans = keys->find("transitions");
                    const Var* values = keys->find("values");
                    if (const Var* up = keys->find("update"))
                        t.update = static_cast<fmt::OcPropUpdate>(std::clamp(static_cast<int>(up->asNumber()), 0, 2));
                    const usize n = times ? times->nums.size() : 0;
                    for (usize k = 0; k < n && values && k < values->items.size(); ++k) {
                        OcPropKey key;
                        key.time = times->nums[k];
                        key.transition = trans ? at(trans->nums, k, 1.0) : 1.0;
                        key.value = valueOf(convertValue(values->items[k], doc, kw, propName));
                        t.keys.push_back(std::move(key));
                    }
                    break;
                }
                case OcPropTrackType::Method: {
                    const Var* times = keys->find("times");
                    const Var* trans = keys->find("transitions");
                    const Var* values = keys->find("values");
                    const usize n = times ? times->nums.size() : 0;
                    for (usize k = 0; k < n && values && k < values->items.size(); ++k) {
                        const Var& call = values->items[k];
                        OcPropKey key;
                        key.time = times->nums[k];
                        key.transition = trans ? at(trans->nums, k, 1.0) : 1.0;
                        const Var* m = call.find("method");
                        key.value.kind = OcPropKind::String;
                        key.value.s = m ? m->asString() : "";
                        if (const Var* a = call.find("args"))
                            for (const Var& arg : a->items) key.args.push_back(valueOf(convertValue(arg, doc, kw, propName)));
                        t.keys.push_back(std::move(key));
                    }
                    break;
                }
                case OcPropTrackType::Bezier: {
                    const Var* times = keys->find("times");
                    const Var* points = keys->find("points");
                    const usize n = times ? times->nums.size() : 0;
                    for (usize k = 0; k < n && points && (k + 1) * 5 <= points->nums.size(); ++k) {
                        OcPropKey key;
                        key.time = times->nums[k];
                        key.value.kind = OcPropKind::Float;
                        key.value.n[0] = points->nums[k * 5];
                        key.handle[0] = points->nums[k * 5 + 1];
                        key.handle[1] = points->nums[k * 5 + 2];
                        key.handle[2] = points->nums[k * 5 + 3];
                        key.handle[3] = points->nums[k * 5 + 4];
                        t.keys.push_back(std::move(key));
                    }
                    if (keys->find("handle_modes")) {
                        rating = Rating::Approximated;
                        why = "the bezier handles are kept as stored; the editor's handle modes (balanced, mirrored) are not, so moving a key later does not re-balance them";
                    }
                    break;
                }
                case OcPropTrackType::Audio: {
                    const Var* times = keys->find("times");
                    const Var* clips = keys->find("clips");
                    const usize n = times ? times->nums.size() : 0;
                    for (usize k = 0; k < n && clips && k < clips->items.size(); ++k) {
                        const Var& c = clips->items[k];
                        OcPropKey key;
                        key.time = times->nums[k];
                        key.value.kind = OcPropKind::String;
                        if (const Var* s = c.find("stream")) {
                            if (s->type == VType::ExtResource) {
                                if (const gd::GodotExtResource* e = doc.findExt(s->s)) key.value.s = fs_.resolve(*e, &diag_, doc.file);
                            } else key.value.s = s->asString();
                        }
                        if (const Var* so = c.find("start_offset")) key.startOffset = so->asNumber();
                        if (const Var* eo = c.find("end_offset")) key.endOffset = eo->asNumber();
                        t.keys.push_back(std::move(key));
                    }
                    rating = Rating::Approximated;
                    why = "the track and its keys are kept (stream = the Godot res:// path, start and end offsets); the player reports each key as an audio event "
                          "and an AudioStreamPlayer target plays it once the audio package lands (G5_PARITY A1 audio contract)";
                    break;
                }
                case OcPropTrackType::Animation: {
                    const Var* times = keys->find("times");
                    const Var* clips = keys->find("clips");
                    const usize n = times ? times->nums.size() : 0;
                    for (usize k = 0; k < n && clips; ++k) {
                        OcPropKey key;
                        key.time = times->nums[k];
                        key.value.kind = OcPropKind::String;
                        if (k < clips->strs.size()) key.value.s = clips->strs[k];
                        else if (k < clips->items.size()) key.value.s = clips->items[k].asString();
                        t.keys.push_back(std::move(key));
                    }
                    break;
                }
            }
        }
        // Godot keeps keys sorted; a hand-edited file might not.
        std::stable_sort(t.keys.begin(), t.keys.end(), [](const OcPropKey& a, const OcPropKey& b) { return a.time < b.time; });
        if (boneTrack && rating == Rating::Converted) {
            rating = Rating::Approximated;
            why = "a bone track ('" + t.path + "'): written, and applied to the bone pose of the Skeleton3D it names (a script-level pose model, not connected to a skinned mesh)";
        }
        if (t.type == OcPropTrackType::Value && t.path.find(':') == std::string::npos) {
            rating = Rating::Approximated;
            why = "a value track needs a property in its path; this one names none, so the runtime cannot write it";
        }
        note(idx, where, rating, why, line);
        (rating == Rating::Converted ? converted : approximated) += 1;
        clip.propTracks.push_back(std::move(t));
    }

    if (getProp(props, "markers"))
        report_.add("resource", doc.file, w.node, "Animation", nameHint + ".markers", Rating::Unsupported,
                    "animation markers (and play_section_with_markers) are not in the runtime", 0);

    const std::string stem = safeName(nameHint, "animation");
    std::string unique = stem;
    for (int n = 2; animationStemsUsed_.count(unique); ++n) unique = stem + "_" + std::to_string(n);
    animationStemsUsed_.insert(unique);
    const std::string asset = "Animations/" + unique + ".ocanim";
    createDirectories(contentDir_ + "/Animations");
    std::string err;
    if (!fmt::saveOcAnim(contentDir_ + "/" + asset, clip, &err)) {
        report_.add("resource", doc.file, w.node, "Animation", nameHint, Rating::Unsupported, "the .ocanim could not be written: " + err, 0);
        return "";
    }
    report_.add("resource", doc.file, w.node, "Animation", nameHint, approximated > 0 || dropped > 0 ? Rating::Approximated : Rating::Converted,
                std::to_string(clip.propTracks.size()) + " track(s) of " + std::to_string(tracks.size()) + " written to " + asset + " (length " +
                    std::to_string(clip.duration) + " s, loop mode " + std::to_string(loop) + "); " + std::to_string(converted) + " converted, " +
                    std::to_string(approximated) + " approximated, " + std::to_string(dropped) + " dropped",
                0);
    animationAssets_[cacheKey] = asset;
    return asset;
}

Var Converter::animationLibraryValue(const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w) {
    std::vector<std::pair<std::string, Var>> entries;
    const Var* data = getProp(props, "_data");
    if (data && data->type == VType::Dictionary) {
        for (usize i = 0; i < data->keys.size() && i < data->items.size(); ++i) {
            Var cv = convertValue(data->items[i], doc, w, "_data");
            if (cv.isNil()) continue;
            entries.emplace_back(data->keys[i].asString(), std::move(cv));
        }
    }
    return makeDict(std::move(entries));
}

void Converter::noteAnimationPlayer(const FNode& fn) {
    const auto note = [&](const Prop& pr, Rating r, const char* why) {
        report_.add("property", fn.reportFile, fn.reportPath, fn.type, pr.key, r, why, pr.line);
    };
    for (const Prop& pr : fn.props) {
        const std::string& k = pr.key;
        if (k == "libraries" || k == "autoplay" || k == "speed_scale" || k == "playback_default_blend_time" || k == "blend_times" || k == "root_node" ||
            k == "active" || k == "deterministic" || k == "callback_mode_process" || k == "callback_mode_method" || k == "callback_mode_discrete" ||
            k == "playback_auto_capture" || k == "playback_auto_capture_duration" || k == "playback_auto_capture_transition_type" ||
            k == "playback_auto_capture_ease_type" || k == "current_animation" || k == "assigned_animation" || k == "current_animation_position")
            note(pr, Rating::Converted, "AnimationPlayer (Aver.Fabric) reads it; the animations are .ocanim property clips (G5_PARITY A1)");
        else if (k == "root_motion_track")
            note(pr, Rating::Converted, "the track's movement is reported by GetRootMotionPosition / Rotation / Scale (and the accumulators) instead of being applied to its node");
        else if (k == "root_motion_local")
            note(pr, Rating::Approximated, "stored; root motion is reported in the track's own space");
        else if (k == "reset_on_save")
            note(pr, Rating::Approximated, "an editor setting; nothing is saved from a running game");
        else if (k == "audio_max_polyphony")
            note(pr, Rating::Approximated, "kept as a value; audio tracks report events and the audio package plays them");
        else if (k == "movie_quit_on_finish")
            note(pr, Rating::Unsupported, "Movie Maker mode does not exist in Aver");
    }
}

}  // namespace aver::fabric
