// Navigation (docs/plans/fabric/G5_PARITY.md, N1): NavigationMesh resources and the properties of NavigationRegion3D, NavigationLink3D,
// NavigationObstacle3D and NavigationAgent3D.
//
// A NavigationMesh has no engine file format: Aver.Fabric builds it from `Object(NavigationMesh, "vertices": ..., "polygons": ..., "cell_size": ...)` with
// Godot's names and Godot's space (metres, +Y up, in the space of the node that holds it). The axis map is applied where the engine consumes the polygons
// (NavigationServer3D.RegionSetNavigationMesh, with the region's global transform), as for the other managed resources (G5_PARITY A2), so the converter's
// work is to resolve the value, rate each parameter against what the baker does with it, and flag a mesh that was never baked in the editor.
#include "FabricInternal.hpp"

#include <cstring>

namespace aver::fabric {

namespace {

struct Family {
    const char* type;
    std::vector<const char*> known;                            // converted
    std::vector<std::pair<const char*, const char*>> approx;   // stored but not (fully) applied: name, why
};

const std::vector<Family>& families() {
    static const std::vector<Family> f = {
        {"NavigationMesh",
         {"vertices", "polygons", "geometry_parsed_geometry_type", "geometry_collision_mask", "geometry_source_geometry_mode", "geometry_source_group_name",
          "cell_size", "cell_height", "border_size", "agent_height", "agent_radius", "agent_max_climb", "agent_max_slope", "region_min_size",
          "edge_max_length", "vertices_per_polygon", "detail_sample_max_error", "filter_low_hanging_obstacles", "filter_ledge_spans",
          "filter_walkable_low_height_spans", "filter_baking_aabb", "filter_baking_aabb_offset"},
         {{"sample_partition_type", "stored; the baker is a heightfield rasteriser with a rectangle cover, not Recast's watershed / monotone / layer partitions"},
          {"region_merge_size", "stored; the rectangle cover does not merge regions by size"},
          {"edge_max_error", "stored; the rectangle cover has no contour to simplify"},
          {"detail_sample_distance", "stored; heights are exact per cell instead of sampled"}}},
        // ---- the nodes ----
        {"NavigationRegion3D", {"navigation_mesh", "enabled", "navigation_layers", "enter_cost", "travel_cost", "use_edge_connections"}, {}},
        {"NavigationLink3D", {"enabled", "bidirectional", "navigation_layers", "start_position", "end_position", "enter_cost", "travel_cost"}, {}},
        {"NavigationObstacle3D",
         {"radius", "height", "vertices", "velocity", "avoidance_enabled", "avoidance_layers", "affect_navigation_mesh", "carve_navigation_mesh", "paused"},
         {{"use_3d_avoidance", "stored; avoidance works in the ground plane (the agent keeps its vertical velocity)"}}},
        {"NavigationAgent3D",
         {"target_position", "path_desired_distance", "target_desired_distance", "path_height_offset", "path_max_distance", "navigation_layers",
          "pathfinding_algorithm", "path_postprocessing", "path_metadata_flags", "simplify_path", "simplify_epsilon", "avoidance_enabled", "radius", "height",
          "max_speed", "neighbor_distance", "max_neighbors", "time_horizon_agents", "time_horizon_obstacles", "avoidance_layers", "avoidance_mask",
          "avoidance_priority", "keep_y_velocity", "velocity", "debug_enabled", "debug_use_custom", "debug_path_custom_color", "debug_path_custom_point_size"},
         {{"use_3d_avoidance", "stored; avoidance works in the ground plane (keep_y_velocity decides the vertical component)"},
          {"debug_enabled_avoidance", "stored; the debug drawing is not shown"}}},
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

bool isNavigationResourceType(const std::string& t) { return t == "NavigationMesh"; }

Var Converter::navigationMeshObject(const std::string& type, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w, int line) {
    const Family* fam = familyOf(type);
    std::vector<std::pair<std::string, Var>> entries;
    int approximated = 0, unknown = 0;
    size_t polygons = 0, vertices = 0;
    for (const gd::GodotProperty& p : props) {
        if (ignorable(p.name)) continue;
        bool known = false;
        for (const char* k : fam->known) if (p.name == k) { known = true; break; }
        if (!known)
            for (const auto& a : fam->approx)
                if (p.name == a.first) {
                    known = true;
                    ++approximated;
                    report_.add("property", doc.file, w.node, type, p.name, Rating::Approximated, a.second, p.line);
                    break;
                }
        if (!known) {
            ++unknown;
            report_.add("property", doc.file, w.node, type, p.name, Rating::Unsupported, "the runtime class has no such property; the value is dropped", p.line);
            continue;
        }
        if (p.name == "polygons") polygons = p.value.count();
        if (p.name == "vertices") vertices = p.value.nums.size() / 3;
        entries.emplace_back(p.name, convertValue(p.value, doc, w, p.name));
    }
    if (polygons > 0)
        report_.add("resource", doc.file, w.node, type, "", unknown ? Rating::Approximated : Rating::Converted,
                    "baked in the editor: " + std::to_string(polygons) + " polygon(s) over " + std::to_string(vertices) +
                        " vertices, built at run time from Object(NavigationMesh) in Godot space; the region places them through the axis map" +
                        (approximated ? "; " + std::to_string(approximated) + " baking parameter(s) stored but not applied" : std::string()) +
                        (unknown ? "; " + std::to_string(unknown) + " unknown property(ies) dropped" : std::string()),
                    line);
    else
        report_.add("resource", doc.file, w.node, type, "", Rating::Approximated,
                    "not baked in the editor (no polygons): the region bakes it at run time from the geometry below it (BoxMesh, ArrayMesh and box / concave collision "
                    "shapes are readable; an imported or primitive mesh converted to an .ocmesh is not), where Godot would leave it empty until bake_navigation_mesh() is called",
                    line);
    return makeObject(type, std::move(entries));
}

void Converter::noteNavigationNode(const FNode& fn) {
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
            if (k == known) {
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
    if (fn.type == "NavigationAgent3D")
        report_.add("node", fn.reportFile, fn.reportPath, fn.type, "", Rating::Converted,
                    "the agent finds paths and (with avoidance on) turns the parent's wanted velocity into a safe one; as in Godot it moves nothing, a script follows get_next_path_position()", 0);
    if (fn.type == "NavigationObstacle3D") {
        bool affects = false;
        for (const Prop& pr : fn.props) if (pr.key == "affect_navigation_mesh" && boolOf(&pr.value, false)) affects = true;
        if (affects)
            report_.add("node", fn.reportFile, fn.reportPath, fn.type, "", Rating::Converted,
                        "affect_navigation_mesh: the obstacle is cut out of the mesh when it is baked at run time; a mesh baked in the editor already has the hole (or not)", 0);
    }
}

}  // namespace aver::fabric
