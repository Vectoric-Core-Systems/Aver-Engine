#pragma once
// Palette entries for the sun, fog, sky-light and post-layer nodes. NOT standalone: included from
// GraphNodeDefs.hpp (via GraphNodeFamilies.gen.inc) inside namespace aver::editor::detail, after pin()/attr() are
// defined and before buildCatalog().
//
// MIRRORS scripting/csharp/Aver.Graph/GraphWorldFxNodeTable.cs pin for pin
// (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md 2.4). All are push-compiler (exec) nodes returning success. An
// empty layer means the layer named "script"; an empty environment key releases every override. In the editor they
// apply during Play only.

inline void appendWorldFxNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"SetSunDirection", "Set Sun Direction", "World", {
        pin("exec", "exec", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetSunIntensity", "Set Sun Intensity", "World", {
        pin("exec", "exec", false), pin("lux", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetSunColor", "Set Sun Color", "World", {
        pin("exec", "exec", false), pin("r", "float", false), pin("g", "float", false), pin("b", "float", false),
        pin("temperatureK", "float", false, "0"), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetFogDensity", "Set Fog Density", "World", {
        pin("exec", "exec", false), pin("density", "float", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetFogColor", "Set Fog Color", "World", {
        pin("exec", "exec", false), pin("r", "float", false), pin("g", "float", false), pin("b", "float", false),
        pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetFogShape", "Set Fog Shape", "World", {
        pin("exec", "exec", false), pin("falloff", "float", false), pin("height", "float", false), pin("start", "float", false),
        pin("maxOpacity", "float", false, "1"), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"SetSkyLight", "Set Sky Light", "World", {
        pin("exec", "exec", false), pin("multiplier", "float", false, "1"), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"ClearEnvironment", "Clear Environment Overrides", "World", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("key", "Override (sun.dir, sun.lux, sun.color, sun.temp, fog.density, fog.color, fog.falloff, fog.height, fog.start, fog.maxopacity, sky.light; empty = all)")}});
    t.push_back({"SetPostValue", "Set Post Value", "World", {
        pin("exec", "exec", false), pin("v0", "float", false), pin("v1", "float", false), pin("v2", "float", false),
        pin("then", "exec", true), pin("success", "bool", true)},
        {attr("layer", "Layer (empty = script)"), attr("key", "Post key (saturation, bloom, vignette, lift, ...)")}});
    t.push_back({"SetPostLayerWeight", "Set Post Layer Weight", "World", {
        pin("exec", "exec", false), pin("weight", "float", false, "1"), pin("blendSeconds", "float", false, "0"),
        pin("then", "exec", true), pin("success", "bool", true)},
        {attr("layer", "Layer (empty = script)")}});
    t.push_back({"ClearPostLayer", "Clear Post Layer", "World", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("layer", "Layer (empty = script)")}});
    t.push_back({"SetPostVolumeWeight", "Set Post Volume Weight", "World", {
        pin("exec", "exec", false), pin("weight", "float", false), pin("blendSeconds", "float", false),
        pin("then", "exec", true), pin("success", "bool", true)},
        {attr("volume", "Level post volume name")}});
}
