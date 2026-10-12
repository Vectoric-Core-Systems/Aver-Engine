# Builds cmake/modules.catalog.json from the module manifests (modules/<dir>/module.json) and checks it.
#
#   python scripts/module-catalog.py --write    # regenerate the catalog after editing a manifest
#   python scripts/module-catalog.py --check    # fail when a manifest changed and the catalog did not
#                                               # (registered as the ctest ModuleCatalogCheck)
#   python scripts/module-catalog.py            # validate the manifests only
#
# The catalog is what the launcher reads (from a source checkout or <install>/cmake/modules.catalog.json);
# CMake reads the manifests directly (cmake/AvModuleManifest.cmake). Format and rules: docs/MODULES.md.
#
# Validation here is the schema and the graph; whether a manifest matches the real CMake link lines is
# checked at configure time by aver_check_module_manifests().
#
# Exits 1 on any problem.

import argparse
import json
import os
import re
import sys

CATEGORIES = {"Core", "Rendering", "Gameplay", "Physics", "Animation", "Audio", "UI", "Tools",
              "Platform", "Networking", "Content"}
EDITIONS = ("standard", "slim", "full")
PLATFORMS = ("windows", "linux", "macos")
OPTION_RE = re.compile(r"^AVER_(MODULE|RHI)_[A-Z0-9_]+$")
STR_KEYS = ("name", "dir", "displayName", "description", "category", "consequence")
LIST_KEYS = ("dependsOn", "optionalDependsOn", "derivedFrom")


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_manifests(root, errors):
    out = []
    mdir = os.path.join(root, "modules")
    for d in sorted(os.listdir(mdir)):
        p = os.path.join(mdir, d, "module.json")
        if not os.path.isdir(os.path.join(mdir, d)):
            continue
        if not os.path.exists(p):
            errors.append("modules/%s has no module.json" % d)
            continue
        try:
            with open(p, encoding="utf-8") as f:
                m = json.load(f)
        except (OSError, ValueError) as e:
            errors.append("modules/%s/module.json: %s" % (d, e))
            continue
        m["_folder"] = d
        out.append(m)
    return out


def validate(mods, errors):
    by_name = {}
    options = {}
    for m in mods:
        w = "modules/%s/module.json" % m["_folder"]
        if m.get("schema") != 1:
            errors.append("%s: schema must be 1" % w)
        for k in STR_KEYS:
            if not isinstance(m.get(k), str):
                errors.append("%s: \"%s\" must be a string" % (w, k))
        for k in LIST_KEYS:
            v = m.get(k, [])
            if not isinstance(v, list) or not all(isinstance(x, str) for x in v) or len(set(v)) != len(v):
                errors.append("%s: \"%s\" must be a list of unique module names" % (w, k))
        if m.get("dir") != m["_folder"]:
            errors.append("%s: \"dir\" is %r, the folder is %r" % (w, m.get("dir"), m["_folder"]))
        if m.get("category") not in CATEGORIES:
            errors.append("%s: category %r is not one of %s" % (w, m.get("category"), sorted(CATEGORIES)))
        nm = m.get("name", "")
        if not nm.startswith("Aver."):
            errors.append("%s: name must start with \"Aver.\"" % w)
        if nm in by_name:
            errors.append("%s: name %s is also used by modules/%s" % (w, nm, by_name[nm]["_folder"]))
        by_name[nm] = m
        for k in ("required", "default"):
            if not isinstance(m.get(k), bool):
                errors.append("%s: \"%s\" must be true or false" % (w, k))
        opt = m.get("option")
        if "option" not in m or not (opt is None or isinstance(opt, str)):
            errors.append("%s: \"option\" must be a string or null" % w)
        elif opt is not None:
            if not OPTION_RE.match(opt):
                errors.append("%s: option %r must look like AVER_MODULE_X or AVER_RHI_X" % (w, opt))
            if opt in options:
                errors.append("%s: option %s is also used by %s" % (w, opt, options[opt]))
            options[opt] = nm
        ed = m.get("editions")
        if ed is not None:
            if not isinstance(ed, dict) or not set(ed) <= set(EDITIONS) or not all(isinstance(v, bool) for v in ed.values()):
                errors.append("%s: \"editions\" must map standard/slim/full to true or false" % w)
            elif opt is None:
                errors.append("%s: \"editions\" only makes sense on a module with an option" % w)
        plats = m.get("platforms")
        if plats is not None and (not isinstance(plats, list) or not plats or len(set(plats)) != len(plats)
                                  or not all(p in PLATFORMS for p in plats)):
            errors.append("%s: \"platforms\" must be a non-empty list of unique names from %s" % (w, list(PLATFORMS)))
        skel = m.get("skeleton", False)
        if skel:
            if opt is not None or m.get("required"):
                errors.append("%s: a skeleton has no option and is not required" % w)
            continue
        derived = m.get("derivedFrom", [])
        if m.get("required"):
            if opt is not None or derived:
                errors.append("%s: a required module has option null and no derivedFrom" % w)
        elif opt is not None:
            if derived:
                errors.append("%s: a module with an option cannot also be derivedFrom" % w)
        elif not derived:
            errors.append("%s: option is null and required is false, so \"derivedFrom\" must say what builds it" % w)

    # references
    for m in mods:
        w = "modules/%s/module.json" % m["_folder"]
        for k in LIST_KEYS:
            for r in m.get(k, []) if isinstance(m.get(k, []), list) else []:
                if r not in by_name:
                    errors.append("%s: %s names %s, which no manifest defines" % (w, k, r))
                elif r == m.get("name"):
                    errors.append("%s: %s names the module itself" % (w, k))
    if errors:
        return by_name

    # a required module can never have an off dependency
    for m in mods:
        if m.get("skeleton"):
            continue
        w = "modules/%s/module.json" % m["_folder"]
        if m["required"]:
            for d in m.get("dependsOn", []):
                dm = by_name[d]
                if not dm["required"]:
                    errors.append("%s: required module depends on %s, which can be switched off -- make that module "
                                  "required, or move the edge to optionalDependsOn if the link is guarded by if(TARGET ...)" % (w, d))

    # a hard edge must exist wherever the module does: dependsOn and derivedFrom may only name modules
    # built on every platform the dependent is built on (no "platforms" means all three)
    def plats_of(m):
        p = m.get("platforms")
        return set(p) if isinstance(p, list) else set(PLATFORMS)
    for m in mods:
        if m.get("skeleton"):
            continue
        w = "modules/%s/module.json" % m["_folder"]
        mine = plats_of(m)
        for k in ("dependsOn", "derivedFrom"):
            for d in m.get(k, []):
                missing = mine - plats_of(by_name[d])
                if missing:
                    errors.append("%s: %s names %s, which is not built on %s -- narrow this module's \"platforms\", "
                                  "or move the edge to optionalDependsOn behind if(TARGET ...)"
                                  % (w, k, d, ", ".join(sorted(missing))))

    # a derived module must not depend on anything that can be off while it is built: its deps must be
    # among the modules that are on whenever its derivedFrom are (those, their deps, required ones, and
    # derived modules whose own derivedFrom are all in the set)
    always = {n for n, m in by_name.items() if m["required"] and not m.get("skeleton")}
    for m in mods:
        if m.get("skeleton") or m["required"] or m.get("option") is not None:
            continue
        w = "modules/%s/module.json" % m["_folder"]
        on = set(m["derivedFrom"]) | always
        grew = True
        while grew:
            grew = False
            for n in list(on):
                for d in by_name[n].get("dependsOn", []):
                    if d not in on:
                        on.add(d); grew = True
            for n, x in by_name.items():
                if n not in on and not x.get("skeleton") and x.get("derivedFrom") and set(x["derivedFrom"]) <= on:
                    on.add(n); grew = True
        for d in m.get("dependsOn", []):
            if d not in on:
                errors.append("%s: derived module depends on %s, which can be off while %s is built"
                              % (w, d, ", ".join(m["derivedFrom"])))

    # cycles over the hard edges (dependsOn + derivedFrom)
    state = {}
    def visit(n, path):
        state[n] = 1
        for d in by_name[n].get("dependsOn", []) + by_name[n].get("derivedFrom", []):
            if state.get(d) == 1:
                errors.append("dependency cycle: %s" % " -> ".join(path + [n, d]))
            elif d not in state:
                visit(d, path + [n])
        state[n] = 2
    for n in sorted(by_name):
        if n not in state:
            visit(n, [])
    return by_name


