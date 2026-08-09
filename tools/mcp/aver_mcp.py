#!/usr/bin/env python3
"""An MCP server for driving the Aver engine.

WHY THIS EXISTS. Every visual defect found in this tree was found by building, launching Sandbox.exe
with flags, screenshotting, cropping and looking -- nine of them in one phase (docs/STATUS.md 4u), and
three more in the session that wrote this file. That loop is entirely mechanical and I have run it by
hand a dozen times. This turns it into tools an agent can call, so "open it and look" stops being the
step that gets skipped because it is tedious.

IT DRIVES THE EXISTING CLI AND CHANGES NOTHING IN THE ENGINE. Sandbox.exe already takes 43 flags --
--frames, --screenshot, --probe-rel, --open-asset, --force-caps, --warp and the rest -- and the gates
oracle is built entirely out of them. So there is no engine-side listener here, no socket, no named
pipe, and no risk to a working editor. That is a deliberate first cut, not an oversight: a live control
channel is a real feature with threading and lifetime concerns, and it should not be the thing that
also introduces this tooling.

WHAT IT WILL NOT DO, and these are refusals rather than omissions:

  * It never runs `gates.ps1 -Record`. Re-recording overwrites the only record of what the renderer
    used to do; it is the user's call, and scripts/record-gates.ps1 is how they make it.
  * It never launches an interactive Sandbox.exe. --frames is always passed, so every run terminates on
    its own. A windowed editor left running holds bin/*.dll open and the next build fails LNK1168 --
    and the user may have their own editor open, which this must not disturb.
  * It never kills a process it did not start.

TRANSPORT: JSON-RPC 2.0, newline-delimited, over stdin/stdout. PURE STDLIB -- no mcp package, no npm
install. Nothing to provision on a fresh machine, and nothing whose licence has to be vetted against
this repo's permissive-only rule.
"""
import base64
import json
import os
import re
import subprocess
import sys
import time

# The repo root: this file lives at tools/mcp/, so that is THREE levels up, not two. Two was the first
# version and it put ROOT at tools/ -- every PowerShell call would have run `./scripts/build.ps1` from
# the wrong directory. Found because the tool-level exception handler reported it as a result instead of
# taking the server down with it.
#
# Asserted rather than assumed, because a wrong ROOT fails confusingly: `build.ps1 not found` sends a
# reader looking for a missing script rather than a miscounted dirname.
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if not os.path.isfile(os.path.join(ROOT, "CMakeLists.txt")):
    sys.stderr.write("[aver-mcp] FATAL: %s is not the repo root (no CMakeLists.txt)\n" % ROOT)
    sys.exit(2)

SERVER_NAME = "aver-engine"
SERVER_VERSION = "0.1.0"

# stdout is the protocol channel; anything that is not a JSON-RPC message corrupts the stream. So all
# diagnostics go to stderr, and print() is never used.
def log(msg):
    sys.stderr.write("[aver-mcp] %s\n" % msg)
    sys.stderr.flush()


def run_powershell(script, timeout):
    """Run a PowerShell command from the repo root and return (exit, stdout+stderr)."""
    proc = subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
        cwd=ROOT, capture_output=True, text=True, timeout=timeout, errors="replace")
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


# --------------------------------------------------------------------------------------------------
# tools
# --------------------------------------------------------------------------------------------------

def tool_build(args):
    release = bool(args.get("release"))
    extra = args.get("cmake_args") or []
    build_dir = args.get("build_dir")
    cmd = "./scripts/build.ps1"
    if release:
        cmd += " -Release"
    if build_dir:
        cmd += ' -BuildDir "%s"' % build_dir
    for a in extra:
        cmd += " " + a
    code, out = run_powershell(cmd, timeout=1800)

    # The whole log is thousands of lines of Ninja progress. What a caller needs is whether it built and
    # what went wrong, so errors and warnings are extracted and the rest is dropped.
    errors = [l.strip() for l in out.splitlines() if re.search(r"\berror\b [A-Z]+\d+|error:", l)]
    warnings = [l.strip() for l in out.splitlines() if re.search(r"\bwarning [A-Z]+\d+", l)]
    tail = [l.strip() for l in out.splitlines() if l.strip().startswith("[build]")]
    return {
        "ok": code == 0,
        "exit": code,
        "status": tail[-1] if tail else "(no [build] line)",
        "errors": errors[:40],
        "warning_count": len(warnings),
    }


