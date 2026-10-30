// Godot shading language: the type lattice, the per-shader-type built-in variable tables, render modes, the built-in function overload table
// and the operator typing rules. Written from the documented language; no Godot source is copied.
#include "GodotShaderInternal.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <unordered_map>

namespace aver::fmt::godot {

using B = GdBase;

// ------------------------------------------------------------------------------------------------ type helpers

GdKind gdKind(GdBase b) {
    const int v = static_cast<int>(b);
    if (b == B::Void) return GdKind::Void;
    if (b == B::Error) return GdKind::Error;
    if (b == B::Struct) return GdKind::Struct;
    if (v >= 1 && v <= 4) return GdKind::Bool;
    if (v >= 5 && v <= 8) return GdKind::Int;
    if (v >= 9 && v <= 12) return GdKind::UInt;
    if (v >= 13 && v <= 16) return GdKind::Float;
    if (v >= 17 && v <= 19) return GdKind::Matrix;
    if (gdIsSampler(b)) return GdKind::Sampler;
    return GdKind::Error;
}

int gdDim(GdBase b) {
    const int v = static_cast<int>(b);
    if (v >= 1 && v <= 16) return (v - 1) % 4 + 1;
    if (v >= 17 && v <= 19) return v - 15;
    return 0;
}

GdBase gdScalarOf(GdBase b) {
    switch (gdKind(b)) {
    case GdKind::Bool: return B::Bool;
    case GdKind::Int: return B::Int;
    case GdKind::UInt: return B::UInt;
    case GdKind::Float: case GdKind::Matrix: return B::Float;
    default: return B::Error;
    }
}

GdBase gdMakeVec(GdBase scalar, int n) {
    if (n < 1 || n > 4) return B::Error;
    return static_cast<GdBase>(static_cast<int>(scalar) + n - 1);
}

GdBase gdMakeMat(int n) { return n >= 2 && n <= 4 ? static_cast<GdBase>(15 + n) : B::Error; }

bool gdIsSampler(GdBase b) { return static_cast<int>(b) >= static_cast<int>(B::Sampler2D) && static_cast<int>(b) <= static_cast<int>(B::SamplerExternalOES); }

bool gdIsNumeric(GdBase b) {
    const GdKind k = gdKind(b);
    return k == GdKind::Int || k == GdKind::UInt || k == GdKind::Float || k == GdKind::Matrix;
}

namespace {
struct NameRow { GdBase b; const char* n; };
constexpr NameRow kNames[] = {
    {B::Void, "void"}, {B::Bool, "bool"}, {B::BVec2, "bvec2"}, {B::BVec3, "bvec3"}, {B::BVec4, "bvec4"},
    {B::Int, "int"}, {B::IVec2, "ivec2"}, {B::IVec3, "ivec3"}, {B::IVec4, "ivec4"},
    {B::UInt, "uint"}, {B::UVec2, "uvec2"}, {B::UVec3, "uvec3"}, {B::UVec4, "uvec4"},
    {B::Float, "float"}, {B::Vec2, "vec2"}, {B::Vec3, "vec3"}, {B::Vec4, "vec4"},
    {B::Mat2, "mat2"}, {B::Mat3, "mat3"}, {B::Mat4, "mat4"},
    {B::Sampler2D, "sampler2D"}, {B::ISampler2D, "isampler2D"}, {B::USampler2D, "usampler2D"},
    {B::Sampler2DArray, "sampler2DArray"}, {B::ISampler2DArray, "isampler2DArray"}, {B::USampler2DArray, "usampler2DArray"},
    {B::Sampler3D, "sampler3D"}, {B::ISampler3D, "isampler3D"}, {B::USampler3D, "usampler3D"},
    {B::SamplerCube, "samplerCube"}, {B::SamplerCubeArray, "samplerCubeArray"}, {B::SamplerExternalOES, "samplerExternalOES"},
};
}  // namespace

const char* gdBaseName(GdBase b) {
    for (const NameRow& r : kNames) if (r.b == b) return r.n;
    return b == B::Struct ? "struct" : "<error>";
}

GdBase gdBaseFromName(std::string_view name) {
    for (const NameRow& r : kNames) if (name == r.n) return r.b;
    return B::Error;
}

std::string gdTypeName(const GdType& t, const GdShaderModule* m) {
    std::string s;
    if (t.base == B::Struct) s = (m && t.structIdx >= 0 && t.structIdx < static_cast<int>(m->structs.size())) ? m->structs[t.structIdx].name : "struct";
    else s = gdBaseName(t.base);
    if (t.arraySize > 0) s += "[" + std::to_string(t.arraySize) + "]";
    return s;
}

const char* gdShaderTypeName(GdShaderType t) {
    switch (t) {
    case GdShaderType::Spatial: return "spatial";
    case GdShaderType::CanvasItem: return "canvas_item";
    case GdShaderType::Particles: return "particles";
    case GdShaderType::Sky: return "sky";
    case GdShaderType::Fog: return "fog";
    }
    return "spatial";
}

const char* gdProcessorName(GdProcessor p) {
    switch (p) {
    case GdProcessor::Vertex: return "vertex";
    case GdProcessor::Fragment: return "fragment";
    case GdProcessor::Light: return "light";
    case GdProcessor::Start: return "start";
    case GdProcessor::Process: return "process";
    case GdProcessor::Sky: return "sky";
    case GdProcessor::Fog: return "fog";
    case GdProcessor::None: break;
    }
    return "";
}

GdExpr::~GdExpr() {
    std::vector<std::unique_ptr<GdExpr>> pending;
    for (auto& a : args) pending.push_back(std::move(a));
    while (!pending.empty()) {
        std::unique_ptr<GdExpr> n = std::move(pending.back());
        pending.pop_back();
        if (!n) continue;
        for (auto& a : n->args) pending.push_back(std::move(a));
    }
}

std::string GdShaderDiag::str() const {
    std::string s = file.empty() ? std::string("<shader>") : file;
    if (line > 0) {
        s += ':' + std::to_string(line);
        if (column > 0) s += ':' + std::to_string(column);
    }
    s += severity == GodotSeverity::Error ? ": error: " : severity == GodotSeverity::Warning ? ": warning: " : ": note: ";
    s += message;
    return s;
}

void gdShaderDiagsToGodot(const std::vector<GdShaderDiag>& in, GodotDiagnostics& out) {
    for (const GdShaderDiag& d : in) {
        std::string msg = d.column > 0 ? "col " + std::to_string(d.column) + ": " + d.message : d.message;
        out.add(d.severity, d.file, d.line, d.code, msg);
    }
}

int GdShaderModule::findStruct(std::string_view name) const {
    for (usize i = 0; i < structs.size(); ++i) if (structs[i].name == name) return static_cast<int>(i);
    return -1;
}
int GdShaderModule::findUniform(std::string_view name) const {
    for (usize i = 0; i < uniforms.size(); ++i) if (uniforms[i].name == name) return static_cast<int>(i);
    return -1;
}
int GdShaderModule::findVarying(std::string_view name) const {
    for (usize i = 0; i < varyings.size(); ++i) if (varyings[i].name == name) return static_cast<int>(i);
    return -1;
}
int GdShaderModule::findFunction(std::string_view name) const {
    for (usize i = 0; i < functions.size(); ++i) if (functions[i].name == name) return static_cast<int>(i);
    return -1;
}
const GdFunction* GdShaderModule::processor(GdProcessor p) const {
    for (const GdFunction& f : functions) if (f.processor == p) return &f;
    return nullptr;
}
bool GdShaderModule::hasRenderMode(std::string_view mode) const {
    return std::find(renderModes.begin(), renderModes.end(), mode) != renderModes.end();
}
usize GdShaderModule::errorCount() const {
    usize n = 0;
    for (const GdShaderDiag& d : diags) n += d.severity == GodotSeverity::Error;
    return n;
}

// ------------------------------------------------------------------------------------------------ built-in variables

namespace {
constexpr GdAccess In = GdAccess::In, Out = GdAccess::Out, InOut = GdAccess::InOut;
constexpr u16 sV = gdStageBit(GdProcessor::Vertex), sF = gdStageBit(GdProcessor::Fragment), sL = gdStageBit(GdProcessor::Light),
              sS = gdStageBit(GdProcessor::Start), sP = gdStageBit(GdProcessor::Process), sK = gdStageBit(GdProcessor::Sky),
              sG = gdStageBit(GdProcessor::Fog), sALL = 0xFFFF;

// TIME, PI, TAU, E: readable everywhere, custom functions included.
#define GD_GLOBALS {"TIME", B::Float, In, sALL, true}, {"PI", B::Float, In, sALL, true}, {"TAU", B::Float, In, sALL, true}, {"E", B::Float, In, sALL, true}

const GdBuiltinVar kSpatial[] = {
    GD_GLOBALS,
    {"OUTPUT_IS_SRGB", B::Bool, In, sV | sF, false},
    {"VIEWPORT_SIZE", B::Vec2, In, sV | sF | sL, false},
    // vertex()
    {"VIEW_MATRIX", B::Mat4, In, sV | sF | sL, false},
    {"INV_VIEW_MATRIX", B::Mat4, In, sV | sF | sL, false},
    {"MAIN_CAM_INV_VIEW_MATRIX", B::Mat4, In, sV, false},
    {"INV_PROJECTION_MATRIX", B::Mat4, In, sV | sF | sL, false},
    {"NODE_POSITION_WORLD", B::Vec3, In, sV | sF, false},
    {"NODE_POSITION_VIEW", B::Vec3, In, sV | sF, false},
    {"CAMERA_POSITION_WORLD", B::Vec3, In, sV | sF, false},
    {"CAMERA_DIRECTION_WORLD", B::Vec3, In, sV | sF, false},
    {"CAMERA_VISIBLE_LAYERS", B::UInt, In, sV | sF, false},
    {"INSTANCE_ID", B::Int, In, sV, false},
    {"INSTANCE_CUSTOM", B::Vec4, In, sV, false},
    {"VIEW_INDEX", B::Int, In, sV | sF, false},
    {"VIEW_MONO_LEFT", B::Int, In, sV | sF, false},
    {"VIEW_RIGHT", B::Int, In, sV | sF, false},
    {"EYE_OFFSET", B::Vec3, In, sV | sF, false},
    {"VERTEX", B::Vec3, InOut, sV, false},
    {"VERTEX_ID", B::Int, In, sV, false},
    {"POSITION", B::Vec4, Out, sV, false},
    {"ROUGHNESS", B::Float, Out, sV | sF, false},
    {"POINT_SIZE", B::Float, InOut, sV, false},
    {"MODELVIEW_MATRIX", B::Mat4, InOut, sV, false},
    {"MODELVIEW_NORMAL_MATRIX", B::Mat3, InOut, sV, false},
    {"MODEL_MATRIX", B::Mat4, In, sV | sF | sL, false},
    {"MODEL_NORMAL_MATRIX", B::Mat3, In, sV | sF, false},
    {"PROJECTION_MATRIX", B::Mat4, InOut, sV | sF | sL, false},
    {"BONE_INDICES", B::UVec4, In, sV, false},
    {"BONE_WEIGHTS", B::Vec4, In, sV, false},
    {"CUSTOM0", B::Vec4, In, sV, false}, {"CUSTOM1", B::Vec4, In, sV, false}, {"CUSTOM2", B::Vec4, In, sV, false}, {"CUSTOM3", B::Vec4, In, sV, false},
    {"Z_CLIP_SCALE", B::Float, Out, sV, false},
    // shared by vertex() (inout) and fragment() (in / inout)
    {"NORMAL", B::Vec3, InOut, sV | sF, false},
    {"NORMAL", B::Vec3, In, sL, false},
    {"VERTEX", B::Vec3, In, sF | sL, false},
    {"UV", B::Vec2, In, sF | sL, false},
    {"UV2", B::Vec2, In, sF | sL, false},
    {"COLOR", B::Vec4, In, sF, false},
    {"ALBEDO", B::Vec3, In, sL, false},
    {"METALLIC", B::Float, In, sL, false},
    {"ROUGHNESS", B::Float, In, sL, false},
    {"TANGENT", B::Vec3, InOut, sV | sF, false},
    {"BINORMAL", B::Vec3, InOut, sV | sF, false},
    {"UV", B::Vec2, InOut, sV, false},
    {"UV2", B::Vec2, InOut, sV, false},
    {"COLOR", B::Vec4, InOut, sV, false},
    // fragment()
    {"FRAGCOORD", B::Vec4, In, sF | sL, false},
    {"FRONT_FACING", B::Bool, In, sF, false},
    {"VIEW", B::Vec3, In, sF | sL, false},
    {"POINT_COORD", B::Vec2, In, sF, false},
    {"LIGHT_VERTEX", B::Vec3, InOut, sF, false},
    {"SCREEN_UV", B::Vec2, In, sF | sL, false},
    {"DEPTH", B::Float, Out, sF, false},
    {"NORMAL_MAP", B::Vec3, Out, sF, false},
    {"NORMAL_MAP_DEPTH", B::Float, Out, sF, false},
    {"BENT_NORMAL_MAP", B::Vec3, Out, sF, false},
    {"ALBEDO", B::Vec3, Out, sF, false},
    {"ALPHA", B::Float, Out, sF | sL, false},
    {"ALPHA_SCISSOR_THRESHOLD", B::Float, Out, sF, false},
    {"ALPHA_HASH_SCALE", B::Float, Out, sF, false},
    {"ALPHA_ANTIALIASING_EDGE", B::Float, Out, sF, false},
    {"ALPHA_TEXTURE_COORDINATE", B::Vec2, Out, sF, false},
    {"PREMUL_ALPHA_FACTOR", B::Float, Out, sF, false},
    {"METALLIC", B::Float, Out, sF, false},
    {"SPECULAR", B::Float, Out, sF, false},
    {"RIM", B::Float, Out, sF, false},
    {"RIM_TINT", B::Float, Out, sF, false},
    {"CLEARCOAT", B::Float, Out, sF, false},
    {"CLEARCOAT_GLOSS", B::Float, Out, sF, false},
    {"ANISOTROPY", B::Float, Out, sF, false},
    {"ANISOTROPY_FLOW", B::Vec2, Out, sF, false},
    {"SSS_STRENGTH", B::Float, Out, sF, false},
    {"SSS_TRANSMITTANCE_COLOR", B::Vec4, Out, sF, false},
    {"SSS_TRANSMITTANCE_DEPTH", B::Float, Out, sF, false},
    {"SSS_TRANSMITTANCE_BOOST", B::Float, Out, sF, false},
    {"BACKLIGHT", B::Vec3, InOut, sF | sL, false},
    {"AO", B::Float, Out, sF, false},
    {"AO_LIGHT_AFFECT", B::Float, Out, sF, false},
    {"EMISSION", B::Vec3, Out, sF, false},
    {"FOG", B::Vec4, Out, sF, false},
    {"RADIANCE", B::Vec4, Out, sF, false},
    {"IRRADIANCE", B::Vec4, Out, sF, false},
    // light()
    {"LIGHT", B::Vec3, In, sL, false},
    {"LIGHT_COLOR", B::Vec3, In, sL, false},
    {"SPECULAR_AMOUNT", B::Float, In, sL, false},
    {"LIGHT_IS_DIRECTIONAL", B::Bool, In, sL, false},
    {"ATTENUATION", B::Float, In, sL, false},
    {"DIFFUSE_LIGHT", B::Vec3, Out, sL, false},
    {"SPECULAR_LIGHT", B::Vec3, Out, sL, false},
};

// Fragment-only overrides of the shared rows: in fragment() UV / UV2 / COLOR are inputs and light() has the same plus ALBEDO / METALLIC / ROUGHNESS inputs.
// The table keeps one row per name; accessFor() below adjusts the access for the stage.

const GdBuiltinVar kParticles[] = {
    GD_GLOBALS,
    {"LIFETIME", B::Float, In, sS | sP, false}, {"DELTA", B::Float, In, sS | sP, false},
    {"NUMBER", B::UInt, In, sS | sP, false}, {"INDEX", B::UInt, In, sS | sP, false},
    {"EMISSION_TRANSFORM", B::Mat4, In, sS | sP, false}, {"RANDOM_SEED", B::UInt, In, sS | sP, false},
    {"ACTIVE", B::Bool, InOut, sS | sP, false}, {"COLOR", B::Vec4, InOut, sS | sP, false},
    {"VELOCITY", B::Vec3, InOut, sS | sP, false}, {"TRANSFORM", B::Mat4, InOut, sS | sP, false},
    {"CUSTOM", B::Vec4, InOut, sS | sP, false}, {"MASS", B::Float, InOut, sS | sP, false},
    {"USERDATA1", B::Vec4, In, sS | sP, false}, {"USERDATA2", B::Vec4, In, sS | sP, false}, {"USERDATA3", B::Vec4, In, sS | sP, false},
    {"USERDATA4", B::Vec4, In, sS | sP, false}, {"USERDATA5", B::Vec4, In, sS | sP, false}, {"USERDATA6", B::Vec4, In, sS | sP, false},
    {"FLAG_EMIT_POSITION", B::UInt, In, sS | sP, false}, {"FLAG_EMIT_ROT_SCALE", B::UInt, In, sS | sP, false},
    {"FLAG_EMIT_VELOCITY", B::UInt, In, sS | sP, false}, {"FLAG_EMIT_COLOR", B::UInt, In, sS | sP, false},
    {"FLAG_EMIT_CUSTOM", B::UInt, In, sS | sP, false},
    {"EMITTER_VELOCITY", B::Vec3, In, sS | sP, false}, {"INTERPOLATE_TO_END", B::Float, In, sS | sP, false},
    {"AMOUNT_RATIO", B::Float, In, sS | sP, false},
    {"RESTART_POSITION", B::Bool, In, sS, false}, {"RESTART_ROT_SCALE", B::Bool, In, sS, false}, {"RESTART_VELOCITY", B::Bool, In, sS, false},
    {"RESTART_COLOR", B::Bool, In, sS, false}, {"RESTART_CUSTOM", B::Bool, In, sS, false},
    {"RESTART", B::Bool, In, sP, false}, {"COLLIDED", B::Bool, In, sP, false}, {"COLLISION_NORMAL", B::Vec3, In, sP, false},
    {"COLLISION_DEPTH", B::Float, In, sP, false}, {"ATTRACTOR_FORCE", B::Vec3, In, sP, false},
};

const GdBuiltinVar kSky[] = {
    GD_GLOBALS,
    {"POSITION", B::Vec3, In, sALL, true}, {"RADIANCE", B::SamplerCube, In, sALL, true},
    {"AT_HALF_RES_PASS", B::Bool, In, sALL, true}, {"AT_QUARTER_RES_PASS", B::Bool, In, sALL, true}, {"AT_CUBEMAP_PASS", B::Bool, In, sALL, true},
    {"LIGHT0_ENABLED", B::Bool, In, sALL, true}, {"LIGHT0_ENERGY", B::Float, In, sALL, true}, {"LIGHT0_DIRECTION", B::Vec3, In, sALL, true},
    {"LIGHT0_COLOR", B::Vec3, In, sALL, true}, {"LIGHT0_SIZE", B::Float, In, sALL, true},
    {"LIGHT1_ENABLED", B::Bool, In, sALL, true}, {"LIGHT1_ENERGY", B::Float, In, sALL, true}, {"LIGHT1_DIRECTION", B::Vec3, In, sALL, true},
    {"LIGHT1_COLOR", B::Vec3, In, sALL, true}, {"LIGHT1_SIZE", B::Float, In, sALL, true},
    {"LIGHT2_ENABLED", B::Bool, In, sALL, true}, {"LIGHT2_ENERGY", B::Float, In, sALL, true}, {"LIGHT2_DIRECTION", B::Vec3, In, sALL, true},
    {"LIGHT2_COLOR", B::Vec3, In, sALL, true}, {"LIGHT2_SIZE", B::Float, In, sALL, true},
    {"LIGHT3_ENABLED", B::Bool, In, sALL, true}, {"LIGHT3_ENERGY", B::Float, In, sALL, true}, {"LIGHT3_DIRECTION", B::Vec3, In, sALL, true},
    {"LIGHT3_COLOR", B::Vec3, In, sALL, true}, {"LIGHT3_SIZE", B::Float, In, sALL, true},
    {"EYEDIR", B::Vec3, In, sK, false}, {"SCREEN_UV", B::Vec2, In, sK, false}, {"SKY_COORDS", B::Vec2, In, sK, false},
    {"HALF_RES_COLOR", B::Vec4, In, sK, false}, {"QUARTER_RES_COLOR", B::Vec4, In, sK, false},
    {"COLOR", B::Vec3, Out, sK, false}, {"ALPHA", B::Float, Out, sK, false}, {"FOG", B::Vec4, Out, sK, false},
};

const GdBuiltinVar kFog[] = {
    GD_GLOBALS,
    {"WORLD_POSITION", B::Vec3, In, sG, false}, {"OBJECT_POSITION", B::Vec3, In, sG, false}, {"UVW", B::Vec3, In, sG, false},
    {"SIZE", B::Vec3, In, sG, false}, {"SDF", B::Float, In, sG, false},
    {"ALBEDO", B::Vec3, Out, sG, false}, {"DENSITY", B::Float, Out, sG, false}, {"EMISSION", B::Vec3, Out, sG, false},
};

// canvas_item is parsed so that a 2D shader reports "unsupported" with its real contents checked, not rejected at the first built-in.
const GdBuiltinVar kCanvas[] = {
    GD_GLOBALS,
    {"VIEWPORT_SIZE", B::Vec2, In, sV | sF, false},
    {"MODEL_MATRIX", B::Mat4, InOut, sV, false}, {"CANVAS_MATRIX", B::Mat4, In, sV | sF, false}, {"SCREEN_MATRIX", B::Mat4, In, sV | sF, false},
    {"INSTANCE_ID", B::Int, In, sV, false}, {"INSTANCE_CUSTOM", B::Vec4, In, sV, false}, {"AT_LIGHT_PASS", B::Bool, In, sV | sF, false},
    {"TEXTURE_PIXEL_SIZE", B::Vec2, In, sV | sF | sL, false}, {"VERTEX", B::Vec2, InOut, sV | sF, false}, {"VERTEX_ID", B::Int, In, sV, false},
    {"UV", B::Vec2, InOut, sV | sF | sL, false}, {"COLOR", B::Vec4, InOut, sV | sF | sL, false}, {"POINT_SIZE", B::Float, InOut, sV, false},
    {"BONE_INDICES", B::IVec4, In, sV, false}, {"BONE_WEIGHTS", B::Vec4, In, sV, false}, {"CUSTOM0", B::Vec4, In, sV, false}, {"CUSTOM1", B::Vec4, In, sV, false},
    {"FRAGCOORD", B::Vec4, In, sF | sL, false}, {"SCREEN_UV", B::Vec2, In, sF | sL, false}, {"SCREEN_PIXEL_SIZE", B::Vec2, In, sF, false},
    {"POINT_COORD", B::Vec2, In, sF | sL, false}, {"NORMAL", B::Vec3, InOut, sF | sL, false}, {"NORMAL_MAP", B::Vec3, Out, sF, false},
    {"NORMAL_MAP_DEPTH", B::Float, Out, sF, false}, {"TEXTURE", B::Sampler2D, In, sF | sL, false}, {"NORMAL_TEXTURE", B::Sampler2D, In, sF, false},
    {"SPECULAR_SHININESS_TEXTURE", B::Sampler2D, In, sF, false}, {"SPECULAR_SHININESS", B::Vec4, InOut, sF | sL, false},
    {"LIGHT_VERTEX", B::Vec3, InOut, sF, false}, {"SHADOW_VERTEX", B::Vec2, InOut, sF, false},
    {"LIGHT_POSITION", B::Vec3, In, sL, false}, {"LIGHT_DIRECTION", B::Vec3, In, sL, false}, {"LIGHT_IS_DIRECTIONAL", B::Bool, In, sL, false},
    {"LIGHT_COLOR", B::Vec4, In, sL, false}, {"LIGHT_ENERGY", B::Float, In, sL, false}, {"LIGHT", B::Vec4, InOut, sL, false},
    {"SHADOW_MODULATE", B::Vec4, In, sL, false}, {"LIGHT_VERTEX", B::Vec3, In, sL, false},
};

struct RenderModes { GdShaderType t; std::vector<std::string_view> modes; };
const RenderModes kRenderModes[] = {
    {GdShaderType::Spatial, {"blend_mix", "blend_add", "blend_sub", "blend_mul", "blend_premul_alpha", "depth_draw_opaque", "depth_draw_always",
                             "depth_draw_never", "depth_prepass_alpha", "depth_test_disabled", "depth_test_default", "depth_test_inverted",
                             "sss_mode_skin", "cull_back", "cull_front", "cull_disabled", "unshaded", "wireframe", "debug_shadow_splits",
                             "diffuse_burley", "diffuse_lambert", "diffuse_lambert_wrap", "diffuse_toon", "specular_schlick_ggx", "specular_toon",
                             "specular_disabled", "skip_vertex_transform", "world_vertex_coords", "ensure_correct_normals", "shadows_disabled",
                             "ambient_light_disabled", "shadow_to_opacity", "vertex_lighting", "particle_trails", "alpha_to_coverage",
                             "alpha_to_coverage_and_one", "fog_disabled"}},
    {GdShaderType::CanvasItem, {"blend_mix", "blend_add", "blend_sub", "blend_mul", "blend_premul_alpha", "blend_disabled", "unshaded", "light_only",
                                "skip_vertex_transform", "world_vertex_coords"}},
    {GdShaderType::Particles, {"keep_data", "disable_force", "disable_velocity", "collision_use_scale"}},
    {GdShaderType::Sky, {"use_half_res_pass", "use_quarter_res_pass", "disable_fog"}},
    {GdShaderType::Fog, {}},
};
}  // namespace

const GdBuiltinVar* gdBuiltinVars(GdShaderType t, int& count) {
    switch (t) {
    case GdShaderType::Spatial: count = static_cast<int>(std::size(kSpatial)); return kSpatial;
    case GdShaderType::CanvasItem: count = static_cast<int>(std::size(kCanvas)); return kCanvas;
    case GdShaderType::Particles: count = static_cast<int>(std::size(kParticles)); return kParticles;
    case GdShaderType::Sky: count = static_cast<int>(std::size(kSky)); return kSky;
    case GdShaderType::Fog: count = static_cast<int>(std::size(kFog)); return kFog;
    }
    count = 0;
    return nullptr;
}

bool gdRenderModeValid(GdShaderType t, std::string_view mode) {
    for (const RenderModes& r : kRenderModes)
        if (r.t == t) return std::find(r.modes.begin(), r.modes.end(), mode) != r.modes.end();
    return false;
}

// ------------------------------------------------------------------------------------------------ built-in functions

namespace detail {
namespace {

// Signature mini-language: "RET(A,B,C)" ; alternatives separated by ';'. A leading '2' limits the generic size to 2..4.
//   F I U B : float / int / uint / bool scalar-or-vector of the generic size   f i u b : the scalars
//   v2 v3 v4 iv2.. uv2.. bv2..: vectors   m2 m3 m4: matrices   M: matrix of the generic size (2..4)   oX: an out parameter of X
struct SigRow { const char* name; const char* sigs; const char* hlsl; };

const SigRow kFns[] = {
    {"radians", "F(F)", ""}, {"degrees", "F(F)", ""}, {"sin", "F(F)", ""}, {"cos", "F(F)", ""}, {"tan", "F(F)", ""},
    {"asin", "F(F)", ""}, {"acos", "F(F)", ""}, {"atan", "F(F);F(F,F)", "@atan"},
    {"sinh", "F(F)", ""}, {"cosh", "F(F)", ""}, {"tanh", "F(F)", ""}, {"asinh", "F(F)", "emb_asinh"}, {"acosh", "F(F)", "emb_acosh"}, {"atanh", "F(F)", "emb_atanh"},
    {"pow", "F(F,F)", ""}, {"exp", "F(F)", ""}, {"exp2", "F(F)", ""}, {"log", "F(F)", ""}, {"log2", "F(F)", ""}, {"sqrt", "F(F)", ""},
    {"inversesqrt", "F(F)", "rsqrt"},
    {"abs", "F(F);I(I)", ""}, {"sign", "F(F);I(I)", "emb_sign"}, {"floor", "F(F)", ""}, {"trunc", "F(F)", ""}, {"round", "F(F)", "round"},
    {"roundEven", "F(F)", "round"}, {"ceil", "F(F)", ""}, {"fract", "F(F)", "frac"},
    {"mod", "F(F,F);F(F,f)", "emb_mod"}, {"modf", "F(F,oF)", ""},
    {"min", "F(F,F);F(F,f);I(I,I);I(I,i);U(U,U);U(U,u)", "@splat"}, {"max", "F(F,F);F(F,f);I(I,I);I(I,i);U(U,U);U(U,u)", "@splat"},
    {"clamp", "F(F,F,F);F(F,f,f);I(I,I,I);I(I,i,i);U(U,U,U);U(U,u,u)", "@splat"},
    {"mix", "F(F,F,F);F(F,F,f);F(F,F,B)", "emb_mix"}, {"fma", "F(F,F,F)", "mad"},
    {"isinf", "B(F)", ""}, {"isnan", "B(F)", ""},
    {"step", "F(F,F);F(f,F)", "emb_step"}, {"smoothstep", "F(F,F,F);F(f,f,F)", "emb_smoothstep"},
    {"floatBitsToInt", "I(F)", "asint"}, {"floatBitsToUint", "U(F)", "asuint"}, {"intBitsToFloat", "F(I)", "asfloat"}, {"uintBitsToFloat", "F(U)", "asfloat"},
    {"length", "f(F)", ""}, {"distance", "f(F,F)", ""}, {"dot", "f(F,F)", ""}, {"cross", "v3(v3,v3)", ""}, {"normalize", "F(F)", ""},
    {"reflect", "F(F,F)", ""}, {"refract", "F(F,F,f)", ""}, {"faceforward", "F(F,F,F)", ""},
    {"matrixCompMult", "M(M,M)", "@compmult"}, {"outerProduct", "m2(v2,v2);m3(v3,v3);m4(v4,v4)", "emb_outer"},
    {"transpose", "M(M)", ""}, {"determinant", "f(M)", ""}, {"inverse", "M(M)", "emb_inverse"},
    {"lessThan", "2B(F,F);2B(I,I);2B(U,U)", "@cmp<"}, {"lessThanEqual", "2B(F,F);2B(I,I);2B(U,U)", "@cmp<="},
    {"greaterThan", "2B(F,F);2B(I,I);2B(U,U)", "@cmp>"}, {"greaterThanEqual", "2B(F,F);2B(I,I);2B(U,U)", "@cmp>="},
    {"equal", "2B(F,F);2B(I,I);2B(U,U);2B(B,B)", "@cmp=="}, {"notEqual", "2B(F,F);2B(I,I);2B(U,U);2B(B,B)", "@cmp!="},
    {"any", "2b(B)", ""}, {"all", "2b(B)", ""}, {"not", "2B(B)", "@not"},
    {"dFdx", "F(F)", "ddx"}, {"dFdy", "F(F)", "ddy"}, {"fwidth", "F(F)", ""},
    {"dFdxCoarse", "F(F)", "ddx_coarse"}, {"dFdyCoarse", "F(F)", "ddy_coarse"}, {"dFdxFine", "F(F)", "ddx_fine"}, {"dFdyFine", "F(F)", "ddy_fine"},
    {"fwidthCoarse", "F(F)", "emb_fwidthCoarse"}, {"fwidthFine", "F(F)", "emb_fwidthFine"},
    {"packHalf2x16", "u(v2)", "emb_packHalf2x16"}, {"unpackHalf2x16", "v2(u)", "emb_unpackHalf2x16"},
    {"packUnorm2x16", "u(v2)", "emb_packUnorm2x16"}, {"unpackUnorm2x16", "v2(u)", "emb_unpackUnorm2x16"},
    {"packSnorm2x16", "u(v2)", "emb_packSnorm2x16"}, {"unpackSnorm2x16", "v2(u)", "emb_unpackSnorm2x16"},
    {"packUnorm4x8", "u(v4)", "emb_packUnorm4x8"}, {"unpackUnorm4x8", "v4(u)", "emb_unpackUnorm4x8"},
    {"packSnorm4x8", "u(v4)", "emb_packSnorm4x8"}, {"unpackSnorm4x8", "v4(u)", "emb_unpackSnorm4x8"},
    {"bitfieldExtract", "I(I,i,i);U(U,i,i)", "emb_bitfieldExtract"}, {"bitfieldInsert", "I(I,I,i,i);U(U,U,i,i)", "emb_bitfieldInsert"},
    {"bitfieldReverse", "I(I);U(U)", "reversebits"}, {"bitCount", "I(I);I(U)", "emb_bitCount"},
    {"findLSB", "I(I);I(U)", "emb_findLSB"}, {"findMSB", "I(I);I(U)", "emb_findMSB"},
    {"frexp", "F(F,oI)", "!"}, {"ldexp", "F(F,I)", "!"},
    {"umulExtended", "U(U,U,oU,oU)", "!"}, {"imulExtended", "I(I,I,oI,oI)", "!"}, {"uaddCarry", "U(U,U,oU)", "!"}, {"usubBorrow", "U(U,U,oU)", "!"},
};

GdBase vecOf(char k, int n) {
    switch (k) {
    case 'F': case 'f': return gdMakeVec(B::Float, n);
    case 'I': case 'i': return gdMakeVec(B::Int, n);
    case 'U': case 'u': return gdMakeVec(B::UInt, n);
    default: return gdMakeVec(B::Bool, n);
    }
}

GdBase parseTok(const std::string& tok, int n, bool& isOut) {
    isOut = false;
    std::string t = tok;
    if (t.size() > 1 && t[0] == 'o' && (t[1] == 'F' || t[1] == 'I' || t[1] == 'U' || t[1] == 'B' || t[1] == 'f' || t[1] == 'i' || t[1] == 'u')) {
        isOut = true;
        t = t.substr(1);
    }
    if (t == "F" || t == "I" || t == "U" || t == "B") return vecOf(t[0], n);
    if (t == "f" || t == "i" || t == "u" || t == "b") return vecOf(t[0], 1);
    if (t == "M") return gdMakeMat(n);
    if (t.size() == 2 && t[0] == 'm') return gdMakeMat(t[1] - '0');
    if (t.size() == 2 && t[0] == 'v') return gdMakeVec(B::Float, t[1] - '0');
    if (t.size() == 3 && t[1] == 'v') return gdMakeVec(t[0] == 'i' ? B::Int : t[0] == 'u' ? B::UInt : B::Bool, t[2] - '0');
    return B::Error;
}

struct Table {
    std::vector<BuiltinOverload> all;
    std::unordered_map<std::string, std::vector<int>> byName;
    std::vector<int> none;
    void add(BuiltinOverload o) {
        for (int id : byName[o.name]) {
            const BuiltinOverload& e = all[static_cast<usize>(id)];
            if (e.params == o.params && e.outParam == o.outParam && e.ret == o.ret) return;   // the scalar case of a generic signature repeats
        }
        byName[o.name].push_back(static_cast<int>(all.size()));
        all.push_back(std::move(o));
    }
};

void expand(Table& t, const SigRow& row) {
    std::string sigs = row.sigs;
    for (usize pos = 0; pos <= sigs.size();) {
        usize e = sigs.find(';', pos);
        if (e == std::string::npos) e = sigs.size();
        std::string s = sigs.substr(pos, e - pos);
        pos = e + 1;
        if (s.empty()) continue;
        int nMin = 1;
        if (s[0] == '2') { nMin = 2; s = s.substr(1); }
        const usize lp = s.find('(');
        const std::string ret = s.substr(0, lp);
        std::vector<std::string> ps;
        {
            const usize rp = s.rfind(')');
            const std::string inner = s.substr(lp + 1, rp - lp - 1);
            std::string cur;
            for (char c : inner) {
                if (c == ',') { ps.push_back(cur); cur.clear(); }
                else cur += c;
            }
            if (!cur.empty()) ps.push_back(cur);
        }
        const bool generic = ret == "F" || ret == "I" || ret == "U" || ret == "B" || ret == "M" ||
                             std::any_of(ps.begin(), ps.end(), [](const std::string& p) { return p == "F" || p == "I" || p == "U" || p == "B" || p == "oF" || p == "oI" || p == "oU" || p == "M"; });
        const bool mat = ret == "M" || std::find(ps.begin(), ps.end(), "M") != ps.end();
        const int lo = mat ? 2 : nMin, hi = generic ? 4 : lo;
        for (int n = lo; n <= hi; ++n) {
            BuiltinOverload o;
            o.name = row.name;
            o.hlsl = row.hlsl;
            bool isOut = false;
            o.ret.base = parseTok(ret, n, isOut);
            for (const std::string& p : ps) {
                const GdBase pb = parseTok(p, n, isOut);
                o.params.push_back(pb);
                o.outParam.push_back(isOut);
            }
            t.add(std::move(o));
            if (!generic) break;
        }
    }
}

void addTextureFns(Table& t) {
    struct Fam { GdBase s; GdBase ret; GdBase coord; GdBase coordProj; GdBase icoord; GdBase size; GdBase grad; bool lodOk; };
    const GdBase V2 = B::Vec2, V3 = B::Vec3, V4 = B::Vec4;
    // sampler, element vector, texture coordinate, projective coordinate (Error = none), integer texel coordinate, textureSize result, gradient
    const Fam fams[] = {
        {B::Sampler2D, V4, V2, V3, B::IVec2, B::IVec2, V2, true},
        {B::ISampler2D, B::IVec4, V2, V3, B::IVec2, B::IVec2, V2, true},
        {B::USampler2D, B::UVec4, V2, V3, B::IVec2, B::IVec2, V2, true},
        {B::Sampler2DArray, V4, V3, B::Error, B::IVec3, B::IVec3, V2, true},
        {B::ISampler2DArray, B::IVec4, V3, B::Error, B::IVec3, B::IVec3, V2, true},
        {B::USampler2DArray, B::UVec4, V3, B::Error, B::IVec3, B::IVec3, V2, true},
        {B::Sampler3D, V4, V3, V4, B::IVec3, B::IVec3, V3, true},
        {B::ISampler3D, B::IVec4, V3, V4, B::IVec3, B::IVec3, V3, true},
        {B::USampler3D, B::UVec4, V3, V4, B::IVec3, B::IVec3, V3, true},
        {B::SamplerCube, V4, V3, B::Error, B::Error, B::IVec2, V3, true},
        {B::SamplerCubeArray, V4, V4, B::Error, B::Error, B::IVec3, V3, true},
    };
    auto mk = [&](const char* name, GdBase ret, std::vector<GdBase> ps, const char* hlsl) {
        BuiltinOverload o;
        o.name = name;
        o.ret.base = ret;
        o.params = std::move(ps);
        o.outParam.assign(o.params.size(), false);
        o.hlsl = hlsl;
        t.add(std::move(o));
    };
    for (const Fam& f : fams) {
        mk("texture", f.ret, {f.s, f.coord}, "emb_texture");
        mk("texture", f.ret, {f.s, f.coord, B::Float}, "emb_texture");
        mk("textureLod", f.ret, {f.s, f.coord, B::Float}, "emb_textureLod");
        mk("textureGrad", f.ret, {f.s, f.coord, f.grad, f.grad}, "emb_textureGrad");
        mk("textureSize", f.size, {f.s, B::Int}, "emb_textureSize");
        mk("textureQueryLevels", B::Int, {f.s}, "emb_textureQueryLevels");
        const bool arr2d = f.s == B::Sampler2DArray || f.s == B::ISampler2DArray || f.s == B::USampler2DArray;
        mk("textureQueryLod", V2, {f.s, arr2d ? V2 : f.s == B::SamplerCubeArray ? V3 : f.coord}, "emb_textureQueryLod");
        if (f.coordProj != B::Error) {
            mk("textureProj", f.ret, {f.s, f.coordProj}, "emb_textureProj");
            mk("textureProj", f.ret, {f.s, V4}, "emb_textureProj");
            mk("textureProj", f.ret, {f.s, f.coordProj, B::Float}, "emb_textureProj");
            mk("textureProjLod", f.ret, {f.s, f.coordProj, B::Float}, "emb_textureProjLod");
            mk("textureProjLod", f.ret, {f.s, V4, B::Float}, "emb_textureProjLod");
        }
        if (f.icoord != B::Error) mk("texelFetch", f.ret, {f.s, f.icoord, B::Int}, "emb_texelFetch");
        if (f.s != B::Sampler3D && f.s != B::ISampler3D && f.s != B::USampler3D) {
            mk("textureGather", f.ret, {f.s, f.coord}, "emb_textureGather");
            mk("textureGather", f.ret, {f.s, f.coord, B::Int}, "emb_textureGather");
        }
    }
}

const Table& table() {
    static const Table t = [] {
        Table x;
        for (const SigRow& r : kFns) expand(x, r);
        addTextureFns(x);
        return x;
    }();
    return t;
}
}  // namespace

const std::vector<BuiltinOverload>& builtinOverloads() { return table().all; }

const std::vector<int>& builtinOverloadIds(std::string_view name) {
    const Table& t = table();
    auto it = t.byName.find(std::string(name));
    return it == t.byName.end() ? t.none : it->second;
}

// ------------------------------------------------------------------------------------------------ operators

namespace {
GdType T(GdBase b) { GdType t; t.base = b; return t; }
GdType err() { return T(B::Error); }
bool isIntLike(GdKind k) { return k == GdKind::Int || k == GdKind::UInt; }
bool isNumKind(GdKind k) { return k == GdKind::Int || k == GdKind::UInt || k == GdKind::Float; }
}  // namespace

GdType binaryResultType(std::string_view op, const GdType& ta, const GdType& tb) {
    if (ta.isArray() || tb.isArray() || ta.base == B::Error || tb.base == B::Error) return err();
    const GdBase a = ta.base, b = tb.base;
    const GdKind ka = gdKind(a), kb = gdKind(b);
    if (op == "&&" || op == "||") return a == B::Bool && b == B::Bool ? T(B::Bool) : err();
    if (op == "==" || op == "!=") {
        if (ka == GdKind::Void || ka == GdKind::Sampler) return err();
        if (a != b || ta.structIdx != tb.structIdx) return err();
        return T(B::Bool);
    }
    if (op == "<" || op == ">" || op == "<=" || op == ">=") {
        if (a != b || !isNumKind(ka) || gdDim(a) != 1) return err();
        return T(B::Bool);
    }
    const bool arith = op == "+" || op == "-" || op == "*" || op == "/";
    if (arith) {
        if (ka == GdKind::Matrix || kb == GdKind::Matrix) {
            if (ka == GdKind::Matrix && kb == GdKind::Matrix) return a == b ? T(a) : err();
            if (op == "*") {
                if (ka == GdKind::Matrix && b == gdMakeVec(B::Float, gdDim(a))) return T(b);   // mat * vec
                if (kb == GdKind::Matrix && a == gdMakeVec(B::Float, gdDim(b))) return T(a);   // vec * mat
            }
            if (ka == GdKind::Matrix && b == B::Float) return T(a);
            if (kb == GdKind::Matrix && a == B::Float) return T(b);
            return err();
        }
        if (!isNumKind(ka) || !isNumKind(kb) || ka != kb) return err();
        if (a == b) return T(a);
        if (gdDim(a) == 1) return T(b);    // scalar op vector
        if (gdDim(b) == 1) return T(a);    // vector op scalar
        return err();
    }
    if (op == "%") {
        if (!isIntLike(ka) || ka != kb) return err();
        if (a == b) return T(a);
        if (gdDim(a) == 1) return T(b);
        if (gdDim(b) == 1) return T(a);
        return err();
    }
    if (op == "<<" || op == ">>") {
        if (!isIntLike(ka) || !isIntLike(kb)) return err();
        if (gdDim(a) == gdDim(b) || gdDim(b) == 1) return T(a);
        return err();
    }
    if (op == "&" || op == "|" || op == "^") {
        if (!isIntLike(ka) || ka != kb) return err();
        if (a == b) return T(a);
        if (gdDim(a) == 1) return T(b);
        if (gdDim(b) == 1) return T(a);
        return err();
    }
    return err();
}

GdType unaryResultType(std::string_view op, const GdType& t) {
    if (t.isArray() || t.base == B::Error) return err();
    const GdKind k = gdKind(t.base);
    if (op == "!") return t.base == B::Bool ? T(B::Bool) : err();
    if (op == "~") return isIntLike(k) ? T(t.base) : err();
    if (op == "-" || op == "+") return (isNumKind(k) || k == GdKind::Matrix) ? T(t.base) : err();
    if (op == "++" || op == "--") return (isNumKind(k)) ? T(t.base) : err();
    return err();
}

std::string normaliseSwizzle(std::string_view sw, int dim) {
    if (sw.empty() || sw.size() > 4 || dim < 2) return {};
    const char* sets[3] = {"xyzw", "rgba", "stpq"};
    int set = -1;
    std::string out;
    for (char c : sw) {
        int s = -1, idx = -1;
        for (int k = 0; k < 3; ++k)
            for (int i = 0; i < 4; ++i)
                if (sets[k][i] == c) { s = k; idx = i; }
        if (s < 0 || (set >= 0 && s != set) || idx >= dim) return {};
        set = s;
        out += sets[0][idx];
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ constant folding

namespace {
double convertComp(double v, GdKind to) {
    switch (to) {
    case GdKind::Bool: return v != 0.0 ? 1.0 : 0.0;
    case GdKind::Int: return std::trunc(v);
    case GdKind::UInt: return std::trunc(v) < 0 ? 0.0 : std::trunc(v);
    default: return v;
    }
}

// A folded component as a 64-bit integer, clamped so a hostile literal cannot make the cast undefined.
long long ll(double v) {
    if (!(v > -9.0e18)) return v != v ? 0 : -9000000000000000000LL;
    if (v > 9.0e18) return 9000000000000000000LL;
    return static_cast<long long>(v);
}

bool binFold(const std::string& op, const GdConstValue& a, const GdConstValue& b, GdConstValue& out) {
    const GdKind ka = gdKind(a.type.base), kb = gdKind(b.type.base);
    out.type = binaryResultType(op, a.type, b.type);
    if (out.type.base == B::Error) return false;
    const usize na = a.v.size(), nb = b.v.size();
    if (op == "&&" || op == "||") {
        const bool r = op == "&&" ? (a.v[0] != 0 && b.v[0] != 0) : (a.v[0] != 0 || b.v[0] != 0);
        out.v = {r ? 1.0 : 0.0};
        return true;
    }
    if (op == "==" || op == "!=") {
        bool eq = na == nb;
        for (usize i = 0; eq && i < na; ++i) eq = a.v[i] == b.v[i];
        out.v = {(op == "==") == eq ? 1.0 : 0.0};
        return true;
    }
    if (op == "<" || op == ">" || op == "<=" || op == ">=") {
        const double x = a.v[0], y = b.v[0];
        out.v = {(op == "<" ? x < y : op == ">" ? x > y : op == "<=" ? x <= y : x >= y) ? 1.0 : 0.0};
        return true;
    }
    if (ka == GdKind::Matrix || kb == GdKind::Matrix) {
        if (op == "*" && ka == GdKind::Matrix && kb == GdKind::Matrix) {
            const int n = gdDim(a.type.base);
            out.v.assign(static_cast<usize>(n * n), 0.0);
            for (int c = 0; c < n; ++c) for (int r = 0; r < n; ++r) {
                double s = 0;
                for (int k = 0; k < n; ++k) s += a.v[static_cast<usize>(k * n + r)] * b.v[static_cast<usize>(c * n + k)];
                out.v[static_cast<usize>(c * n + r)] = s;
            }
            return true;
        }
        if (op == "*" && ka == GdKind::Matrix && gdDim(b.type.base) == gdDim(a.type.base) && kb == GdKind::Float) {
            const int n = gdDim(a.type.base);
            out.v.assign(static_cast<usize>(n), 0.0);
            for (int r = 0; r < n; ++r) for (int k = 0; k < n; ++k) out.v[static_cast<usize>(r)] += a.v[static_cast<usize>(k * n + r)] * b.v[static_cast<usize>(k)];
            return true;
        }
        const bool compWise = (ka == GdKind::Matrix && kb == GdKind::Matrix) || (ka == GdKind::Matrix && nb == 1) || (kb == GdKind::Matrix && na == 1);
        if (!compWise) return false;
    }
    const usize n = std::max(na, nb);
    out.v.resize(n);
    for (usize i = 0; i < n; ++i) {
        const double x = a.v[na == 1 ? 0 : i], y = b.v[nb == 1 ? 0 : i];
        const bool isInt = isIntLike(gdKind(gdScalarOf(out.type.base)));
        double r = 0;
        if (op == "+") r = x + y;
        else if (op == "-") r = x - y;
        else if (op == "*") r = x * y;
        else if (op == "/") { if (isInt) { if (y == 0) return false; r = std::trunc(x / y); } else r = x / y; }
        else if (op == "%") { if (ll(y) == 0) return false; r = static_cast<double>(ll(x) % ll(y)); }
        else if (op == "<<") r = static_cast<double>(static_cast<long long>(static_cast<unsigned long long>(ll(x)) << (ll(y) & 63)));
        else if (op == ">>") r = static_cast<double>(ll(x) >> (ll(y) & 63));
        else if (op == "&") r = static_cast<double>(ll(x) & ll(y));
        else if (op == "|") r = static_cast<double>(ll(x) | ll(y));
        else if (op == "^") r = static_cast<double>(ll(x) ^ ll(y));
        else return false;
        out.v[i] = r;
    }
    return true;
}
}  // namespace

bool foldConst(const GdExpr& e, const GdShaderModule& m, GdConstValue& out) {
    out = GdConstValue{};
    switch (e.kind) {
    case GdExprKind::BoolLit: case GdExprKind::IntLit: case GdExprKind::UIntLit: case GdExprKind::FloatLit:
        out.type = e.type; out.v = {e.num}; out.valid = true; return true;
    case GdExprKind::Var:
        if (e.cv && e.cv->valid) { out = *e.cv; return true; }
        return false;
    case GdExprKind::Unary: {
        GdConstValue a;
        if (!foldConst(*e.args[0], m, a)) return false;
        out.type = a.type;
        out.v = a.v;
        if (e.op == "-") for (double& x : out.v) x = -x;
        else if (e.op == "!") out.v[0] = a.v[0] != 0 ? 0.0 : 1.0;
        else if (e.op == "~") for (double& x : out.v) x = static_cast<double>(~ll(x));
        out.valid = true;
        return true;
    }
    case GdExprKind::Binary: {
        GdConstValue a, b;
        if (!foldConst(*e.args[0], m, a) || !foldConst(*e.args[1], m, b)) return false;
        if (!binFold(e.op, a, b, out)) return false;
        out.valid = true;
        return true;
    }
    case GdExprKind::Ternary: {
        GdConstValue c, a, b;
        if (!foldConst(*e.args[0], m, c) || !foldConst(*e.args[1], m, a) || !foldConst(*e.args[2], m, b)) return false;
        out = c.v[0] != 0 ? a : b;
        out.valid = true;
        return true;
    }
    case GdExprKind::Swizzle: {
        GdConstValue a;
        if (!foldConst(*e.args[0], m, a)) return false;
        out.type = e.type;
        for (char c : e.swizzle) out.v.push_back(a.v[static_cast<usize>(c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : 3)]);
        out.valid = true;
        return true;
    }
    case GdExprKind::Construct: {
        if (e.type.base == B::Struct || e.type.isArray()) return false;
        const GdKind tk = gdKind(e.type.base);
        std::vector<double> comps;
        bool anyMatrix = false;
        int matDim = 0;
        for (const GdExprP& a : e.args) {
            GdConstValue av;
            if (!foldConst(*a, m, av)) return false;
            if (gdKind(av.type.base) == GdKind::Matrix) { anyMatrix = true; matDim = gdDim(av.type.base); }
            for (double x : av.v) comps.push_back(x);
        }
        if (tk == GdKind::Matrix) {
            const int n = gdDim(e.type.base);
            out.type = e.type;
            out.v.assign(static_cast<usize>(n * n), 0.0);
            if (anyMatrix) {
                for (int c = 0; c < n; ++c) for (int r = 0; r < n; ++r) {
                    double v = c == r ? 1.0 : 0.0;
                    if (c < matDim && r < matDim) v = comps[static_cast<usize>(c * matDim + r)];
                    out.v[static_cast<usize>(c * n + r)] = v;
                }
            } else if (comps.size() == 1) {
                for (int i = 0; i < n; ++i) out.v[static_cast<usize>(i * n + i)] = comps[0];
            } else if (comps.size() >= static_cast<usize>(n * n)) {
                for (int i = 0; i < n * n; ++i) out.v[static_cast<usize>(i)] = comps[static_cast<usize>(i)];
            } else return false;
            out.valid = true;
            return true;
        }
        const int n = gdDim(e.type.base);
        if (n < 1 || comps.empty()) return false;
        out.type = e.type;
        out.v.resize(static_cast<usize>(n));
        for (int i = 0; i < n; ++i) out.v[static_cast<usize>(i)] = convertComp(comps.size() == 1 ? comps[0] : comps[static_cast<usize>(i)], tk);
        out.valid = true;
        return true;
    }
    default:
        return false;
    }
}

}  // namespace detail

std::vector<GdBuiltinSig> gdBuiltinOverloads(std::string_view name) {
    std::vector<GdBuiltinSig> out;
    for (int id : detail::builtinOverloadIds(name)) {
        const detail::BuiltinOverload& o = detail::builtinOverloads()[static_cast<usize>(id)];
        GdBuiltinSig s;
        s.name = o.name.c_str();
        s.ret = o.ret;
        s.params = o.params;
        s.outParam = o.outParam;
        s.hlsl = o.hlsl.c_str();
        out.push_back(std::move(s));
    }
    return out;
}

bool gdIsBuiltinFunction(std::string_view name) { return !detail::builtinOverloadIds(name).empty(); }

}  // namespace aver::fmt::godot
