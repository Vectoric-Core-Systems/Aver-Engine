"""Proves that a comment-only edit changed no code.

A sweep that rewrites every file in the tree cannot be reviewed by reading it, and a green build only
says the code still compiles -- not that a constant in an untested path is the one that was there
before. This normalises both revisions of every source file down to their code alone (comments gone,
whitespace collapsed) and compares. A file that differs has had its CODE touched, whatever the diff
looks like.

    python scripts/check-code-unchanged.py [<git-ref>]      # default HEAD

Comments inside a C++ raw string are the interesting case: the HLSL this engine compiles at runtime
lives in R"(...)" literals, so `//` in there is string CONTENT, not a C++ comment. Removing it is
still a comment-only change -- DXC ignores it either way -- so raw-string bodies are normalised
recursively rather than treated as opaque.
"""
import subprocess, sys, re, os

EXTS = ('.c', '.cc', '.cpp', '.h', '.hpp', '.cs', '.rs', '.hlsl', '.hlsli')
SKIP = ('third_party/', 'modules/physics.jolt/')


def strip_comments(src, in_raw=False):
    """Return src with comments removed. String and char literals are preserved verbatim, except
    that C++ raw-string bodies are themselves stripped."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]

        # line comment
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                i += 1
            out.append('\n')
            continue

        # block comment
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            i += 2
            while i + 1 < n and not (src[i] == '*' and src[i + 1] == '/'):
                i += 1
            i += 2
            out.append(' ')
            continue

        # C++ raw string: R"delim( ... )delim"   (also u8R", LR", etc.)
        if not in_raw and c in 'Rr' and i + 1 < n and src[i + 1] == '"':
            m = re.match(r'R"([^\s()\\]{0,16})\(', src[i:])
            if m and (i == 0 or not (src[i - 1].isalnum() or src[i - 1] == '_') or
                      src[max(0, i - 2):i] in ('u8', 'LR')):
                delim = m.group(1)
                close = ')' + delim + '"'
                end = src.find(close, i + m.end())
                if end == -1:
                    end = n
                body = src[i + m.end():end]
                out.append('R"(')
                out.append(strip_comments(body, in_raw=True))
                out.append(')"')
                i = end + len(close)
                continue

        # C# verbatim string: @"..."  with "" as the escaped quote
        if c == '@' and i + 1 < n and src[i + 1] == '"':
            j = i + 2
            while j < n:
                if src[j] == '"':
                    if j + 1 < n and src[j + 1] == '"':
                        j += 2
                        continue
                    break
                j += 1
            out.append(src[i:j + 1])
            i = j + 1
            continue

        # ordinary string / char literal
        if c in '"\'':
            quote = c
            j = i + 1
            while j < n:
                if src[j] == '\\':
                    j += 2
                    continue
                if src[j] == quote or src[j] == '\n':
                    break
                j += 1
            out.append(src[i:j + 1])
            i = j + 1
            continue

        out.append(c)
        i += 1
    return ''.join(out)


def normalise(src):
    return ' '.join(strip_comments(src).split())


def git_show(ref, path):
    r = subprocess.run(['git', 'show', f'{ref}:{path}'], capture_output=True)
    return None if r.returncode else r.stdout.decode('utf-8', 'replace')


def main():
    ref = sys.argv[1] if len(sys.argv) > 1 else 'HEAD'
    files = subprocess.run(['git', 'ls-files'], capture_output=True, text=True,
                           encoding='utf-8').stdout.split('\n')
    checked = changed = added = 0
    for path in files:
        path = path.strip()
        if not path.endswith(EXTS) or any(s in path for s in SKIP):
            continue
        old = git_show(ref, path)
        if old is None:
            added += 1
            continue
        if not os.path.exists(path):
            print(f'DELETED  {path}')
            changed += 1
            continue
        with open(path, encoding='utf-8', errors='replace') as f:
            new = f.read()
        checked += 1
        a, b = normalise(old), normalise(new)
        if a != b:
            changed += 1
            # Where they first part company, which is what a reader needs to go and look at.
            k = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
            print(f'CODE CHANGED  {path}')
            print(f'    was: ...{a[max(0, k - 70):k + 90]}')
            print(f'    now: ...{b[max(0, k - 70):k + 90]}')
    print(f'\n{checked} files compared against {ref}, {added} new, {changed} with code changes')
    return 1 if changed else 0


if __name__ == '__main__':
    sys.exit(main())
