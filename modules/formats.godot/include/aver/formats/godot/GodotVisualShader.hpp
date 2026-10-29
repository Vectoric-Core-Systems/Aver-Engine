#pragma once
// Fabric G6 SH3: Godot VisualShader resources as Godot shading-language text. A .tres VisualShader holds a node graph per shader function (vertex / fragment /
// light, or sky, or fog); this re-implements the way Godot turns it into code (VisualShader::generate_shader_code) for the common node set, so the text can go
// through the same front-end as a hand-written .gdshader (gdShaderCompile, gdShaderForEngine, gdShaderForEnv). Written from the documented node classes and
// the engine's behaviour; no Godot code is copied. Pure text in, plain structs out.
// Contract: docs/plans/fabric/G6_SHADERS.md, section SH3.
#include "aver/formats/godot/GodotScene.hpp"

#include <string>
#include <vector>

namespace aver::fmt::godot {

struct GdVisualNodeNote {
    std::string function;   // "vertex", "fragment", "light", "sky", "fog", ...
    int id = 0;
    std::string type;       // the node class, e.g. VisualShaderNodeCurveTexture
    std::string reason;
};

struct GdVisualShaderResult {
    bool ok = false;
    std::string error;
    std::string shaderType;                 // "spatial", "sky", "fog", "particles", "canvas_item"
    std::string code;                       // Godot shading-language text
    int nodeCount = 0;                      // nodes converted
    std::vector<GdVisualNodeNote> unsupported;   // nodes (or whole functions) that could not be converted; their outputs read as the unconnected default
    std::vector<GdVisualNodeNote> approximated;  // nodes converted with a difference worth reporting
};

// `res` is a parsed .tres whose header type is VisualShader. Fails (ok = false) for a resource of another type, a canvas_item or particles shader (2D is out of scope;
// particles graphs end in VisualShaderNodeParticle* nodes that are not converted), or a graph with no connection to the output node.
GdVisualShaderResult gdVisualShaderToCode(const GodotResource& res);

}  // namespace aver::fmt::godot
