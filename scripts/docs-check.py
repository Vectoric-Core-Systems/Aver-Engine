"""Docs that cannot rot, first step (gap 6 of docs/TOOLING_COMPARISON_DRIFT.md): every repo path a doc names in
backticks or a link must still exist.

    python scripts/docs-check.py [--list]     exit 1 when a doc names a path that is gone

Checks `docs/**/*.md`, `README.md`, `CONTRIBUTING.md`. A mention counts when it looks like a repo path: it starts
with a top-level repo directory (modules/, sandbox/, tools/, scripts/, tests/, docs/, Runtime/, cmake/, ...) and has a
file extension or a trailing slash. `path:line` suffixes and globs are allowed (globs must match something).
"""
import glob, os, re, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TOP = {d for d in os.listdir(ROOT) if os.path.isdir(os.path.join(ROOT, d)) and not d.startswith(".")} - {
    "build", "build-release", "build-relwithdebinfo", "third_party"}
TOKEN = re.compile(r"`([^`\s]+)`|\]\(([^)\s#]+)")


def candidates(text):
    for m in TOKEN.finditer(text):
        tok = (m.group(1) or m.group(2)).strip().rstrip(".,;")
        tok = tok.replace("\\", "/")
        tok = re.sub(r":\d+(-\d+)?$", "", tok)
        head = tok.split("/", 1)[0]
        if head not in TOP or "/" not in tok:
            continue
        if not (re.search(r"\.[A-Za-z0-9]{1,6}$", tok) or tok.endswith("/") or "*" in tok):
            continue
        yield tok


def expand(tok):
    """`a/{b,c}/d.*` -> every combination; `x.hpp/.cpp` -> x.hpp and x.cpp."""
    m = re.search(r"\{([^{}]*)\}", tok)
    if m:
        out = []
        for alt in m.group(1).split(","):
            out += expand(tok[:m.start()] + alt + tok[m.end():])
        return out
    m = re.match(r"^(.*)\.([A-Za-z0-9]+)/\.([A-Za-z0-9]+)$", tok)
    if m:
        return [f"{m.group(1)}.{m.group(2)}", f"{m.group(1)}.{m.group(3)}"]
    return [tok]


def exists(tok):
    p = os.path.join(ROOT, tok)
    return bool(glob.glob(p, recursive=True)) if "*" in tok else os.path.exists(p)


def main():
    docs = glob.glob(os.path.join(ROOT, "docs", "**", "*.md"), recursive=True)
    docs += [os.path.join(ROOT, f) for f in ("README.md", "CONTRIBUTING.md") if os.path.exists(os.path.join(ROOT, f))]
    missing = {}
    for d in docs:
        text = open(d, encoding="utf-8", errors="replace").read()
        # Lines about another project (the Drift comparison) name ITS paths; placeholders like <mod> are not paths.
        ours = "
".join(l for l in text.splitlines() if "Drift" not in l and "drft" not in l)
        for tok in set(candidates(ours)):
            if "..." in tok or "…" in tok or "<" in tok or not tok.isascii():
                continue   # elided or a placeholder
            # A brace or two-extension shorthand is fine when any spelling exists (docs write {include,src}).
            if not any(exists(t) for t in expand(tok)):
                missing.setdefault(os.path.relpath(d, ROOT), []).append(tok)
    n = sum(len(v) for v in missing.values())
    for d in sorted(missing):
        print(f"{d}: {len(missing[d])} missing")
        if "--list" in sys.argv:
            for t in sorted(missing[d]):
                print(f"    {t}")
    print(f"docs-check: {n} stale path(s) in {len(missing)} of {len(docs)} doc(s)")
    return 1 if n else 0


if __name__ == "__main__":
    sys.exit(main())
