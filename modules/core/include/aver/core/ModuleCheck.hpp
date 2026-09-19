#pragma once
// Pinning the module dependency DAG, so an invalid combination fails to COMPILE with a sentence
// somebody can act on -- instead of as an unresolved external forty includes deep, or as a silent
// empty world.
//
// CORE PROVIDES THE MECHANISM AND NOT THE FACTS, and that distinction is the whole design.
// AVER_MODULE_* are PUBLIC compile definitions on the module targets themselves, so they reach a
// translation unit ONLY through what that TU actually links -- Runtime/CMakeLists.txt
// records being bitten by exactly this, where every `#if AVER_MODULE_*` in GameApp.cpp silently
// evaluated false because the link line was missing. Core is built first and links none of the
// optional modules, so a list of DAG facts written HERE would see every macro undefined and check
// nothing at all, while looking thorough.
//
// So the macro is meant to be invoked by each dependent module from its OWN public header. It then
// fires only in a translation unit that has already linked that module -- which is precisely the
// context where the question "and did its dependency come too?" is meaningful.
//
// NOBODY INVOKES IT YET, AND THIS PARAGRAPH USED TO CLAIM OTHERWISE. No header in the tree includes
// this one, and AVER_REQUIRE_MODULE appears nowhere but its own definition below, so the DAG guard
// as it stands catches nothing -- which is the "looks thorough, checks nothing" shape the paragraph
// above warns about, turned on the header that warns about it. Said out loud because a guard
// believed to be running is worse than one known to be absent: the belief is what stops anyone
// adding the second mechanism that would actually catch the bug.
//
// KEPT RATHER THAN DELETED because the mechanism is correct and docs/ABI_VERIFICATION_PLAN.md's
// section 3 still reasons from it. Adopting it is one AVER_REQUIRE_MODULE line in the public header
// of every module that declares DEPS (render.voxi -> rhi, anim.scene -> scene, framework -> scene,
// render.softbody -> scene and physics, and so on), and it is a separate change because the only
// proof it is right is building the combinations scripts/module-matrix.ps1 builds -- a wrong
// invocation fails a configuration nobody compiles daily, which is the class of breakage this
// header is supposed to prevent rather than cause.
//
// WHAT THIS DOES NOT CATCH, stated plainly because a guard that overstates itself is worse than
// none. The dominant defect class in this codebase is "a member is declared inside
// `#if AVER_MODULE_X` but used from a site guarded differently, or not guarded at all". That is a
// preprocessor data-flow property of one specific configuration, and nothing short of preprocessing
// that configuration can decide it. This header catches a module built without its dependency; the
// CMake validator in cmake/AvModule.cmake catches a dependency named but not defined; and
// scripts/module-matrix.ps1 -- which actually builds the combinations -- is what catches the rest.
// The three are complementary and none of them is sufficient.

// Fails the build when `dependent` is compiled in without `dependency`.
//
// Both arguments are the MACROS, not their names; the two string literals are what the reader sees.
// Written as an #if rather than a static_assert on purpose: it must work in a header included by a
// C translation unit as well as a C++ one, and it must fire during preprocessing, before any
// declaration that the missing module would have supplied is even parsed.
#define AVER_REQUIRE_MODULE(dependentMacro, dependencyMacro, dependentName, dependencyName)        \
    AVER_REQUIRE_MODULE_IMPL(dependentMacro, dependencyMacro, dependentName, dependencyName)

#define AVER_REQUIRE_MODULE_IMPL(dependentMacro, dependencyMacro, dependentName, dependencyName)   \
    static_assert(!(dependentMacro) || (dependencyMacro),                                          \
                  dependentName " is compiled in without " dependencyName ", which it stands on. " \
                  "The root CMakeLists.txt is meant to force it off in that case -- see the "       \
                  "force-off blocks beside the option() lines. Building it anyway produces "        \
                  "unresolved externals far from this line rather than an answer.")

// True when a module macro is both defined and on. Spelled out because `#if AVER_MODULE_X` treats
// an UNDEFINED macro as 0 silently, which is the same answer as "explicitly off" -- and those two
// are very different situations when the cause is a missing link line rather than a switch.
//
// Uninvoked for the same reason AVER_REQUIRE_MODULE is, and it goes wherever that decision goes:
// nothing includes this header, so the distinction it draws is drawn nowhere in the tree.
#define AVER_MODULE_ON(m) ((m) + 0)
