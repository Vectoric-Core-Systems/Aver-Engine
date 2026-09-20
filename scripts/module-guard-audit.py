# Finds, by preprocessing arithmetic rather than by building, the defect scripts/module-matrix.ps1
# exists to catch.
#
# WHY THIS EXISTS BESIDE THE MATRIX AND NOT INSTEAD OF IT. The matrix is the only thing that can
# DECIDE whether a configuration compiles -- it runs the actual preprocessor, the actual compiler
# and the actual linker over twenty configurations, and that takes hours. This script runs in about
# two seconds and finds the same three shapes by reading the guard stack, so the loop is: run this,
# fix what it names, and spend the matrix's hours confirming rather than discovering. Every defect
# it reports here was a real matrix failure when it was first written; the reverse does not hold, so
# a clean run here is not a green matrix.
#
# THE THREE SHAPES, all of which are "a preprocessor data-flow property of ONE configuration":
#
#   includes  An #include of an optional module's public header from outside that module's guard.
#             With the module off its include directory is never added to the target, so this is a
#             C1083 that kills the whole translation unit before a line of its own is compiled --
#             which is how two whole matrix rows used to die inside GameRender.hpp.
#
#   namespace A line naming an optional module's namespace from a site the module's macro does not
#             reach. A POINTER OR REFERENCE to a type forward-declared outside every guard is
#             exempt, because that is legal and is the documented pattern at
#             Runtime/include/aver/game/GameLandscape.hpp:39.
#
#   abi       A call to a module's plain-C ABI (aver_scene_*, aver_fw_*, aver_phys_*, ...) from
#             outside that module's guard. Same absence as the namespace shape, invisible to it
#             because a C symbol carries no `::`.
#
#   members   A member DECLARED inside one #if and USED from a site guarded differently. This is the
#             one a grep cannot see and the one that actually bites: SandboxApp::materialPanel was
#             declared under AVER_MODULE_PBR and called under AVER_WITH_IMGUI alone, so `pbr-off`
#             failed on a call site two thousand lines from the declaration.
#
#             WHAT IT CANNOT SEE, stated because a clean run should not be read as more than it is:
#             a use written through a pointer or a reference -- `a->refreshPlayerStart()` inside a
#             local RAII struct -- is skipped, because the same spelling is overwhelmingly a member
#             of some OTHER object and flagging those buries the real ones. That exact line was a
#             real `scene-off` failure that this scan reported nothing for. The matrix caught it.
#
#             ONE HEADER AT A TIME, defaulting to SandboxApp.hpp, and that is not laziness: the
#             match is by NAME, so pointing it at a second header whose class happens to declare an
#             `unload()` or a `SurfaceLook` of its own reports every unrelated class's member of the
#             same name. SandboxApp is where this shape actually occurs, because it is one class
#             with four thousand lines of members and fifteen files of definitions.
#
# USAGE
#   python scripts/module-guard-audit.py                 # all three, over sandbox/ and Runtime/
#   python scripts/module-guard-audit.py --only members
#   python scripts/module-guard-audit.py --header sandbox/src/SandboxApp.hpp --roots sandbox/src
#
# Exits 1 when anything is reported, so CI can run it as a gate.

import io
import os
import re
import sys

# ---------------------------------------------------------------------------------------------
# What the module system actually promises.
# ---------------------------------------------------------------------------------------------

# namespace -> the macro that must be in scope for it to exist.
NS_MACRO = {
    "pbr":       "AVER_MODULE_PBR",
    "voxi":      "AVER_MODULE_VOXI",
    "physics":   "AVER_MODULE_PHYSICS",
    "landscape": "AVER_MODULE_LANDSCAPE",
    "deform":    "AVER_MODULE_DEFORM",
    "scripting": "AVER_MODULE_SCRIPTING",
    "scene":     "AVER_MODULE_SCENE",
    "framework": "AVER_MODULE_FRAMEWORK",
    "trifactor": "AVER_MODULE_TRIFACTOR",
    "occlusion": "AVER_MODULE_OCCLUSION",
    "particles": "AVER_MODULE_PARTICLES",
    "fluids":    "AVER_MODULE_FLUIDS",
    "sr":        "AVER_MODULE_SR",
    "upgrade":   "AVER_MODULE_UPGRADE",
}

