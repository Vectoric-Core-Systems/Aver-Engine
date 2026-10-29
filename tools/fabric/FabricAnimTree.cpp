// AnimationTree and its node resources, Curve / Curve3D / Gradient, and the properties of the skeleton, path and helper nodes
// (docs/plans/fabric/G5_PARITY.md, A2).
//
// These resources have no engine file format: Aver.Fabric builds them from `Object(Class, "prop": value, ...)` values, with the property names the
// scene stored (`nodes/Blend2/node`, `states/Idle/position`, `blend_point_0/pos`, `_data` ...), which the runtime classes read. The converter's work is
// to resolve every nested SubResource / ExtResource into such a value, drop what is editor-only, and rate each node and property.
#include "FabricInternal.hpp"

#include <algorithm>
#include <cstring>

namespace aver::fabric {

namespace {

// A property name list entry ending in '*' matches by prefix, and "name/*" matches the indexed form (`input_*` too).
bool matches(const std::string& key, const char* pattern) {
    const size_t n = std::strlen(pattern);
    if (n > 0 && pattern[n - 1] == '*') return key.compare(0, n - 1, pattern, n - 1) == 0;
    return key == pattern;
}

struct Family {
    const char* type;
    std::vector<const char*> known;                          // converted
    std::vector<std::pair<const char*, const char*>> approx;   // stored but not (fully) applied: name, why
};

const std::vector<Family>& families() {
    static const std::vector<Family> f = {
        {"AnimationNodeAnimation", {"animation", "play_mode"},
         {{"advance_on_start", "stored; the first pass always includes the key at the start of the animation"},
          {"use_custom_timeline", "stored; the animation plays on its own timeline, length and loop mode"},
          {"timeline_length", "stored; only used with a custom timeline, which is not applied"},
          {"stretch_time_scale", "stored; only used with a custom timeline, which is not applied"},
          {"start_offset", "stored; only used with a custom timeline, which is not applied"},
          {"loop_mode", "stored; only used with a custom timeline, which is not applied (the animation's own loop mode plays)"}}},
        {"AnimationNodeBlendTree", {"graph_offset", "nodes/*", "node_connections"}, {}},
        {"AnimationNodeStateMachine", {"graph_offset", "states/*", "transitions", "state_machine_type", "allow_transition_to_self", "reset_ends"}, {}},
        {"AnimationNodeStateMachineTransition",
         {"switch_mode", "advance_mode", "advance_condition", "advance_expression", "xfade_time", "xfade_curve", "reset", "priority", "auto_advance", "disabled"}, {}},
        {"AnimationNodeBlendSpace1D", {"min_space", "max_space", "snap", "value_label", "blend_mode", "sync", "blend_point_*"}, {}},
        {"AnimationNodeBlendSpace2D", {"min_space", "max_space", "snap", "x_label", "y_label", "blend_mode", "sync", "auto_triangles", "triangles", "blend_point_*"}, {}},
        {"AnimationNodeBlend2", {"sync", "filter_enabled", "filters"}, {}},
        {"AnimationNodeBlend3", {"sync"}, {}},
        {"AnimationNodeAdd2", {"sync", "filter_enabled", "filters"}, {}},
        {"AnimationNodeAdd3", {"sync"}, {}},
        {"AnimationNodeSub2", {"sync", "filter_enabled", "filters"}, {}},
        {"AnimationNodeOneShot",
         {"sync", "filter_enabled", "filters", "mix_mode", "fadein_time", "fadein_curve", "fadeout_time", "fadeout_curve", "break_loop_at_end", "autorestart",
          "autorestart_delay", "autorestart_random_delay", "abort_on_reset"}, {}},
        {"AnimationNodeTimeScale", {}, {}},
        {"AnimationNodeTimeSeek", {}, {}},
        {"AnimationNodeTransition", {"sync", "input_count", "xfade_time", "xfade_curve", "allow_transition_to_self", "input_*"}, {}},
        {"AnimationNodeOutput", {}, {}},
        {"AnimationNodeStartState", {}, {}},
        {"AnimationNodeEndState", {}, {}},
        {"AnimationNodeSync", {"sync", "filter_enabled", "filters"}, {}},
        {"Curve", {"_data", "point_count", "min_value", "max_value", "min_domain", "max_domain", "bake_resolution"}, {}},
        {"Curve3D", {"_data", "point_count", "closed", "bake_interval", "up_vector_enabled"}, {}},
        {"Gradient", {"offsets", "colors", "interpolation_mode"},
         {{"interpolation_color_space", "stored; colours are blended in sRGB"}}},
        // ---- the nodes ----
        {"AnimationTree",
         {"tree_root", "anim_player", "active", "deterministic", "root_node", "callback_mode_process", "callback_mode_method", "callback_mode_discrete",
          "advance_expression_base_node", "root_motion_track", "parameters/*"},
         {{"root_motion_local", "stored; root motion is reported in the track's own space"},
          {"audio_max_polyphony", "stored; audio tracks report events and the audio package plays them"},
          {"reset_on_save", "an editor setting; nothing is saved from a running game"}}},
        {"Skeleton3D", {"bones/*", "motion_scale", "show_rest_only", "modifier_callback_mode_process"}, {}},
        {"BoneAttachment3D", {"bone_name", "bone_idx", "override_pose", "use_external_skeleton", "external_skeleton"}, {}},
        {"SkeletonIK3D", {"root_bone", "tip_bone", "target", "target_node", "min_distance", "max_iterations", "interpolation", "use_magnet", "magnet", "active", "influence"},
         {{"override_tip_basis", "the tip always takes the target's basis"}}},
        {"Path3D", {"curve"}, {}},
        {"PathFollow3D", {"progress", "progress_ratio", "h_offset", "v_offset", "rotation_mode", "cubic_interp", "loop", "tilt_enabled"}, {}},
        {"SpringArm3D", {"shape", "spring_length", "margin", "collision_mask"}, {}},
        {"RemoteTransform3D", {"remote_path", "use_global_coordinates", "update_position", "update_rotation", "update_scale"}, {}},
        {"VisibleOnScreenNotifier3D", {"aabb"}, {}},
        {"VisibleOnScreenEnabler3D", {"aabb", "enable_mode", "enable_node_path"}, {}},
    };
    return f;
}

const Family* familyOf(const std::string& type) {
    for (const Family& f : families()) if (type == f.type) return &f;
    return nullptr;
}

bool ignorable(const std::string& n) {
    return n == "resource_local_to_scene" || n == "resource_name" || n == "resource_path" || n == "script" || n == "metadata/_custom_type_script";
}

}  // namespace

bool isAnimTreeResourceType(const std::string& t) {
    return t.rfind("AnimationNode", 0) == 0 || t == "Curve" || t == "Curve3D" || t == "Gradient";
}

Var Converter::animTreeObject(const std::string& type, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w, int line) {
    const Family* fam = familyOf(type);
    std::vector<std::pair<std::string, Var>> entries;
    int approximated = 0, unknown = 0;
    for (const gd::GodotProperty& p : props) {
        if (ignorable(p.name)) continue;
        bool known = false;
        if (fam) {
            for (const char* k : fam->known) if (matches(p.name, k)) { known = true; break; }
            for (const auto& a : fam->approx)
                if (p.name == a.first) {
                    known = true;
                    ++approximated;
                    report_.add("property", doc.file, w.node, type, p.name, Rating::Approximated, a.second, p.line);
                    break;
                }
        }
        if (!known) {
            ++unknown;
            report_.add("property", doc.file, w.node, type, p.name, Rating::Unsupported, "the runtime class has no such property; the value is dropped", p.line);
            continue;
        }
        Var cv = convertValue(p.value, doc, w, p.name);
        if (cv.isNil() && !p.value.isNil()) continue;   // a nested resource that could not be converted; already reported
        entries.emplace_back(p.name, std::move(cv));
    }
    report_.add("resource", doc.file, w.node, type, "", approximated || unknown ? Rating::Approximated : Rating::Converted,
                "built at run time from Object(" + type + ") by Aver.Fabric" + (approximated ? "; " + std::to_string(approximated) + " property(ies) stored but not applied" : std::string()) +
                    (unknown ? "; " + std::to_string(unknown) + " unknown property(ies) dropped" : std::string()),
                line);
    return makeObject(type, std::move(entries));
}

void Converter::noteAnimTreeNode(const FNode& fn) {
    const Family* fam = familyOf(fn.type);
    if (!fam) return;
    for (const Prop& pr : fn.props) {
        const std::string& k = pr.key;
        if (k == "_name" || k == "script" || k == "unique_name_in_owner" || k.rfind("metadata/", 0) == 0 || k == "process_mode" || k == "process_priority" ||
            k == "process_physics_priority" || k == "visible" || k == "top_level" || k == "scene_file_path" || k == "position" || k == "rotation" || k == "scale" ||
            k == "transform" || k == "basis" || k == "rotation_degrees" || k == "rotation_edit_mode" || k == "rotation_order" || k == "editor_description")
            continue;
        bool done = false;
        for (const char* known : fam->known)
            if (matches(k, known)) {
                report_.add("property", fn.reportFile, fn.reportPath, fn.type, k, Rating::Converted, "Aver.Fabric " + fn.type + " reads it", pr.line);
                done = true;
                break;
            }
        if (done) continue;
        for (const auto& a : fam->approx)
            if (k == a.first) {
                report_.add("property", fn.reportFile, fn.reportPath, fn.type, k, Rating::Approximated, a.second, pr.line);
                done = true;
                break;
            }
        if (!done) report_.add("property", fn.reportFile, fn.reportPath, fn.type, k, Rating::Unsupported, "Aver.Fabric " + fn.type + " has no such property", pr.line);
    }
}

}  // namespace aver::fabric
