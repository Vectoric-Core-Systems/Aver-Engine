#pragma once
// Palette entries for the gameplay-ability nodes (tags, attributes, effects, abilities). NOT standalone:
// include it from GraphNodeDefsFamilies.cpp inside namespace aver::editor::detail, after pin()/attr() are
// defined and before buildCatalog(), then call appendGasNodeDefs(t) just before buildCatalog returns.
//
// MIRRORS scripting/csharp/Aver.Graph/GraphGasNodeTable.cs pin for pin (docs/plans/wave2/GAS.md
// section 3). The id pins and the name attributes are alternatives: a non-zero id wins. Exec nodes
// take part in the PUSH compiler only; the pure reads work under either.

inline void appendGasNodeDefs(std::vector<GraphNodeDesc>& t) {
    t.push_back({"AN_GasTagId", "Gameplay Tag Id", "Abilities", {
        pin("tagId", "int", true)},
        {attr("tag", "Tag")}});
    t.push_back({"AN_GasAttributeId", "Attribute Id", "Abilities", {
        pin("attrId", "int", true)},
        {attr("attribute", "Attribute")}});
    t.push_back({"AN_GasEffectId", "Effect Id", "Abilities", {
        pin("effectId", "int", true)},
        {attr("effect", "Effect")}});
    t.push_back({"AN_GasAbilityId", "Ability Id", "Abilities", {
        pin("abilityId", "int", true)},
        {attr("ability", "Ability")}});
    t.push_back({"AN_GasHasTag", "Has Gameplay Tag", "Abilities", {
        pin("entity", "int", false), pin("tagId", "int", false), pin("has", "bool", true)},
        {attr("tag", "Tag")}});
    t.push_back({"AN_GasHasTags", "Has Gameplay Tags", "Abilities", {
        pin("entity", "int", false), pin("all", "bool", false), pin("has", "bool", true)},
        {attr("tags", "Tags (comma list)")}});
    t.push_back({"AN_GasTagCount", "Gameplay Tag Count", "Abilities", {
        pin("entity", "int", false), pin("tagId", "int", false), pin("count", "int", true)},
        {attr("tag", "Tag")}});
    t.push_back({"AN_GasAddTag", "Add Gameplay Tag", "Abilities", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("tagId", "int", false), pin("count", "int", false, "1"), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("tag", "Tag")}});
    t.push_back({"AN_GasRemoveTag", "Remove Gameplay Tag", "Abilities", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("tagId", "int", false), pin("count", "int", false, "1"), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("tag", "Tag")}});
    t.push_back({"AN_GasGetAttribute", "Get Attribute", "Abilities", {
        pin("entity", "int", false), pin("attrId", "int", false), pin("value", "float", true), pin("base", "float", true), pin("success", "bool", true)},
        {attr("attribute", "Attribute")}});
    t.push_back({"AN_GasSetAttribute", "Set Attribute Base", "Abilities", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("attrId", "int", false), pin("value", "float", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("attribute", "Attribute")}});
    t.push_back({"AN_GasAddAttributeSet", "Add Attribute Set", "Abilities", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("set", "Attribute set")}});
    t.push_back({"AN_GasApplyEffect", "Apply Gameplay Effect", "Abilities", {
        pin("exec", "exec", false), pin("target", "int", false), pin("source", "int", false), pin("effectId", "int", false), pin("callerValue", "float", false), pin("then", "exec", true), pin("handle", "int", true), pin("applied", "bool", true)},
        {attr("effect", "Effect"), attr("callertag", "Caller tag")}});
    t.push_back({"AN_GasRemoveEffect", "Remove Gameplay Effect", "Abilities", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("handle", "int", false), pin("then", "exec", true), pin("success", "bool", true)}});
    t.push_back({"AN_GasRemoveEffectsByTag", "Remove Effects By Tag", "Abilities", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("tagId", "int", false), pin("then", "exec", true), pin("count", "int", true)},
        {attr("tag", "Tag")}});
    t.push_back({"AN_GasRemoveEffectsByDef", "Remove Effects By Definition", "Abilities", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("effectId", "int", false), pin("then", "exec", true), pin("count", "int", true)},
        {attr("effect", "Effect")}});
    t.push_back({"AN_GasEffectInfo", "Gameplay Effect Info", "Abilities", {
        pin("entity", "int", false), pin("effectId", "int", false), pin("active", "bool", true), pin("stacks", "int", true), pin("remaining", "float", true), pin("handle", "int", true)},
        {attr("effect", "Effect")}});
    t.push_back({"AN_GasGrantAbility", "Grant Ability", "Abilities", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("abilityId", "int", false), pin("level", "int", false, "1"), pin("then", "exec", true), pin("handle", "int", true)},
        {attr("ability", "Ability")}});
    t.push_back({"AN_GasRevokeAbility", "Revoke Ability", "Abilities", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("abilityId", "int", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("ability", "Ability")}});
    t.push_back({"AN_GasTryActivate", "Try Activate Ability", "Abilities", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("abilityId", "int", false), pin("target", "int", false), pin("then", "exec", true), pin("success", "bool", true), pin("reason", "int", true)},
        {attr("ability", "Ability")}});
    t.push_back({"AN_GasTryActivateByTag", "Try Activate By Tag", "Abilities", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("tagId", "int", false), pin("target", "int", false), pin("then", "exec", true), pin("success", "bool", true), pin("abilityId", "int", true), pin("reason", "int", true)},
        {attr("tag", "Tag")}});
    t.push_back({"AN_GasCanActivate", "Can Activate Ability", "Abilities", {
        pin("entity", "int", false), pin("abilityId", "int", false), pin("can", "bool", true), pin("reason", "int", true)},
        {attr("ability", "Ability")}});
    t.push_back({"AN_GasCommitAbility", "Commit Ability", "Abilities", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("abilityId", "int", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("ability", "Ability")}});
    t.push_back({"AN_GasEndAbility", "End Ability", "Abilities", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("abilityId", "int", false), pin("cancelled", "bool", false), pin("then", "exec", true), pin("success", "bool", true)},
        {attr("ability", "Ability")}});
    t.push_back({"AN_GasCancelByTag", "Cancel Abilities By Tag", "Abilities", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("tagId", "int", false), pin("then", "exec", true), pin("count", "int", true)},
        {attr("tag", "Tag")}});
    t.push_back({"AN_GasIsAbilityActive", "Is Ability Active", "Abilities", {
        pin("entity", "int", false), pin("abilityId", "int", false), pin("active", "bool", true)},
        {attr("ability", "Ability")}});
    t.push_back({"AN_GasCooldown", "Ability Cooldown", "Abilities", {
        pin("entity", "int", false), pin("abilityId", "int", false), pin("remaining", "float", true), pin("onCooldown", "bool", true)},
        {attr("ability", "Ability")}});
    t.push_back({"AN_GasSendEvent", "Send Gameplay Event", "Abilities", {
        pin("exec", "exec", false), pin("entity", "int", false), pin("instigator", "int", false), pin("tagId", "int", false), pin("magnitude", "float", false), pin("then", "exec", true), pin("triggered", "int", true)},
        {attr("tag", "Tag")}});
}