# The plain-C ABI each module exports for the managed layer. A module's namespace is not the only
# way to name it: aver_scene_material_name is declared in aver/scene/scene_abi.h, so it is exactly
# as absent with AVER_MODULE_SCENE=OFF as scene::World is -- and `scene-off` failed on two
# unguarded calls to it that the namespace scan could not see, because the symbol has no `::` in it.
ABI_MACRO = {
    "aver_scene_":    "AVER_MODULE_SCENE",
    "aver_fw_":       "AVER_MODULE_FRAMEWORK",
    "aver_phys_":     "AVER_MODULE_PHYSICS",
    "aver_pbr_":      "AVER_MODULE_PBR",
    "aver_voxi_":     "AVER_MODULE_VOXI",
    "aver_script_":   "AVER_MODULE_SCRIPTING",
}

# include directory under aver/ -> the macro that must be in scope. Mostly the same names, plus the
# joins that live in their own directory but are gated on the module they join to.
INC_MACRO = dict(NS_MACRO)
INC_MACRO.update({
    "save":       "AVER_MODULE_SCENE",
    "anim.scene": "AVER_MODULE_SCENE",
})

# "this macro being on PROVES that one is", read off the force-disable cascade in the root
# CMakeLists.txt. Nothing else implies anything: AVER_WITH_IMGUI, AVER_MODULE_SYNAPSE and
# AVER_MODULE_TRIFACTOR in particular prove nothing about any other module, and reading them as if
# they did is how most of these defects got written in the first place.
IMPLIES = {
    "AVER_MODULE_VOXI":            ["AVER_MODULE_PBR"],
    "AVER_MODULE_FRAMEWORK":       ["AVER_MODULE_SCENE"],
    "AVER_MODULE_PARTICLES":       ["AVER_MODULE_SCENE"],
    "AVER_MODULE_RENDER_SOFTBODY": ["AVER_MODULE_SCENE", "AVER_MODULE_PHYSICS"],
    "AVER_MODULE_SYNAPSE_SCENE":   ["AVER_MODULE_SCENE"],
    "AVER_FLUIDS_SIMULATED":       ["AVER_MODULE_FLUIDS", "AVER_MODULE_PHYSICS"],
}

# Macros worth tracking for the member scan. AVER_WITH_IMGUI is in because `no-ui` is a matrix row.
TRACKED = ("AVER_MODULE_", "AVER_FLUIDS_SIMULATED", "AVER_WITH_IMGUI")

# Modules with no option() of their own -- always compiled in -- so a mismatch on them cannot fail
# any configuration. Reported separately rather than dropped: a member sitting under a guard that
# has nothing to do with it is still wrong, it just is not urgent.
ALWAYS_ON = ("AVER_MODULE_SYNAPSE",)

MACRO_RE = re.compile(r"AVER_(?:MODULE|WITH|ENABLE|HAVE|RHI|FLUIDS)_[A-Z0-9_]+")
# A directive may be indented after the hash: `#  if AVER_MODULE_X`, which this tree writes for
# nested guards. Normalise it, or an indented #endif pops a frame its #if never pushed and the
# whole stack drifts -- which both invents defects and, worse, hides real ones.
HASH_RE = re.compile(r"^#[ \t]*")
# `#if !MACRO ... #else` puts the else branch in the configuration where MACRO DOES hold.
NEG_RE = re.compile(r"^#if *! *(AVER_[A-Z0-9_]+) *$")
FWD_RE = re.compile(r"namespace\s+aver::(\w+)\s*\{\s*(?:class|struct)\s+(\w+)\s*;")
INC_RE = re.compile(r'#include *[<"]aver/([A-Za-z0-9_.]+)/')
STR_RE = re.compile(r'"[^"]*"')
# A declaration, not a call: a type, then the name, then one of ( = ; {
DECL_RE = re.compile(r"^((?:[A-Za-z_][\w:<>,&*\s\[\]]*?\s|[*&]\s*))([A-Za-z_]\w*)\s*(?:\(|=|;|\{)")

