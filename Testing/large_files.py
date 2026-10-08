#!/usr/bin/env python3
"""Records the large, local-only data files of bench tests in a LARGE_FILES.md next to them.

Policy (Testing/README.md, "Data policy"): a data file above LIMIT_MB is NOT committed. It stays on the
machine that recorded it (and wherever the owner archives it), and the test folder gets a LARGE_FILES.md
that says what the file is, how big, and its SHA-256, so anybody can tell whether a copy they were handed
is the original. The summary/findings and the graphs are committed as always.

  python Testing/large_files.py                      # list large files of every test folder (no writes)
  python Testing/large_files.py --write <test_dir>...  # (re)write LARGE_FILES.md in the given test folders

A file is "large" if it is bigger than --limit-mb (default 5) and git does not track it. Caches
(__pycache__) are skipped. Existing descriptions in LARGE_FILES.md are kept when the file is rewritten:
edit the "What" column by hand, the size and hash are refreshed.
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def tracked_files():
    out = subprocess.run(["git", "ls-files", "-z", "Testing"], cwd=REPO, capture_output=True, check=True).stdout
    return {p for p in out.decode("utf-8", "replace").split("\0") if p}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def find_large(test_dir, limit_bytes, tracked):
    found = []
    for root, dirs, files in os.walk(test_dir):
        dirs[:] = [d for d in dirs if d != "__pycache__"]
        for name in files:
            full = os.path.join(root, name)
            rel = os.path.relpath(full, REPO).replace(os.sep, "/")
            if rel in tracked or name == "LARGE_FILES.md":
                continue
            size = os.path.getsize(full)
            if size > limit_bytes:
                found.append((os.path.relpath(full, test_dir).replace(os.sep, "/"), size))
    return sorted(found)


def old_descriptions(md_path):
    desc = {}
    if os.path.exists(md_path):
        for line in open(md_path, encoding="utf-8"):
            m = re.match(r"\|\s*`([^`]+)`\s*\|[^|]*\|[^|]*\|\s*(.*?)\s*\|\s*$", line)
            if m:
                desc[m.group(1)] = m.group(2)
    return desc


def write_md(test_dir, items):
    md = os.path.join(test_dir, "LARGE_FILES.md")
    old = old_descriptions(md)
    lines = [
        "# Large data files (not in git)\n",
        "\n",
        "These files are bigger than the repository's per-file limit for test data (Testing/README.md, \"Data policy\")\n",
        "and live only on the machine that recorded them. The size and SHA-256 identify the original; edit the\n",
        "\"What\" column by hand. Regenerate sizes/hashes with `python Testing/large_files.py --write <this folder>`.\n",
        "\n",
        "| File | Size | SHA-256 | What |\n",
        "|---|---|---|---|\n",
    ]
    for rel, size in items:
        print(f"  hashing {rel} ({size / 1e6:.1f} MB) ...", flush=True)
        digest = sha256(os.path.join(test_dir, rel))
        what = old.get(rel, "(describe)")
        lines.append(f"| `{rel}` | {size} B ({size / 1e6:.1f} MB) | `{digest}` | {what} |\n")
    with open(md, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(lines)
    print("wrote", os.path.relpath(md, REPO))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dirs", nargs="*", help="test folders (default: all under Testing/)")
    ap.add_argument("--write", action="store_true", help="write LARGE_FILES.md instead of just listing")
    ap.add_argument("--limit-mb", type=float, default=5.0)
    a = ap.parse_args()
    limit = int(a.limit_mb * 1e6)
    tracked = tracked_files()
    if a.dirs:
        dirs = [os.path.abspath(d) for d in a.dirs]
    else:
        dirs = [os.path.join(HERE, d) for d in sorted(os.listdir(HERE)) if os.path.isdir(os.path.join(HERE, d))]
    status = 0
    for d in dirs:
        items = find_large(d, limit, tracked)
        if not items:
            continue
        name = os.path.basename(d.rstrip("/\\"))
        print(f"{name}: {len(items)} large file(s)")
        for rel, size in items:
            print(f"    {size / 1e6:9.1f} MB  {rel}")
        if a.write:
            write_md(d, items)
        elif not os.path.exists(os.path.join(d, "LARGE_FILES.md")):
            status = 1   # a large local-only file without a manifest
    if status and not a.write:
        print("\nSome folders have large local-only files but no LARGE_FILES.md (run with --write).")
    return status


if __name__ == "__main__":
    sys.exit(main())
