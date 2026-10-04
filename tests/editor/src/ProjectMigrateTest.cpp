// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// migrateProject -- the version migration behind the editor's "Upgrade Project" prompt.
//
// WHAT THIS EXISTS TO CATCH, stated plainly because it is what actually shipped: both buttons on
// that prompt used to end in a bare open(). No step of the migration chain ran, and neither wrote a
// version stamp -- so a 0.2 project was "upgraded", still recorded 0.2, and was asked to upgrade
// again the moment it reopened. Forever. modules/upgrade, a documented and tested step chain, had no
// caller outside its own unit test.
//
// THE STAMP IS THE ASSERTION. A chain that runs and does not stamp is indistinguishable, from the
// outside, from a chain that never ran -- and that is precisely the bug. So every check below reads
// the manifest back off disk rather than trusting a true return.
//
// Compiles sandbox/src/ProjectScaffold.cpp directly, exactly as ProjectCopyTest beside it does and
// for the reason its own comment gives: Sandbox is add_executable-only, and a private member of an
// ImGui screen cannot be reached from a test. migrateProject was lifted out of ProjectBrowser for
// that reason -- it is the branch that most needs proving, so it had to be reachable.
#include "ProjectScaffold.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Version.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/platform/FileSystem.hpp"

#include <filesystem>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("   PASS  {}", what); }
    else { AVER_ERROR("   FAIL  {}", what); ++g_failures; }
}

static std::string scratchDir() {
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) base = std::filesystem::current_path();
    return (base / "AverProjectMigrateTest").string();
}

// A minimal project on disk: a manifest recording `version`, and the Content/Scripts folders a
// Context names. Enough for the chain to run against; the steps themselves are what decide whether
// anything in there matters.
static std::string makeProject(const std::string& name, const std::string& version) {
    const std::filesystem::path root = std::filesystem::path(scratchDir()) / name;
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "Content" / "Scripts", ec);

    const std::string manifest = (root / (name + ".ocproject")).string();
    std::string text = "OCPROJECT 1\nNAME " + name + "\n";
    if (!version.empty()) text += "CREATEDWITH " + version + "\n";
    writeFileText(manifest, text);
    return manifest;
}

static std::string stampOf(const std::string& manifest) {
    fmt::ProjectDesc d;
    std::string why;
    if (!fmt::loadOcproject(manifest, d, &why)) return "<unreadable>";
    return d.createdWith;
}

// ============================================================ an older project is carried forward =
static void testOlderProjectIsMigratedAndStamped() {
    AVER_INFO("=== an older project runs the chain AND records that it did ===");
    const std::string manifest = makeProject("Older", "0.2.0");
    check(stampOf(manifest) == "0.2.0", "the fixture starts at 0.2.0");

    std::string err;
    const bool ok = editor::migrateProject(manifest, &err);
    check(ok, "migrateProject succeeds (err='" + err + "')");
    check(stampOf(manifest) == std::string(kEngineVersion),
          "THE MANIFEST NOW RECORDS THIS ENGINE -- the half that was missing entirely");
}

// A second run must be a no-op that still succeeds: the prompt only stops asking if "already
// current" is success rather than an error.
static void testMigratingTwiceIsHarmless() {
    AVER_INFO("=== migrating an already-current project is success, not an error ===");
    const std::string manifest = makeProject("Twice", "0.2.0");
    std::string err;
    check(editor::migrateProject(manifest, &err), "first migration succeeds");
    const std::string after = stampOf(manifest);
    check(editor::migrateProject(manifest, &err), "second migration also succeeds (err='" + err + "')");
    check(stampOf(manifest) == after, "and leaves the stamp exactly where it was");
}

// THE FAILURE PATH MUST NOT STAMP. A project recorded as current when its migration failed is a
// project nobody is ever asked to fix again -- strictly worse than one that keeps asking.
static void testNewerProjectIsRefusedAndNotStamped() {
    AVER_INFO("=== a project from a NEWER engine is refused, and its stamp is left alone ===");
    const std::string manifest = makeProject("FromTheFuture", "99.0.0");
    std::string err;
    check(!editor::migrateProject(manifest, &err), "migrateProject refuses it");
    check(err.find("newer") != std::string::npos || err.find("downgrade") != std::string::npos,
          "and says why: '" + err + "'");
    check(stampOf(manifest) == "99.0.0",
          "THE STAMP IS UNTOUCHED -- a failed migration must not record success");
}

// A manifest with no version at all is adoptVersionStamp's case on open, not this one: there is
// nothing to plan a chain FROM, and guessing a series could run steps a project never needed.
static void testNoVersionIsRefusedWithAReason() {
    AVER_INFO("=== a project recording no engine version is refused, by name ===");
    const std::string manifest = makeProject("Unstamped", "");
    check(stampOf(manifest).empty(), "the fixture records no version");
    std::string err;
    check(!editor::migrateProject(manifest, &err), "migrateProject refuses it");
    check(err.find("no engine version") != std::string::npos,
          "and the reason names the missing version rather than a parse failure: '" + err + "'");
}

static void testMissingManifestFailsCleanly() {
    AVER_INFO("=== a manifest that is not there fails without crashing ===");
    std::string err;
    const std::string nowhere = (std::filesystem::path(scratchDir()) / "nope.ocproject").string();
    check(!editor::migrateProject(nowhere, &err), "refused");
    check(!err.empty(), "with a non-empty reason");
}

int main() {
    AVER_INFO("======== ProjectMigrateTest ========");
    std::error_code ec;
    std::filesystem::remove_all(scratchDir(), ec);

    testOlderProjectIsMigratedAndStamped();
    testMigratingTwiceIsHarmless();
    testNewerProjectIsRefusedAndNotStamped();
    testNoVersionIsRefusedWithAReason();
    testMissingManifestFailsCleanly();

    AVER_INFO("======== {} failure(s) ========", g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
