#pragma once
// Palette entries for the light, camera and particle-emitter nodes. NOT standalone: GraphNodeFamilies.gen.inc (generated
// by cmake/GraphNodeFamilies.cmake) includes it from GraphNodeDefs.hpp inside namespace aver::editor::detail, after
// pin()/attr() are defined, and calls appendRenderPropsNodeDefs(t).
//
// MIRRORS scripting/csharp/Aver.Graph/GraphRenderPropsNodeTable.cs pin for pin
// (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md section 2.2). The writes go through the component field ABI, so they
// work on any entity that carries the component. Effect paths are attributes, never pins; entities are ints.

inline void appendRenderPropsNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"AN_SetLightIntensity", "Set Light Intensity", "Light", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("lux", "float", false),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_GetLightIntensity", "Get Light Intensity", "Light", {
        pin("entity", "int", false), pin("lux", "float", true), pin("found", "bool", true)}});
    t.push_back({"AN_SetLightColor", "Set Light Color", "Light", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("r", "float", false), pin("g", "float", false), pin("b", "float", false),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SetLightRange", "Set Light Range", "Light", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("rangeCm", "float", false),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SetLightCone", "Set Light Cone", "Light", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("innerDeg", "float", false, "20"), pin("outerDeg", "float", false, "35"),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SetLightShadows", "Set Light Shadows", "Light", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("enabled", "bool", false, "true"),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SetCameraFov", "Set Camera Fov", "Camera", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("fovDeg", "float", false, "60"),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_GetCameraFov", "Get Camera Fov", "Camera", {
        pin("entity", "int", false), pin("fovDeg", "float", true), pin("found", "bool", true)}});
    t.push_back({"AN_SetCameraClip", "Set Camera Clip", "Camera", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("nearCm", "float", false, "5"), pin("farCm", "float", false, "500000"),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SetCameraPriority", "Set Camera Priority", "Camera", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("priority", "int", false),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SetEmitterEnabled", "Set Emitter Enabled", "Particles", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("enabled", "bool", false, "true"),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SetEmitterEffect", "Set Emitter Effect", "Particles", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("effect", "Particle effect (.ocparticle)")}});
    t.push_back({"AN_RestartEmitter", "Restart Emitter", "Particles", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SpawnEmitterAt", "Spawn Emitter At", "Particles", {
        pin("exec", "exec", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("lifetime", "float", false, "3"), pin("then", "exec", true), pin("entity", "int", true)},
        {attr("effect", "Particle effect (.ocparticle)")}});
}
