"""Claim audit: does a check actually catch what it claims to? (idea from Drift Engine's claimAudit.sh; see
docs/TOOLING_COMPARISON_DRIFT.md)

    python scripts/claim-audit.py --file <src> --old "<text>" --new "<text>" --check "<command>"
                                  [--build "<command>"] [--expect-fail]

Applies ONE textual edit to <src> (the text must occur exactly once), rebuilds, runs the check, then restores the
file byte for byte and rebuilds again. Reports:
  CAUGHT    the check failed with the edit in place: it measures what it claims.
  SURVIVED  the check still passed: it is blind to this edit (the claim is unbacked).
The default build is `cmake --build build-release` in a Visual Studio developer environment. Exit code 0 when the
result is CAUGHT, 1 when SURVIVED, 2 on a setup error (no match, build failure).
"""
import argparse, os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def find_vcvars():
    # The newest Visual Studio with C++ tools, as vswhere reports it; "" when there is none.
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.exists(vswhere):
        return ""
    r = subprocess.run([vswhere, "-latest", "-products", "*", "-requires",
                        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                       capture_output=True, text=True)
    vs = r.stdout.strip().splitlines()[0] if r.stdout.strip() else ""
    path = os.path.join(vs, "VC", "Auxiliary", "Build", "vcvars64.bat") if vs else ""
    return path if path and os.path.exists(path) else ""


VCVARS = find_vcvars()


def run(cmd, label):
    print(f"[claim-audit] {label}: {cmd}", flush=True)
    full = f'call "{VCVARS}" >nul 2>&1 && {cmd}' if VCVARS else cmd
    r = subprocess.run(full, shell=True, cwd=ROOT, capture_output=True, text=True)
    tail = (r.stdout + r.stderr).strip().splitlines()[-6:]
    for line in tail:
        print("    " + line)
    return r.returncode


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--file", required=True)
    p.add_argument("--old", required=True)
    p.add_argument("--new", required=True)
    p.add_argument("--check", required=True, help="command whose exit code is the check (0 = pass)")
    p.add_argument("--build", default="cmake --build build-release --parallel 12")
    a = p.parse_args()

    path = os.path.join(ROOT, a.file) if not os.path.isabs(a.file) else a.file
    original = open(path, "rb").read()
    text = original.decode("utf-8")
    n = text.count(a.old)
    if n != 1:
        print(f"[claim-audit] --old occurs {n} times in {a.file}; it must occur exactly once")
        return 2
    try:
        open(path, "wb").write(text.replace(a.old, a.new).encode("utf-8"))
        if run(a.build, "build with the edit") != 0:
            print("[claim-audit] the edited source does not build; pick an edit that compiles")
            return 2
        passed = run(a.check, "check") == 0
    finally:
        open(path, "wb").write(original)
    run(a.build, "rebuild restored source")
    print(f"[claim-audit] {'SURVIVED: the check passed with the edit -- it is blind to it' if passed else 'CAUGHT: the check failed with the edit in place'}")
    return 1 if passed else 0


if __name__ == "__main__":
    sys.exit(main())