SOURCE_EXT = (".cpp", ".hpp", ".h", ".inl")


def norm(line):
    return HASH_RE.sub("#", line.strip())


def satisfied(macro, have):
    if macro in have:
        return True
    for m, implied in IMPLIES.items():
        if m in have and macro in implied:
            return True
    return False


def walk(roots):
    out = []
    for r in roots:
        if os.path.isfile(r):
            out.append(r)
            continue
        for dp, _, fns in os.walk(r):
            for fn in fns:
                if fn.endswith(SOURCE_EXT):
                    out.append(os.path.join(dp, fn))
    return sorted(out)


def scan(path):
    """Yield (lineno, raw_line, macros_in_effect) for every non-directive line."""
    stack = []          # [(holds, macros, raw_if_text)]
    in_block = False
    for i, raw in enumerate(io.open(path, encoding="utf-8", errors="replace").read().split("\n"), 1):
        t = norm(raw)
        if in_block:
            if "*/" in t:
                in_block = False
            continue
        if t.startswith("/*") and "*/" not in t:
            in_block = True
            continue
        if t.startswith("#if"):
            stack.append((True, set(MACRO_RE.findall(t)), t))
            continue
        if t.startswith("#elif"):
            if stack:
                stack[-1] = (True, stack[-1][1] | set(MACRO_RE.findall(t)), stack[-1][2])
            continue
        if t.startswith("#else"):
            if stack:
                m = NEG_RE.match(stack[-1][2])
                stack[-1] = ((True, set([m.group(1)]), stack[-1][2]) if m
                             else (False, stack[-1][1], stack[-1][2]))
            continue
        if t.startswith("#endif"):
            if stack:
                stack.pop()
            continue
        if t.startswith("//") or t.startswith("*"):
            continue
        have = set()
        for holds, macros, _raw in stack:
            if holds:
                have |= macros
        yield i, raw, have, t


def code_of(raw):
    """The line with its trailing comment and string literals removed."""
    return STR_RE.sub('""', raw.split("//")[0])


# ---------------------------------------------------------------------------------------------
# The three scans.
# ---------------------------------------------------------------------------------------------

def audit_includes(files, report):
    for path in files:
        for i, raw, have, t in scan(path):
            m = INC_RE.match(t)
            if not m:
                continue
            macro = INC_MACRO.get(m.group(1))
            if not macro or satisfied(macro, have):
                continue
            report("includes", path, i, t[:90], macro, sorted(have))


def collect_forward_declared(roots):
    """Types forward-declared outside EVERY guard. A pointer or reference to one of these is legal
    with the module absent, and is how a function passes a module type through a build that has no
    module -- see GameLandscape.hpp:39."""
    fwd = set()
    for path in walk(roots):
        depth = 0
        for raw in io.open(path, encoding="utf-8", errors="replace"):
            t = norm(raw)
            if t.startswith("#if"):
                depth += 1
            elif t.startswith("#endif"):
                depth = max(0, depth - 1)
            elif depth == 0:
                for m in FWD_RE.finditer(t):
                    fwd.add(m.group(1) + "::" + m.group(2))
    return fwd


def audit_abi(files, report):
    for path in files:
        for i, raw, have, t in scan(path):
            if t.startswith("#"):
                continue          # the #include of the ABI header is the include scan's business
            code = code_of(raw)
            for prefix, macro in ABI_MACRO.items():
                m = re.search(r"(?<![\w:])" + prefix + r"\w+", code)
                if not m or satisfied(macro, have):
                    continue
                report("abi", path, i, t[:110], macro, sorted(have), extra=m.group(0))
                break


