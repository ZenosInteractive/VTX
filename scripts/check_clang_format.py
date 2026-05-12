#!/usr/bin/env python3
# Local mirror of the clang-format diff-gate that runs in CI
# (.github/workflows/build.yml).  Validates ONLY the lines you've
# changed vs a base ref; the rest of the file (legacy, possibly
# pre-.clang-format) is left alone.
#
# Usage:
#   python scripts/check_clang_format.py                  # check vs origin/main
#   python scripts/check_clang_format.py --base HEAD~1    # check vs previous commit
#   python scripts/check_clang_format.py --fix            # apply formatting in place
#
# Exit codes:
#   0  -- clean (or nothing in scope)
#   1  -- violations found (CI would fail)
#   2  -- tooling missing / environment problem

import argparse
import os
import re
import shutil
import subprocess
import sys


EXCLUDE_PATTERNS = [
    re.compile(r"^thirdparty/"),
    re.compile(r".*generated/"),
    re.compile(r".*arena_generated\.h$"),
    re.compile(r".*portable-file-dialogs\.h$"),
]

INCLUDE_EXTENSIONS = (".cpp", ".cc", ".h", ".hpp")


def find_clang_format_diff():
    candidates = [
        "clang-format-diff",
        "clang-format-diff-15",
        "clang-format-diff-16",
        "clang-format-diff-17",
        "clang-format-diff-18",
    ]
    for c in candidates:
        path = shutil.which(c)
        if path:
            return [path]

    # Windows LLVM installs ship clang-format-diff.py under share/clang/
    # but never on PATH.  Search the common locations.
    if os.name == "nt":
        for root in (
            os.environ.get("ProgramFiles", r"C:\Program Files"),
            os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
        ):
            if not root:
                continue
            for sub in ("LLVM", "LLVM/share/clang"):
                candidate = os.path.join(root, sub, "clang-format-diff.py")
                if os.path.isfile(candidate):
                    py = shutil.which("python") or shutil.which("python3") or sys.executable
                    return [py, candidate]
    return None


def changed_files(base_ref):
    try:
        out = subprocess.check_output(
            ["git", "diff", "--name-only", "--diff-filter=ACM", base_ref, "HEAD"],
            text=True,
        ).strip()
    except subprocess.CalledProcessError as e:
        print(f"[ERROR] git diff failed (is '{base_ref}' a valid ref?): {e}", file=sys.stderr)
        print("        Hint: git fetch origin", file=sys.stderr)
        sys.exit(2)

    if not out:
        return []
    files = []
    for line in out.split("\n"):
        if not line.endswith(INCLUDE_EXTENSIONS):
            continue
        if any(p.search(line) for p in EXCLUDE_PATTERNS):
            continue
        files.append(line)
    return files


def run_format_diff(base_ref, files, fmt_diff_cmd, in_place):
    git_diff = subprocess.check_output(
        ["git", "diff", "-U0", base_ref, "HEAD", "--"] + files,
        text=True,
    )
    args = list(fmt_diff_cmd) + ["-p1", "-style=file"]
    if in_place:
        args.append("-i")
    result = subprocess.run(args, input=git_diff, capture_output=True, text=True)
    return result.stdout, result.stderr, result.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base", default="origin/main",
                        help="Base ref to diff against (default: origin/main)")
    parser.add_argument("--fix", action="store_true",
                        help="Apply the formatting fix in place (rather than just reporting)")
    args = parser.parse_args()

    if not shutil.which("clang-format"):
        print("[ERROR] clang-format not found in PATH.", file=sys.stderr)
        print("        Install: apt install clang-format-15 / brew install clang-format / LLVM Windows installer",
              file=sys.stderr)
        return 2

    fmt_diff = find_clang_format_diff()
    if not fmt_diff:
        print("[ERROR] clang-format-diff not found.", file=sys.stderr)
        print("        On Linux/macOS it ships with clang-format.  On Windows, LLVM installs", file=sys.stderr)
        print("        share/clang/clang-format-diff.py -- this script will pick it up automatically.", file=sys.stderr)
        return 2

    files = changed_files(args.base)
    if not files:
        print(f"[OK] No in-scope C++ files changed vs {args.base}.")
        return 0

    print(f"Diffing against: {args.base}")
    print("Files in scope:")
    for f in files:
        print(f"  {f}")
    print()

    stdout, stderr, rc = run_format_diff(args.base, files, fmt_diff, args.fix)

    if args.fix:
        if rc != 0:
            print(stderr, file=sys.stderr)
            return 2
        print("[OK] Applied clang-format fixes to changed lines.")
        return 0

    if stdout.strip():
        print("[FAIL] Formatting violations on changed lines:")
        print()
        print(stdout)
        print()
        print("Fix: rerun with --fix")
        print(f"  python {os.path.relpath(__file__)} --fix")
        return 1

    print(f"[OK] All changed lines conform to .clang-format.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
