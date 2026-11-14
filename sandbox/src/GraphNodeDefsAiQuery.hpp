#pragma once
// Palette entries for the AI query nodes: environment queries, smart objects and the entity brain.
// NOT standalone: include it from GraphNodeDefsFamilies.cpp inside namespace aver::editor::detail, after
// pin()/attr() are defined and before buildCatalog(), then call appendAiQueryNodeDefs(t) just before
// buildCatalog returns.
//
// MIRRORS scripting/csharp/Aver.Graph/GraphAiQueryNodeTable.cs pin for pin
// (docs/plans/wave3/EQS_SMARTOBJECTS_STATETREE.md section 9). Names of queries, assets, tag lists,
// events and states are attributes, never pins. Exec nodes take part in the PUSH compiler only; the
// pure reads work under either.

inline void appendAiQueryNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"AN_EqsRun", "Run EQS Query", "AI", {
        pin("exec", "exec", false), pin("querier", "int", false), pin("target", "int", false), pin("px", "float", false), pin("py", "float", false), pin("pz", "float", false), pin("hasPoint", "bool", false),
        pin("then", "exec", true), pin("found", "bool", true), pin("count", "int", true), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("entity", "int", true), pin("score", "float", true)},
        {attr("query", "Query (.oceqs)"), attr("usertags", "User tags (comma list)")}});
    t.push_back({"AN_EqsStart", "Start EQS Query", "AI", {
        pin("exec", "exec", false), pin("querier", "int", false), pin("target", "int", false), pin("px", "float", false), pin("py", "float", false), pin("pz", "float", false), pin("hasPoint", "bool", false), pin("priority", "int", false),
        pin("then", "exec", true), pin("handle", "int", true)},
        {attr("query", "Query (.oceqs)"), attr("usertags", "User tags (comma list)")}});
    t.push_back({"AN_EqsStatus", "EQS Query Status", "AI", {
        pin("handle", "int", false), pin("status", "int", true), pin("count", "int", true)}});
    t.push_back({"AN_EqsGetItem", "Get EQS Item", "AI", {
        pin("handle", "int", false), pin("index", "int", false),
        pin("success", "bool", true), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("entity", "int", true), pin("score", "float", true), pin("obj", "int", true), pin("slot", "int", true)}});
    t.push_back({"AN_EqsCancel", "Cancel EQS Query", "AI", {
        pin("exec", "exec", false), pin("handle", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_EqsRelease", "Release EQS Query", "AI", {
        pin("exec", "exec", false), pin("handle", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SoAttach", "Attach Smart Object", "AI", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("asset", "Smart object (.ocso)")}});
    t.push_back({"AN_SoFind", "Find Smart Object Slot", "AI", {
        pin("user", "int", false), pin("x", "float", false), pin("y", "float", false), pin("z", "float", false), pin("radius", "float", false, "1500"), pin("matchAll", "bool", false),
        pin("found", "bool", true), pin("obj", "int", true), pin("slot", "int", true), pin("ex", "float", true), pin("ey", "float", true), pin("ez", "float", true), pin("count", "int", true)},
        {attr("slottags", "Slot tags (comma list)"), attr("usertags", "User tags (comma list)")}});
    t.push_back({"AN_SoClaim", "Claim Smart Object Slot", "AI", {
        pin("exec", "exec", false), pin("user", "int", false), pin("obj", "int", false), pin("slot", "int", false), pin("ttl", "float", false, "-1"),
        pin("then", "exec", true), pin("claim", "int", true), pin("refusal", "int", true)},
        {attr("usertags", "User tags (comma list)")}});
    t.push_back({"AN_SoUse", "Use Smart Object Slot", "AI", {
        pin("exec", "exec", false), pin("claim", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SoRelease", "Release Smart Object Slot", "AI", {
        pin("exec", "exec", false), pin("claim", "int", false), pin("reason", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_SoGetClaim", "Get Smart Object Claim", "AI", {
        pin("user", "int", false), pin("index", "int", false, "0"),
        pin("has", "bool", true), pin("claim", "int", true), pin("obj", "int", true), pin("slot", "int", true), pin("state", "int", true), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("yaw", "float", true), pin("ex", "float", true), pin("ey", "float", true), pin("ez", "float", true)}});
    t.push_back({"AN_SoSlotInfo", "Smart Object Slot Info", "AI", {
        pin("entity", "int", false), pin("slot", "int", false),
        pin("success", "bool", true), pin("state", "int", true), pin("user", "int", true), pin("x", "float", true), pin("y", "float", true), pin("z", "float", true), pin("yaw", "float", true)}});
    t.push_back({"AN_SoSlotCount", "Smart Object Slot Count", "AI", {
        pin("entity", "int", false), pin("count", "int", true)}});
    t.push_back({"AN_SoSetSlotEnabled", "Set Smart Object Slot Enabled", "AI", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("slot", "int", false), pin("enabled", "bool", false, "true"), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_BrainSet", "Set Brain", "AI", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("asset", "Brain (.ocbt or .ocst)")}});
    t.push_back({"AN_BrainSendEvent", "Send Brain Event", "AI", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("payloadI", "int", false), pin("payloadF", "float", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("event", "Event")}});
    t.push_back({"AN_BrainIsInState", "Brain Is In State", "AI", {
        pin("entity", "int", false), pin("inState", "bool", true)},
        {attr("state", "State path")}});
    t.push_back({"AN_BrainStatus", "Brain Status", "AI", {
        pin("entity", "int", false), pin("status", "int", true), pin("kind", "int", true)}});
    t.push_back({"AN_BrainRestart", "Restart Brain", "AI", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_BrainStop", "Stop Brain", "AI", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
}
