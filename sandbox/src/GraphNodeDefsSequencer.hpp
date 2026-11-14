#pragma once
// Palette entries for the sequencer nodes. NOT standalone: include it from GraphNodeDefsFamilies.cpp inside
// namespace aver::editor::detail, after pin()/attr() are defined and before buildCatalog(), then call
// appendSequencerNodeDefs(t) just before buildCatalog returns.
//
// MIRRORS scripting/csharp/Aver.Graph/GraphSequenceNodeTable.cs pin for pin
// (docs/plans/wave5/SEQUENCER_TRACKS.md section 10). Sequences are named by an attribute, never a pin.
// Sequence lifecycle and event-track events arrive as AN_OnEvent entries, so they need no node here.

inline void appendSequencerNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"PlaySequence", "Play Sequence", "Sequencer", {
        pin("exec", "exec", false), pin("start", "float", false, "-1"), pin("rate", "float", false, "0"), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("sequence", "Sequence")}});
    t.push_back({"PauseSequence", "Pause Sequence", "Sequencer", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("sequence", "Sequence")}});
    t.push_back({"StopSequence", "Stop Sequence", "Sequencer", {
        pin("exec", "exec", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("sequence", "Sequence (empty = all)")}});
    t.push_back({"SetSequenceTime", "Set Sequence Time", "Sequencer", {
        pin("exec", "exec", false), pin("seconds", "float", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("sequence", "Sequence")}});
    t.push_back({"SetSequenceRate", "Set Sequence Rate", "Sequencer", {
        pin("exec", "exec", false), pin("rate", "float", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("sequence", "Sequence")}});
    t.push_back({"BindSequenceActor", "Bind Sequence Actor", "Sequencer", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("sequence", "Sequence"), attr("binding", "Binding name")}});
    t.push_back({"IsSequencePlaying", "Is Sequence Playing", "Sequencer", {
        pin("result", "bool", true)},
        {attr("sequence", "Sequence (empty = any)")}});
    t.push_back({"GetSequenceState", "Get Sequence State", "Sequencer", {
        pin("found", "bool", true), pin("time", "float", true), pin("length", "float", true), pin("rate", "float", true)},
        {attr("sequence", "Sequence")}});
}
