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

TRANSPORT: two, both PURE STDLIB -- no mcp package, no npm install, nothing to provision on a fresh
machine and nothing whose licence has to be vetted against this repo's permissive-only rule.

  1. stdio (default, unchanged). JSON-RPC 2.0, newline-delimited, over stdin/stdout. This is what
     .mcp.json launches today (`{"command":"python","args":["tools/mcp/aver_mcp.py"]}`) and it is not
     going anywhere -- plain `python aver_mcp.py`, no arguments, is byte-for-byte the same loop it
     always was. main() below IS that loop.

  2. HTTP (opt-in: pass --http), a.k.a. MCP "Streamable HTTP". Same JSON-RPC messages, POSTed to
     /mcp as `application/json`, one request in and one response out -- proved sufficient against a
     real client (Claude Code) without also implementing the SSE-push half of the spec: a stray GET
     just gets a 405, and a client that never needed a server-initiated push tolerates that fine. See
     serve_http() below. Chosen because a bare TCP socket speaking this file's own line-delimited
     JSON-RPC is NOT one of the transports an MCP client can actually dial (checked: `claude mcp add
     --help` lists exactly stdio, sse, http) -- so "give it a port" has to mean HTTP or it would be an
     unreachable transport, which is worse than no transport at all.

  Both transports dispatch through the SAME handle(msg) -- the wire format changes, the protocol and
  every tool's behaviour do not.