# Which build tree aver_run / aver_tests reach into.
#
# WHY THIS EXISTS AT ALL. aver_build has always taken build_dir; aver_run and aver_tests hardcoded
# "build"/"build-release". That asymmetry is worse than it sounds, because agents in this repo are
# told to build into their OWN tree and never touch `build` -- so they could compile a change and
# then had no way to run it. aver_run silently launched `build/bin/Sandbox.exe` instead: a DIFFERENT,
# older binary that does not have their change and does not know their new flag, which an unknown
# flag being ignored rather than rejected makes completely silent. A whole perf investigation
# reported four frame-time numbers for a flag the binary it measured had never heard of, and only
# caught it by noticing the timestamps. Numbers like that are worse than no numbers.
#
# release + build_dir together: build_dir wins, because it is the more specific statement of intent.
# `release` only ever selected a tree NAME, and naming the tree outright says the same thing better.
def resolve_tree(args):
    """Returns (tree_name, error_or_None). Same charset check tool_package uses."""
    build_dir = args.get("build_dir")
    if build_dir:
        # Rejects path separators and .., so this can only ever name a sibling of the repo root --
        # the argument is interpolated into a filesystem path below.
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", build_dir):
            return None, ("suspicious build_dir %r: letters, digits, dot, dash and underscore only, "
                          "and it must be a directory directly under the repo root" % build_dir)
        return build_dir, None
    return ("build-release" if bool(args.get("release")) else "build"), None


# The flags this refuses to pass through, and why. A denylist rather than an allowlist because the CLI
# grows and an allowlist would silently block new flags; these two are the only ones that can outlive
# the call.
_REFUSED_FLAGS = {
    "--headless": "pointless here -- a headless run cannot produce the screenshot this tool exists for",
}

def tool_run(args):
    frames = int(args.get("frames", 40))
    if frames <= 0:
        return {"ok": False, "error": "frames must be positive: an interactive run would never exit, "
                                      "would hold bin/*.dll open (LNK1168 on the next build), and might "
                                      "collide with the user's own editor"}
    flags = list(args.get("flags") or [])
    for f in flags:
        if f in _REFUSED_FLAGS:
            return {"ok": False, "error": "%s refused: %s" % (f, _REFUSED_FLAGS[f])}

    tree, err = resolve_tree(args)
    if err:
        return {"ok": False, "error": err}
    exe = os.path.join(ROOT, tree, "bin", "Sandbox.exe")
    if not os.path.exists(exe):
        return {"ok": False, "error": "%s does not exist -- build first" % exe}

    shot = args.get("screenshot")
    argv = [exe, "--frames", str(frames)]
    if shot:
        shot = os.path.abspath(shot)
        os.makedirs(os.path.dirname(shot), exist_ok=True)
        argv += ["--screenshot", shot]
    argv += flags

    started = time.time()
    try:
        proc = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True,
                              timeout=int(args.get("timeout", 600)), errors="replace")
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "the run did not finish in time; it was NOT killed by force -- "
                                      "check for a windowed instance still open"}
    out = (proc.stdout or "") + (proc.stderr or "")
    lines = out.splitlines()

    # The probe line, parsed rather than handed back raw. This is the engine's own self-validating
    # measurement -- raw codes, the viewport rect, and an in/outside tag -- and it is what the gates are
    # built on. A caller that had to re-grep it would re-implement the parse in every call site.
    probe = None
    for l in lines:
        if "probe (" in l:
            m = re.search(r"raw \(([\d, ]+)\)", l)
            rect = re.search(r"viewport \((\d+),(\d+) (\d+)x(\d+)\)", l)
            tag = re.search(r"(in-viewport|OUTSIDE-VIEWPORT|VIEWPORT-MOVED)", l)
            probe = {
                "raw": re.sub(r"\s", "", m.group(1)) if m else None,
                "viewport": ("%s,%s %sx%s" % rect.groups()) if rect else None,
                "placement": tag.group(1) if tag else None,
            }
            break

    grep = args.get("grep")
    matched = [l.strip() for l in lines if re.search(grep, l)] if grep else []
    errors = [l.strip() for l in lines if "[ERROR" in l or "[FATAL" in l]
    warns = [l.strip() for l in lines if "[WARN" in l]

    return {
        "ok": proc.returncode == 0,
        "exit": proc.returncode,
        "seconds": round(time.time() - started, 1),
        "probe": probe,
        "screenshot": shot if (shot and os.path.exists(shot)) else None,
        "errors": errors[:30],
        "warnings": warns[:30],
        "matched": matched[:60],
        "log_lines": len(lines),
        # WHICH BINARY THIS ACTUALLY MEASURED, always reported, never on request. A result that does
        # not say what it ran is indistinguishable from a result that ran the wrong thing, and the
        # engine ignores unrecognized flags rather than rejecting them, so "my new flag did nothing"
        # and "my new flag was never in this binary" produce identical output. See resolve_tree.
        "binary": binary_provenance(exe),
    }