def engine_version(root):
    with open(os.path.join(root, "CMakeLists.txt"), encoding="utf-8") as f:
        m = re.search(r"project\(\s*AverEngine\s+VERSION\s+([0-9][0-9.]*)", f.read())
    return m.group(1) if m else None


def build_catalog(root, mods):
    live = [{k: v for k, v in m.items() if not k.startswith("_")} for m in mods if not m.get("skeleton")]
    planned = [{k: m[k] for k in ("name", "dir", "displayName", "description", "category", "dependsOn")}
               for m in mods if m.get("skeleton")]
    live.sort(key=lambda m: m["name"])
    planned.sort(key=lambda m: m["name"])
    cat = {"schema": 1, "engineVersion": engine_version(root), "modules": live}
    if planned:
        cat["planned"] = planned   # README-only directories: nothing is built, the launcher must not offer them
    return json.dumps(cat, indent=2, ensure_ascii=False) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--write", action="store_true", help="regenerate cmake/modules.catalog.json")
    g.add_argument("--check", action="store_true", help="fail if the catalog is out of date")
    ap.add_argument("--root", default=repo_root(), help="engine source root (default: this checkout)")
    a = ap.parse_args()

    errors = []
    mods = load_manifests(a.root, errors)
    if not errors:
        validate(mods, errors)
    if not errors and not engine_version(a.root):
        errors.append("CMakeLists.txt: could not read project(AverEngine VERSION ...)")
    if errors:
        for e in errors:
            print("module-catalog: " + e, file=sys.stderr)
        print("module-catalog: %d problem(s)" % len(errors), file=sys.stderr)
        return 1

    text = build_catalog(a.root, mods)
    path = os.path.join(a.root, "cmake", "modules.catalog.json")
    n_opt = sum(1 for m in mods if m.get("option"))
    if a.write:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("module-catalog: wrote %s (%d modules, %d with an option)" % (os.path.relpath(path, a.root), len(mods), n_opt))
        return 0
    if a.check:
        try:
            with open(path, encoding="utf-8", newline="") as f:
                have = f.read().replace("\r\n", "\n")
        except OSError:
            have = None
        if have != text:
            print("module-catalog: cmake/modules.catalog.json is %s. A module.json changed; run:\n"
                  "    python scripts/module-catalog.py --write" % ("missing" if have is None else "out of date"), file=sys.stderr)
            return 1
        print("module-catalog: catalog is current (%d modules)" % len(mods))
        return 0
    print("module-catalog: %d manifests valid (%d with an option)" % (len(mods), n_opt))
    return 0


if __name__ == "__main__":
    sys.exit(main())