"""
import base64
import http.server
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


# --------------------------------------------------------------------------------------------------
# mcp.conf: optional, developer-local port assignment. Same file, same grammar, as the one
# sandbox/src/McpConf.cpp reads for the editor's --mcp control channel (see mcp.conf.example at the
# repo root) -- this is the Python-side reader for the OTHER key in it, tool_server.port. Kept as a
# free function rather than folded into ROOT's module-level setup so it can be called with an
# explicit root/default in a test without touching the real mcp.conf.
# --------------------------------------------------------------------------------------------------

def _parse_strict_port(v):
    """Parses `v` as a port the same way the C++ reader's std::from_chars does: ASCII digits only,
    the whole (already-trimmed) string, nothing else -- no leading '+', no '_' digit-group
    separators, no leading/trailing junk. Returns an int or None.

    Plain int(v) is NOT equivalent: Python's int() accepts a leading '+' ("+8080") and PEP-515
    underscore grouping ("8_080") that std::from_chars rejects outright. Both readers' docstrings
    claim "the same contract" as each other; before this, that claim was false for exactly those two
    inputs -- the same mcp.conf line was accepted here and rejected (with a warning) on the C++ side,
    silently giving the editor and the tool server two different ports from one file. Caught by
    actually running both parsers on the same adversarial input, not by re-reading either one.
    """
    if not re.fullmatch(r"[0-9]+", v):
        return None
    return int(v)


def read_conf_port(root, key, default):
    """Reads `key` from <root>/mcp.conf as a TCP port (1-65535).

    Same optional, per-key contract as the C++ reader: a missing file, an empty file, or a missing
    key all fall back to `default` SILENTLY -- that is the normal case, not a problem. A key that IS
    present but does not parse (not a number, out of range, trailing junk) also falls back to
    `default`, but logs why, because that one is a typo a developer would otherwise never see.

    Grammar: one `key = value` per line, '#' and blank lines ignored, first occurrence of a
    duplicate key wins (matching or not -- the first match is authoritative either way, same as the
    C++ reader). Whitespace around the key and the value is trimmed: mcp.conf is meant to be
    hand-edited from mcp.conf.example, where every line is aligned with spaces around '=', so this is
    deliberately more forgiving than editor.ini's own machine-written-and-read grammar.

    Returns (port, source) where source is "mcp.conf" or "built-in default" (the latter possibly
    annotated with why, for the log line).
    """
    path = os.path.join(root, "mcp.conf")
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return default, "built-in default"     # no file: the ordinary, silent case

    for raw in text.splitlines():
        row = raw.rstrip("\r")
        stripped = row.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if "=" not in row:
            continue                            # a line with no '=': skipped, not fatal
        k, _, v = row.partition("=")
        k = k.strip()
        if k != key:
            continue
        v = v.strip()
        port = _parse_strict_port(v)
        if port is None or not (1 <= port <= 65535):
            log("mcp.conf: %r = %r is not a usable port (1-65535); falling back to default %s"
                % (key, v, default))
            return default, "built-in default (mcp.conf value invalid)"
        return port, "mcp.conf"                 # first occurrence wins
    return default, "built-in default"          # key not present: the ordinary, silent case


def resolve_tool_server_port(argv):
    """Explicit `--port` argument > mcp.conf's tool_server.port > built-in default (8787).

    Read once at startup and logged. Only actually BINDS anything when the process is also told
    --http (see main()/serve_http()) -- over stdio (still the default; see .mcp.json) the resolved
    value is reported but never opens a socket.
    """
    default = 8787
    cli_port = None
    for i, a in enumerate(argv):
        if a == "--port" and i + 1 < len(argv):
            cli_port = argv[i + 1]
        elif a.startswith("--port="):
            cli_port = a.split("=", 1)[1]
    if cli_port is not None:
        # Same strict digit-only grammar as read_conf_port, for the same reason: consistency between
        # this server's own two entry points into the same number, not just with the C++ reader.
        p = _parse_strict_port(cli_port)
        if p is not None and 1 <= p <= 65535:
            return p, "command line"
        log("--port %r is not a usable port (1-65535); ignoring, falling back to mcp.conf/default"
            % cli_port)
    return read_conf_port(ROOT, "tool_server.port", default)


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
    # Tags come from levelTag() in modules/core/src/Log.cpp, printed as "[TAG ] message".
    # "[CRIT" was added when the Critical level was: without it, the one severity that means "this
    # process may be about to die" was the only one that appeared in NO summary here -- it matched
    # neither the error nor the warning pattern and vanished silently from every gate run.
    errors = [l.strip() for l in lines if "[ERROR" in l or "[CRIT" in l or "[FATAL" in l]
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
    """Stage the ENGINE payload, and optionally verify it.

    THIS USED TO PACKAGE A GAME, and every call to it failed once that path was deleted: it shelled
    out to ./scripts/stage-game.ps1 and ./scripts/verify-game.ps1, which went with AverGame.exe. The
    engine payload -- the thing the launcher pulls and installs -- is what packaging means here now,
    so this points at stage-payload.ps1 / verify-payload.ps1 instead of failing on a missing file.

    NO `project` ARGUMENT ANY MORE, and that is the substantive difference rather than a rename: a
    game package was cut per-project, while a payload is the engine itself and there is no project to
    name. A caller still passing `project` is told plainly rather than having it ignored.

    STAGING IS NOT DESTRUCTIVE to the repo -- it copies into an output directory the caller names --
    but it DOES overwrite that directory when -Force is passed, so `force` stays opt-in. The two
    scripts stay separate for the same reason as before: staging produces a payload, verifying proves
    it runs somewhere else, and a tool that always did both would make it awkward to look at the
    output in between.
    """
    out = args.get("out") or ""
    if not out:
        return {"ok": False, "error": "'out' is required (the directory to stage the payload into)"}
    if args.get("project"):
        return {"ok": False,
                "error": "packaging is per-ENGINE now, not per-project: there is no packaged-game "
                         "path any more (AverGame.exe and stage-game.ps1 were removed). Drop "
                         "'project' and pass only 'out'."}
    if '"' in out or "`" in out or ";" in out:
        return {"ok": False, "error": "suspicious character in 'out'"}

    # Release by default, because a payload is what ships. stage-payload.ps1 derives its own build
    # directory from the config when none is given, so `build_dir` stays optional rather than being
    # defaulted here to a tree that may not exist.
    build_dir = args.get("build_dir") or ""
    if build_dir and not re.fullmatch(r"[A-Za-z0-9_.-]+", build_dir):
        return {"ok": False, "error": "suspicious build_dir"}

    cmd = './scripts/stage-payload.ps1 -Out "%s"' % out
    if build_dir:
        cmd += " -BuildDir %s" % build_dir
    cmd += " -Config %s" % (args["config"] if args.get("config") in ("Debug", "Release") else "Release")
    if args.get("with_samples"):
        cmd += " -WithSamples"
    if args.get("force"):
        cmd += " -Force"
    code, out_text = run_powershell(cmd, timeout=900)
    lines = [l.strip() for l in out_text.splitlines() if l.strip().startswith("[stage]")]
    result = {
        "ok": code == 0,
        "errors": code,
        "stage": lines,
        "verdict": next((l for l in reversed(lines) if "OK ->" in l or "FAILED" in l), ""),
    }
    if code != 0 or not args.get("verify"):
        return result

    vcode, vtext = run_powershell('./scripts/verify-payload.ps1 -Payload "%s"' % out, timeout=900)
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
        "description": "Stage the ENGINE payload -- what the Aver Launcher pulls and installs -- with "
                       "./scripts/stage-payload.ps1, and optionally prove it runs outside the tree "
                       "that built it with ./scripts/verify-payload.ps1. There is no packaged-GAME "
                       "path any more: AverGame.exe and stage-game.ps1 were removed, so this takes no "
                       "project. Defaults to Release, because a payload is what ships.",
        "inputSchema": {"type": "object", "properties": {
            "out": {"type": "string", "description": "directory to stage the payload into"},
            "build_dir": {"type": "string", "description": "build tree to stage from (default: derived from config)"},
            "config": {"type": "string", "description": "Debug or Release (default Release)"},
            "with_samples": {"type": "boolean", "description": "include the sample content"},
            "force": {"type": "boolean", "description": "replace a non-empty output directory"},
            "verify": {"type": "boolean", "description": "also run verify-payload.ps1 on the result"},
        }, "required": ["out"]},
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
        # WHICH CHECKOUT THIS PROCESS ACTED IN, on every result, unconditionally -- not just tool_run's
        # existing binary_provenance (which reports a path RELATIVE to ROOT, so it never actually reveals
        # which ROOT that was). One long-lived process has exactly one ROOT for its entire lifetime, and
        # that ROOT is fixed by wherever the *.mcp.json (or whoever else) pointed the interpreter at this
        # script -- NOT by the caller's own cwd or intent. A stale or hand-edited launcher can point at a
        # different checkout than the one the caller is scoped to, and until now nothing in the response
        # said so: the wrong tree answered silently (see the long comment above resolve_tree, and
        # memory/aver-mcp-tools-wrong-tree.md). This does not choose or fix which tree gets used -- it
        # makes whichever one was used impossible to miss.
        if isinstance(result, dict):
            result.setdefault("root", ROOT)
        payload = json.dumps(result, indent=2)
        return {"jsonrpc": "2.0", "id": mid, "result": {
            "content": [{"type": "text", "text": payload}],
            "isError": not result.get("ok", True),
        }}

    if mid is None:
        return None                       # any other notification
    return {"jsonrpc": "2.0", "id": mid,
            "error": {"code": -32601, "message": "unsupported method %r" % method}}


def serve_stdio():
    """The original, and still default, transport. Unchanged: a caller on stdio today gets exactly
    what it got before this file grew an HTTP option."""
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


# Loopback ONLY -- matches modules/mcp's McpBridge, which binds 127.0.0.1 for the same reason (this is
# a build/test/package runner with no auth of its own; reachable-from-the-network is a very different
# risk profile and nothing in this task asked for it). Not configurable by design: the port is the only
# knob mcp.conf exposes for this server, on purpose.
_HTTP_HOST = "127.0.0.1"
_HTTP_PATH = "/mcp"


class _McpHTTPHandler(http.server.BaseHTTPRequestHandler):
    """POST /mcp with a single JSON-RPC message; get back its reply. That's the whole surface.

    Maps handle(msg)'s existing return convention onto HTTP the only way that's consistent with it:
    a dict reply -> 200 application/json with that dict as the body; None (a notification, e.g.
    notifications/initialized) -> 202 with an empty body, which is what a real client (Claude Code)
    was proven to send and accept -- see the protocol transcripts in the Part 2 report. GET is refused
    with 405: the SSE-push half of Streamable HTTP is not implemented because the live capture showed
    a client that never needed a server-initiated push tolerates a 405 on that GET without incident.
    """

    server_version = "aver-mcp-http/1"

    def log_message(self, fmt, *args):
        log("http %s - %s" % (self.address_string(), (fmt % args)))

    def _reply_json(self, status, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _reply_empty(self, status):
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_POST(self):
        if self.path.split("?", 1)[0].rstrip("/") != _HTTP_PATH:
            self._reply_json(404, {"jsonrpc": "2.0", "id": None,
                                    "error": {"code": -32600, "message": "no such endpoint %r; use %s"
                                              % (self.path, _HTTP_PATH)}})
            return
        try:
            length = int(self.headers.get("Content-Length", "0") or "0")
        except ValueError:
            length = 0
        raw = self.rfile.read(length) if length > 0 else b""
        try:
            msg = json.loads(raw.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError) as e:
            self._reply_json(400, {"jsonrpc": "2.0", "id": None,
                                    "error": {"code": -32700, "message": "parse error: %s" % e}})
            return
        try:
            reply = handle(msg)
        except Exception as e:
            self._reply_json(500, {"jsonrpc": "2.0", "id": msg.get("id"),
                                    "error": {"code": -32603, "message": "%s: %s" % (type(e).__name__, e)}})
            return
        if reply is None:
            self._reply_empty(202)          # a notification: acknowledged, nothing to say back
        else:
            self._reply_json(200, reply)

    def do_GET(self):
        self._reply_empty(405)              # server-initiated push not implemented; see class docstring


class _McpHTTPServer(http.server.ThreadingHTTPServer):
    """ThreadingHTTPServer that REFUSES an already-occupied port instead of quietly sharing it.

    allow_reuse_address defaults to True in socketserver, and on Windows that flag does not mean
    what it means on Unix: SO_REUSEADDR there permits a genuine second bind to a port another
    socket is already LISTENING on, rather than only reclaiming a TIME_WAIT one. So the default
    lets a second `aver_mcp.py --http --port N` start "successfully" beside a first -- two live
    servers, each possibly rooted at a DIFFERENT checkout, with the OS deciding which one a given
    connection reaches. That is precisely the wrong-tree confusion this whole change exists to
    remove, arrived at from the other end: not a build reported against the wrong root, but a
    client silently talking to the wrong server.

    Refusing costs nothing real. The port only matters while a server holds it, and a stale
    TIME_WAIT on a loopback listener clears in seconds; an operator who genuinely wants the port
    back should stop the process holding it, not race it.
    """
    allow_reuse_address = False


def serve_http(port):
    try:
        httpd = _McpHTTPServer((_HTTP_HOST, port), _McpHTTPHandler)
    except OSError as e:
        # A BOUND PORT IS AN OPERATOR MISTAKE, NOT A CRASH. Unhandled, this surfaced as a raw
        # WinError 10013 traceback and exit 1, which reads as a broken server rather than as
        # "something is already there". The editor's own control channel already answers this
        # situation by logging why and carrying on (McpBridge::start returns false); a tool server
        # has nothing to carry on WITH, so it exits -- but it exits saying what is wrong and how to
        # find the process holding the port.
        log("cannot serve HTTP on %s:%d -- %s" % (_HTTP_HOST, port, e))
        log("something is probably already listening there. Find it with: "
            "netstat -ano | findstr :%d   (then choose another port with --port, or in mcp.conf's "
            "tool_server.port)" % port)
        return 2
    log("serving from %s over http://%s:%d%s (Streamable HTTP; POST only, GET replies 405)"
        % (ROOT, _HTTP_HOST, port, _HTTP_PATH))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
    return 0


def main():
    argv = sys.argv[1:]
    port, source = resolve_tool_server_port(argv)
    if "--http" in argv:
        return serve_http(port)
    # Default path, exactly as before --http existed: stdio, no socket opened, nothing about this
    # process's behaviour on .mcp.json's existing stdio launch has changed.
    log("serving from %s over stdio (tool_server.port=%d, source=%s -- pass --http to serve over "
        "HTTP on that port instead)" % (ROOT, port, source))
    serve_stdio()
    return 0


if __name__ == "__main__":
    sys.exit(main())