# Describes the binary a run actually used, and says so out loud when source is newer than it.
#
# The staleness test is deliberately a HEURISTIC, and reported as one: a source file newer than the
# executable means the executable cannot contain that edit, which is sound, but the converse proves
# nothing (an unrelated edit also trips it). A false "stale" costs a rebuild; a false "fresh" costs a
# published number that was never real, which is the failure this is here to prevent.
def binary_provenance(exe):
    built = os.path.getmtime(exe)
    newest, newest_path = 0.0, None
    for sub in ("modules", "sandbox", "game"):
        base = os.path.join(ROOT, sub)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            # Vendored third-party trees are huge and are not what a caller just edited.
            dirnames[:] = [d for d in dirnames if d not in ("Jolt", "third_party", ".git")]
            for fn in filenames:
                if fn.endswith((".cpp", ".hpp", ".h", ".hlsl", ".cs")):
                    p = os.path.join(dirpath, fn)
                    try:
                        m = os.path.getmtime(p)
                    except OSError:
                        continue
                    if m > newest:
                        newest, newest_path = m, os.path.relpath(p, ROOT)
    info = {
        "path": os.path.relpath(exe, ROOT),
        "built": time.strftime("%H:%M:%S", time.localtime(built)),
    }
    if newest_path and newest > built:
        info["stale"] = True
        info["newer_source"] = newest_path
        info["newer_source_at"] = time.strftime("%H:%M:%S", time.localtime(newest))
        info["warning"] = ("this binary predates %s -- it cannot contain that edit, and any flag added "
                           "with it will be silently ignored rather than rejected. Rebuild into this "
                           "tree, or pass build_dir to name the tree you actually built."
                           % newest_path)
    return info


def tool_inspect_image(args):
    """Crop and/or downscale a PNG so a region is actually legible when viewed.

    Cropping matters more than it sounds: the backbuffer is 3532x1987, and a toolbar viewed at full
    frame is a few pixels tall. Every UI defect found in this tree was found in a crop.
    """
    try:
        from PIL import Image
    except ImportError:
        return {"ok": False, "error": "Pillow is not installed; cannot crop"}

    src = os.path.abspath(args["path"])
    if not os.path.exists(src):
        return {"ok": False, "error": "%s does not exist" % src}
    im = Image.open(src).convert("RGB")
    full = im.size

    box = args.get("box")
    if box:
        x, y, w, h = (int(v) for v in box)
        # Clamped rather than rejected: an off-by-a-few crop request should give a slightly smaller
        # image, not an error that sends the caller back to guess again.
        x = max(0, min(x, full[0] - 1)); y = max(0, min(y, full[1] - 1))
        w = max(1, min(w, full[0] - x)); h = max(1, min(h, full[1] - y))
        im = im.crop((x, y, x + w, y + h))

    scale = float(args.get("scale", 1.0))
    if scale != 1.0:
        # NEAREST, so nothing is invented by the resample. A smoothed crop of a one-pixel UI seam is a
        # crop that no longer shows the seam.
        im = im.resize((max(1, int(im.width * scale)), max(1, int(im.height * scale))), Image.NEAREST)

    dst = args.get("out") or (os.path.splitext(src)[0] + "_crop.png")
    dst = os.path.abspath(dst)
    im.save(dst)
    return {"ok": True, "path": dst, "size": list(im.size), "source_size": list(full)}


