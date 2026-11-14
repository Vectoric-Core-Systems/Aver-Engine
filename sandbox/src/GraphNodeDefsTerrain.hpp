#pragma once
// Palette entries for the landscape and water nodes. NOT standalone: include it from GraphNodeDefsFamilies.cpp inside
// namespace aver::editor::detail, after pin()/attr() are defined and before buildCatalog(), then call
// appendTerrainNodeDefs(t) just before buildCatalog returns.
//
// MIRRORS scripting/csharp/Aver.Graph/GraphTerrainNodeTable.cs pin for pin
// (docs/plans/wave3/LANDSCAPE_LAYERS_RIVERS.md section 3). All are pure reads, so they work under either
// compiler. A layerId of 0 or more beats the layer attribute.

inline void appendTerrainNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"AN_LandscapeSample", "Sample Landscape", "Landscape", {
        pin("x", "float", false), pin("y", "float", false),
        pin("found", "bool", true), pin("z", "float", true), pin("nx", "float", true), pin("ny", "float", true), pin("nz", "float", true), pin("layer", "int", true)}});
    t.push_back({"AN_LandscapeLayerId", "Landscape Layer Id", "Landscape", {
        pin("layerId", "int", true)},
        {attr("layer", "Layer")}});
    t.push_back({"AN_LandscapeLayerWeight", "Landscape Layer Weight", "Landscape", {
        pin("x", "float", false), pin("y", "float", false), pin("layerId", "int", false, "-1"), pin("weight", "float", true)},
        {attr("layer", "Layer")}});
    t.push_back({"AN_LandscapeSurface", "Landscape Surface", "Landscape", {
        pin("x", "float", false), pin("y", "float", false),
        pin("surfaceId", "int", true), pin("friction", "float", true), pin("restitution", "float", true), pin("layer", "int", true)}});
    t.push_back({"AN_SurfaceId", "Surface Id", "Landscape", {
        pin("surfaceId", "int", true)},
        {attr("surface", "Surface")}});
    t.push_back({"AN_WaterSample", "Sample Water", "Landscape", {
        pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("over", "bool", true), pin("submerged", "bool", true), pin("surfaceZ", "float", true), pin("depth", "float", true), pin("flowX", "float", true), pin("flowY", "float", true), pin("flowZ", "float", true), pin("kind", "int", true)}});
    t.push_back({"AN_EntityWater", "Entity Water", "Landscape", {
        pin("entity", "int", false), pin("heightCm", "float", false, "180"),
        pin("over", "bool", true), pin("submersion", "float", true), pin("flowX", "float", true), pin("flowY", "float", true), pin("flowZ", "float", true), pin("kind", "int", true)}});
}