def audit_namespaces(files, fwd, report):
    for path in files:
        for i, raw, have, t in scan(path):
            code = code_of(raw)
            for ns, macro in NS_MACRO.items():
                m = re.search(r"(?<![\w:])" + ns + r"::(\w+)", code)
                if not m or satisfied(macro, have):
                    continue
                qual = ns + "::" + m.group(1)
                after = code[m.end():].lstrip()
                if qual in fwd and (after.startswith("*") or after.startswith("&")):
                    break
                report("namespace", path, i, t[:110], macro, sorted(have), extra=qual)
                break


def declared_names(t):
    """Every name a declaration line introduces, comment already stripped and ending in `;`.

    ONE LINE CAN DECLARE SEVERAL. `std::string levelPath_, levelName_;` declares two, and reading
    only one of them is how levelPath_ -- used from an unguarded body in SandboxLevelLoad.cpp --
    went unreported while levelName_ beside it was found. Reading only the FIRST is no better: a
    non-greedy type match lands on the last declarator, not the first.

    So variables are read as a declarator LIST: drop the semicolon, split on commas, throw away any
    initialiser and any array extent, and take the last identifier of what is left -- which is the
    declarator's own name whatever the type in front of it looks like. Functions keep the simple
    match, since a parameter list has commas of its own that mean something else entirely."""
    m = DECL_RE.match(t)
    if not m or not m.group(1).strip():
        return []
    head = t[:m.end()]
    if head.rstrip().endswith("(") or head.rstrip().endswith("{"):
        return [m.group(2)]                      # a function declaration
    names = []
    for part in t[:-1].split(","):
        part = part.split("=")[0]
        part = part.split("[")[0]
        ids = re.findall(r"[A-Za-z_]\w*", part)
        if ids:
            names.append(ids[-1])
    return names


def collect_members(header, cls=None):
    """name -> the set of tracked macros its declaration needs.

    ONE CLASS, AND CLASS SCOPE WITHIN IT. Members sit at exactly four spaces; anything deeper is a
    local or a parameter inside an inline body, which no other translation unit can reach. And the
    scan stops at the end of the named class, because this header also declares MeshObj, whose
    `material` member is guarded on AVER_MODULE_PBR -- without the bound, every parameter named
    `material` anywhere in the tree came back as a defect.

    A NAME DECLARED IN BOTH BRANCHES OF ONE #if IS UNCONDITIONAL and is dropped. That is not an
    edge case: applyProjectRenderSettings is declared once under `#if AVER_MODULE_VOXI` and again
    in the matching `#else`, so it exists in every configuration, and reading only the first
    declaration reported all six of its call sites."""
    decl = {}
    unconditional = set()
    depth = 0
    started = False
    if cls is None:
        cls = os.path.splitext(os.path.basename(header))[0]
    for i, raw, have, t in scan(header):
        if not started:
            if re.match(r"(?:class|struct)\s+" + re.escape(cls) + r"\b", t):
                started = True
                depth = t.count("{") - t.count("}")
            continue
        depth += t.count("{") - t.count("}")
        if depth <= 0:
            break
        if not (raw.startswith("    ") and not raw.startswith("     ")):
            continue
        # THE TRAILING COMMENT COMES OFF FIRST. Half the members in this header end in one --
        # `std::string outlinerFilter_;   // name filter box` -- and testing endswith(";") against
        # the raw line silently skipped every single one of them.
        t = t.split("//")[0].rstrip()
        if not t.endswith(";"):
            continue
        if t.startswith(("return", "if", "for", "while", "}", "{", ")", "using", "friend")):
            continue
        mods = {x for x in have if x.startswith(TRACKED[0]) or x in TRACKED[1:]}
        for n in declared_names(t):
            if n.isupper() or not (n.endswith("_") or len(n) >= 6):
                continue
            if not mods:
                unconditional.add(n)
                continue
            decl[n] = mods if n not in decl else (decl[n] & mods)
    return {n: v for n, v in decl.items() if v and n not in unconditional}