def tool_tests(args):
    tree, err = resolve_tree(args)
    if err:
        return {"ok": False, "error": err}
    binf = os.path.join(ROOT, tree, "bin")
    if not os.path.isdir(binf):
        return {"ok": False, "error": "%s does not exist -- build first" % binf}
    only = args.get("only")
    results, failed = [], 0
    for name in sorted(os.listdir(binf)):
        if not name.endswith("Test.exe"):
            continue
        if only and only.lower() not in name.lower():
            continue
        try:
            p = subprocess.run([os.path.join(binf, name)], cwd=ROOT, capture_output=True,
                               text=True, timeout=600, errors="replace")
        except subprocess.TimeoutExpired:
            results.append({"suite": name, "exit": None, "summary": "TIMED OUT"})
            failed += 1
            continue
        out = (p.stdout or "") + (p.stderr or "")
        summary = ""
        for l in reversed(out.splitlines()):
            if "assertions" in l or "passed" in l or "SKIP" in l:
                summary = l.strip()
                break
        if p.returncode != 0:
            failed += 1
        results.append({"suite": name, "exit": p.returncode, "summary": summary,
                        "failures": [l.strip() for l in out.splitlines() if "FAIL" in l][:10]})
    return {"ok": failed == 0, "suites": len(results), "failed": failed, "results": results}


def tool_gates(args):
    """A READ-ONLY gate sweep. -Record is not reachable from here, by design -- see the module docstring."""
    configs = args.get("configs") or ["baseline"]
    bad = [c for c in configs if not re.fullmatch(r"[a-z0-9,=-]+", c)]
    if bad:
        return {"ok": False, "error": "suspicious configuration name(s): %s" % bad}
    cmd = "./scripts/gates.ps1 -Config %s" % ",".join(configs)
    if args.get("release"):
        cmd += " -Release"
    code, out = run_powershell(cmd, timeout=3000)
    lines = out.splitlines()
    return {
        "ok": code == 0,
        "failures": code,
        "gates": [l.strip() for l in lines if re.search(r"raw\(", l)],
        "invariants": [l.strip() for l in lines if "INVARIANT" in l],
        "verdict": next((l.strip() for l in reversed(lines) if "GATE" in l and
                         ("PASS" in l or "FAILED" in l)), ""),
        "note": "read-only. To re-record, a human runs ./scripts/record-gates.ps1",
    }


def tool_package(args):
    """Stage a project into a runnable game directory, and optionally verify it.

    STAGING IS NOT DESTRUCTIVE to the repo -- it copies into an output directory the caller names --
    but it DOES overwrite that directory when -Force is passed, so `force` is opt-in here rather
    than implied. The two scripts are separate on purpose: staging produces a package, verifying
    proves the package runs somewhere else, and a tool that always did both would make it awkward to
    inspect the output between the two.
    """
    project = args.get("project") or ""
    out = args.get("out") or ""
    if not project or not out:
        return {"ok": False, "error": "both 'project' (a .ocproject) and 'out' are required"}
    if not project.lower().endswith(".ocproject"):
        return {"ok": False, "error": "'project' must be a .ocproject manifest"}
    for label, value in (("project", project), ("out", out)):
        if '"' in value or "`" in value or ";" in value:
            return {"ok": False, "error": "suspicious character in '%s'" % label}

    # build-game by default, not build-release. stage-game.ps1 REFUSES a tree configured
    # AVER_ENABLE_UI=ON, because its AverGame.exe links Dear ImGui and must not ship; defaulting to
    # the editor tree here would make every call fail with a message about a flag the caller never
    # passed.
    build_dir = args.get("build_dir") or "build-game"
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", build_dir):
        return {"ok": False, "error": "suspicious build_dir"}

    cmd = './scripts/stage-game.ps1 -Project "%s" -Out "%s" -BuildDir %s' % (project, out, build_dir)
    if args.get("config") in ("Debug", "Release"):
        cmd += " -Config %s" % args["config"]
    if args.get("force"):
        cmd += " -Force"
    code, out_text = run_powershell(cmd, timeout=900)
    lines = [l.strip() for l in out_text.splitlines() if l.strip().startswith("[game]")]
    result = {
        "ok": code == 0,
        "errors": code,
        "stage": lines,
        "verdict": next((l for l in reversed(lines) if "OK ->" in l or "FAILED" in l), ""),
    }
    if code != 0 or not args.get("verify"):
        return result

    vcode, vtext = run_powershell('./scripts/verify-game.ps1 -Package "%s"' % out, timeout=900)
    vlines = [l.strip() for l in vtext.splitlines() if l.strip().startswith("[verify]")]
    result["ok"] = vcode == 0
    result["verify"] = vlines
    result["verify_errors"] = vcode
    result["verify_verdict"] = next((l for l in reversed(vlines) if "OK --" in l or "FAILED" in l), "")
    return result


