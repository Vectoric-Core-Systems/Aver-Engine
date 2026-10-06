#pragma once
// Palette entries for the AN_ crowd, hearing, cover and squad nodes. NOT standalone: include it
// from GraphNodeDefs.hpp inside namespace aver::editor::detail, after pin()/attr() are defined and
// before buildCatalog(), then call appendSynapseAiNodeDefs(t) just before buildCatalog returns.
//
// Each node's body is Aver.Synapse's SynapseAiGraph.<Name>ForGraph, reached through a forwarder of
// the same name in Aver.Framework's GraphInterop (docs/AI_CROWDS_HEARING_COVER.md lists them).
// Exec nodes take part in the PUSH compiler only, like CharacterMove; the pure ones are reads and
// work under either compiler. "success" is false only when the call could not be made at all.

inline void appendSynapseAiNodeDefs(std::vector<GraphNodeDesc>& t) {
    // -- Crowd ---------------------------------------------------------------------------------------
    t.push_back({"AN_CrowdSetAgent", "Crowd Set Agent", "AI", {
        pin("exec", "exec", false), pin("entity", "int", false),
        pin("radiusCm", "float", false, "34"), pin("maxSpeedCm", "float", false, "350"),
        pin("maxAccelCm", "float", false, "1200"), pin("priority", "float", false, "0"),
        pin("then", "exec", true), pin("success", "bool", true)}});

    // mode: 0 follow path agent, 1 seek, 2 arrive, 3 flee, 4 wander, 5 hold.
    t.push_back({"AN_CrowdSetMode", "Crowd Set Mode", "AI", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("mode", "int", false),
        pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("then", "exec", true), pin("success", "bool", true)}});

    // backend: 0 CPU (deterministic, hundreds), 1 GPU (thousands). maxAgents < 0 keeps the cap.
    t.push_back({"AN_CrowdSetBackend", "Crowd Set Backend", "AI", {
        pin("exec", "exec", false), pin("backend", "int", false, "0"), pin("maxAgents", "int", false, "-1"),
        pin("then", "exec", true), pin("success", "bool", true)}});

    t.push_back({"AN_GetCrowdVelocity", "Get Crowd Velocity", "AI", {
        pin("entity", "int", false),
        pin("vx", "float", true), pin("vy", "float", true), pin("speed", "float", true),
        pin("success", "bool", true)}});

    // The CharacterMove feed: forward/right/yawDelta from the crowd velocity (GraphInterop.CrowdSteerForGraph).
    t.push_back({"AN_CrowdSteer", "Crowd Steer", "AI", {
        pin("entity", "int", false), pin("dt", "float", false),
        pin("turnRate", "float", false, "360"), pin("maxSpeedCm", "float", false, "350"),
        pin("forward", "float", true), pin("right", "float", true), pin("yawDelta", "float", true),
        pin("success", "bool", true)}});

    // -- Hearing -------------------------------------------------------------------------------------
    t.push_back({"AN_EmitNoise", "Emit Noise", "AI", {
        pin("exec", "exec", false),
        pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("loudnessCm", "float", false, "1000"), pin("tag", "int", false, "0"), pin("source", "int", false, "0"),
        pin("then", "exec", true), pin("success", "bool", true)}});

    t.push_back({"AN_SetHearing", "Set Hearing", "AI", {
        pin("exec", "exec", false), pin("entity", "int", false),
        pin("sensitivity", "float", false, "1"), pin("maxRangeCm", "float", false, "5000"),
        pin("memorySec", "float", false, "8"),
        pin("then", "exec", true), pin("success", "bool", true)}});

    // "heard" false is ordinary (nothing remembered); "success" false only when the entity has no listener.
    t.push_back({"AN_GetHeard", "Get Heard", "AI", {
        pin("entity", "int", false),
        pin("heard", "bool", true),
        pin("x", "float", true), pin("y", "float", true), pin("z", "float", true),
        pin("level", "float", true), pin("tag", "int", true), pin("confidence", "float", true),
        pin("timeSince", "float", true), pin("success", "bool", true)}});

    // -- Cover and squads ----------------------------------------------------------------------------
    // Finds AND reserves; "found" false means nothing protects from that threat.
    t.push_back({"AN_FindCover", "Find Cover", "AI", {
        pin("exec", "exec", false), pin("entity", "int", false),
        pin("threatX", "float", false), pin("threatY", "float", false), pin("threatZ", "float", false),
        pin("maxSeekCm", "float", false, "2500"), pin("minThreatDistCm", "float", false, "300"),
        pin("then", "exec", true), pin("found", "bool", true),
        pin("x", "float", true), pin("y", "float", true), pin("coverId", "int", true),
        pin("success", "bool", true)}});

    t.push_back({"AN_ReleaseCover", "Release Cover", "AI", {
        pin("exec", "exec", false), pin("entity", "int", false),
        pin("then", "exec", true), pin("success", "bool", true)}});

    t.push_back({"AN_IsCovered", "Is Covered", "AI", {
        pin("entity", "int", false),
        pin("threatX", "float", false), pin("threatY", "float", false), pin("threatZ", "float", false),
        pin("covered", "bool", true)}});

    t.push_back({"AN_SquadJoin", "Squad Join", "AI", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("squadId", "int", false, "0"),
        pin("spacingCm", "float", false, "200"),
        pin("then", "exec", true), pin("success", "bool", true)}});

    t.push_back({"AN_SquadSetTarget", "Squad Set Target", "AI", {
        pin("exec", "exec", false), pin("squadId", "int", false, "0"),
        pin("x", "float", false), pin("y", "float", false), pin("z", "float", false),
        pin("then", "exec", true), pin("success", "bool", true)}});

    // role: 0 none, 1 anchor, 2 flank left, 3 flank right, 4 support. "success" false until the squad has a target.
    t.push_back({"AN_GetSquadSlot", "Get Squad Slot", "AI", {
        pin("entity", "int", false),
        pin("role", "int", true),
        pin("x", "float", true), pin("y", "float", true), pin("z", "float", true),
        pin("success", "bool", true)}});
}