def audit_members(header, files, report):
    decl = collect_members(header)
    if not decl:
        return
    # ONE alternation over every declared name. The per-name form is quadratic and does not finish.
    use_re = re.compile(r"(?<![\w:.>])(" +
                        "|".join(re.escape(n) for n in sorted(decl, key=len, reverse=True)) +
                        r")(?![\w])")
    hdr_abs = os.path.abspath(header)

    def own_names(path):
        """Member names the file declares for a class OF ITS OWN (its paired header counts).

        GraphEditor has its own copySelection, pasteClipboard, deleteSelection and selectionBounds.
        Matching by name alone, every one of those read as a use of SandboxApp's same-named member
        under the wrong guard. A file that declares the name itself is talking about its own."""
        names = set()
        stem = os.path.splitext(path)[0]
        for cand in (stem + ".hpp", stem + ".h", path):
            # NEVER THE AUDITED HEADER ITSELF. SandboxApp.cpp pairs with SandboxApp.hpp, so without
            # this line every member it declares counted as "its own" and the whole file -- the
            # largest consumer of these members in the tree -- was silently exempt.
            if not os.path.exists(cand) or os.path.abspath(cand) == hdr_abs:
                continue
            for _i, raw, _have, t in scan(cand):
                if not (raw.startswith("    ") and not raw.startswith("     ")):
                    continue
                if not t.endswith(";"):
                    continue
                m = DECL_RE.match(t)
                if m and m.group(1).strip():
                    names.add(m.group(2))
        return names

    for path in files:
        if os.path.abspath(path) == hdr_abs:
            continue
        mine = own_names(path)
        for i, raw, have, t in scan(path):
            code = code_of(raw)
            seen = set()
            for m in use_re.finditer(code):
                n = m.group(1)
                if n in seen:
                    continue
                seen.add(n)
                need = decl.get(n)
                if not need or n in mine:
                    continue
                for macro in sorted(need):
                    if satisfied(macro, have):
                        continue
                    report("members", path, i, t[:110], macro, sorted(have),
                           extra=n + " declared under " + "+".join(sorted(need)))
                    break


# ---------------------------------------------------------------------------------------------

def main(argv):
    only = None
    header = "sandbox/src/SandboxApp.hpp"
    roots = ["sandbox/src", "Runtime/src", "Runtime/include"]
    roots_given = False
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--only":
            i += 1
            only = argv[i]
        elif a == "--header":
            i += 1
            header = argv[i]
        elif a == "--roots":
            i += 1
            roots = argv[i].split(",")
            roots_given = True
        else:
            sys.stderr.write("unknown argument: " + a + "\n")
            return 2
        i += 1

    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(repo)

    files = walk(roots)
    urgent = []
    deferred = []

    def report(kind, path, line, text, macro, have, extra=None):
        row = {
            "kind": kind, "path": path.replace("\\", "/"), "line": line,
            "text": text, "macro": macro, "have": have, "extra": extra,
        }
        (deferred if macro in ALWAYS_ON else urgent).append(row)

    if only in (None, "includes"):
        audit_includes(files, report)
    if only in (None, "namespace"):
        audit_namespaces(files, collect_forward_declared(["Runtime/include", "Runtime/src",
                                                          "sandbox/src", "modules"]), report)
    if only in (None, "abi"):
        audit_abi(files, report)
    if only in (None, "members"):
        # THE HEADER'S OWN TREE, not every root: the match is by name, and SandboxApp's members
        # collide with unrelated same-named members of the runtime's classes. Pass --roots to widen
        # it deliberately.
        member_roots = roots if roots_given else [os.path.dirname(header)]
        audit_members(header, walk(member_roots), report)

    for row in urgent:
        print("%s:%d  [%s] needs %s" % (row["path"], row["line"], row["kind"], row["macro"]))
        if row["extra"]:
            print("        %s" % row["extra"])
        print("        %s" % row["text"])
        print("        in effect: %s" % (", ".join(row["have"]) or "(nothing)"))

    print("")
    print("%d finding(s) that can fail a module-matrix configuration." % len(urgent))
    if deferred:
        print("%d more under a module with no option() of its own (%s) -- untidy, cannot fail a build."
              % (len(deferred), ", ".join(ALWAYS_ON)))
    return 1 if urgent else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