def tool_flags(args):
    """The engine's CLI flags, read out of the source so this cannot go stale."""
    src = os.path.join(ROOT, "sandbox", "src", "SandboxApp.cpp")
    with open(src, encoding="utf-8", errors="replace") as f:
        text = f.read()
    flags = sorted(set(re.findall(r'"(--[a-z0-9-]+)"', text)))
    return {"ok": True, "count": len(flags), "flags": flags,
            "refused": {k: v for k, v in _REFUSED_FLAGS.items()}}


TOOLS = [
    {
        "name": "aver_build",
        "description": "Build the engine. Returns whether it built plus any compiler errors, with the "
                       "Ninja progress dropped. Optional Release, build_dir and extra CMake args (e.g. "
                       "-DAVER_MODULE_LANDSCAPE=OFF to check a configuration still builds).",
        "inputSchema": {"type": "object", "properties": {
            "release": {"type": "boolean"},
            "build_dir": {"type": "string"},
            "cmake_args": {"type": "array", "items": {"type": "string"}},
        }},
        "fn": tool_build,
    },
    {
        "name": "aver_run",
        "description": "Run Sandbox.exe for a fixed number of frames with engine flags, optionally "
                       "writing a screenshot. Returns exit code, the parsed probe (raw codes, viewport "
                       "rect, in/outside tag), errors, warnings and any lines matching `grep`. Always "
                       "terminates: interactive runs are refused. Use aver_flags to see what is "
                       "available.",
        "inputSchema": {"type": "object", "properties": {
            "frames": {"type": "integer", "description": "must be > 0; default 40"},
            "flags": {"type": "array", "items": {"type": "string"}},
            "screenshot": {"type": "string", "description": "path for a PNG of the whole backbuffer"},
            "grep": {"type": "string", "description": "regex; matching log lines are returned"},
            "release": {"type": "boolean"},
            "build_dir": {"type": "string", "description": "build tree to run from (default 'build'). "
                                                           "MUST match the build_dir you passed to "
                                                           "aver_build, or you measure a stale binary "
                                                           "that silently ignores your new flags."},
            "timeout": {"type": "integer"},
        }},
        "fn": tool_run,
    },
    {
        "name": "aver_inspect_image",
        "description": "Crop and/or scale a screenshot so a region is legible. The backbuffer is "
                       "3532x1987, so a toolbar at full frame is a few pixels tall -- every UI defect "
                       "in this tree was found in a crop. box is [x, y, w, h].",
        "inputSchema": {"type": "object", "properties": {
            "path": {"type": "string"},
            "box": {"type": "array", "items": {"type": "integer"}},
            "scale": {"type": "number"},
            "out": {"type": "string"},
        }, "required": ["path"]},
        "fn": tool_inspect_image,
    },
    {
        "name": "aver_tests",
        "description": "Run the headless suites in bin/ and report pass/fail with each one's assertion "
                       "summary and any FAIL lines. `only` filters by substring.",
        "inputSchema": {"type": "object", "properties": {
            "only": {"type": "string"},
            "release": {"type": "boolean"},
            "build_dir": {"type": "string", "description": "build tree to run from (default 'build'). "
                                                           "MUST match the build_dir you passed to "
                                                           "aver_build, or you run stale executables."},
        }},
        "fn": tool_tests,
    },
    {
        "name": "aver_gates",
        "description": "Run the render-gate oracle READ-ONLY for the given configurations and report "
                       "each gate, the invariant checks and the verdict. Cannot re-record: that "
                       "overwrites the only record of what the renderer used to do and is a human's "
                       "call via ./scripts/record-gates.ps1.",
        "inputSchema": {"type": "object", "properties": {
            "configs": {"type": "array", "items": {"type": "string"},
                        "description": "baseline, no-rt, no-ms, sm60, tier1-no-typed-uav, "
                                       "no-cons-raster, all-off, no-dxc, warp"},
            "release": {"type": "boolean"},
        }},
        "fn": tool_gates,
    },
    {
        "name": "aver_package",
        "description": "Package a project into a standalone, runnable game directory with "
                       "./scripts/stage-game.ps1, and optionally prove it runs outside the tree "
                       "that built it with ./scripts/verify-game.ps1. Defaults to the build-game "
                       "tree: staging REFUSES a tree configured AVER_ENABLE_UI=ON, because its "
                       "AverGame.exe links Dear ImGui and is not shippable.",
        "inputSchema": {"type": "object", "properties": {
            "project": {"type": "string", "description": "path to the project's .ocproject manifest"},
            "out": {"type": "string", "description": "output directory for the package"},
            "build_dir": {"type": "string", "description": "build tree to stage from (default build-game)"},
            "config": {"type": "string", "description": "Debug or Release"},
            "force": {"type": "boolean", "description": "replace a non-empty output directory"},
            "verify": {"type": "boolean", "description": "also run verify-game.ps1 on the result"},
        }, "required": ["project", "out"]},
        "fn": tool_package,
    },
    {
        "name": "aver_flags",
        "description": "List the engine's command-line flags, read from the source so the list cannot "
                       "go stale, plus the ones this server refuses and why.",
        "inputSchema": {"type": "object", "properties": {}},
        "fn": tool_flags,
    },
]

BY_NAME = {t["name"]: t for t in TOOLS}


# --------------------------------------------------------------------------------------------------
# JSON-RPC 2.0 over newline-delimited stdio
# --------------------------------------------------------------------------------------------------

def handle(msg):
    method = msg.get("method")
    mid = msg.get("id")

    if method == "initialize":
        # The client's protocol version is echoed back rather than pinned to one this file knows about,
        # so a newer client is not refused by a server whose only job is to run a build.
        want = (msg.get("params") or {}).get("protocolVersion", "2025-06-18")
        return {"jsonrpc": "2.0", "id": mid, "result": {
            "protocolVersion": want,
            "capabilities": {"tools": {}},
            "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
        }}

    if method in ("notifications/initialized", "initialized"):
        return None                       # a notification: no id, no reply

    if method == "tools/list":
        return {"jsonrpc": "2.0", "id": mid, "result": {
            "tools": [{k: t[k] for k in ("name", "description", "inputSchema")} for t in TOOLS]}}

    if method == "tools/call":
        params = msg.get("params") or {}
        name = params.get("name")
        tool = BY_NAME.get(name)
        if not tool:
            return {"jsonrpc": "2.0", "id": mid,
                    "error": {"code": -32601, "message": "unknown tool %r" % name}}
        try:
            result = tool["fn"](params.get("arguments") or {})
        except Exception as e:
            # Reported as a tool RESULT rather than a protocol error: a build that blew up is an answer
            # about the build, and a caller should see it as one instead of as a broken server.
            result = {"ok": False, "error": "%s: %s" % (type(e).__name__, e)}
        payload = json.dumps(result, indent=2)
        return {"jsonrpc": "2.0", "id": mid, "result": {
            "content": [{"type": "text", "text": payload}],
            "isError": not result.get("ok", True),
        }}

    if mid is None:
        return None                       # any other notification
    return {"jsonrpc": "2.0", "id": mid,
            "error": {"code": -32601, "message": "unsupported method %r" % method}}


def main():
    log("serving from %s" % ROOT)
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError as e:
            log("bad JSON: %s" % e)
            continue
        reply = handle(msg)
        if reply is not None:
            sys.stdout.write(json.dumps(reply) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
