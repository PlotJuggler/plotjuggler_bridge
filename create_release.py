#!/usr/bin/env python3
"""Bump version in package.xml and conda.recipe/recipe.yaml, then commit."""

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent

VERSION_FILES = {
    REPO / "package.xml": (
        re.compile(r"(<version>)([\d.]+)(</version>)"),
        r"\g<1>{version}\g<3>",
    ),
    REPO / "conda.recipe/recipe.yaml": (
        re.compile(r'(version:\s*")[\d.]+(")'),
        r'\g<1>{version}\2',
    ),
}


def current_version() -> str:
    text = (REPO / "package.xml").read_text()
    m = re.search(r"<version>([\d.]+)</version>", text)
    if not m:
        sys.exit("Cannot read current version from package.xml")
    return m.group(1)


def update_file(path: Path, pattern: re.Pattern, replacement: str, version: str):
    text = path.read_text()
    new_text, n = pattern.subn(replacement.format(version=version), text)
    if n == 0:
        sys.exit(f"Version pattern not found in {path.relative_to(REPO)}")
    path.write_text(new_text)
    print(f"  {path.relative_to(REPO)}: updated to {version}")


def main():
    cur = current_version()
    parser = argparse.ArgumentParser(description="Bump project version and commit.")
    parser.add_argument("version", help=f"New version (current: {cur})")
    parser.add_argument(
        "--no-commit", action="store_true", help="Update files without committing"
    )
    args = parser.parse_args()

    new = args.version
    if new == cur:
        sys.exit(f"Version is already {cur}")

    print(f"Bumping {cur} → {new}")
    for path, (pattern, repl) in VERSION_FILES.items():
        update_file(path, pattern, repl, new)

    if args.no_commit:
        return

    files = [str(p) for p in VERSION_FILES]
    subprocess.run(["git", "add", *files], check=True, cwd=REPO)
    subprocess.run(
        ["git", "commit", "-m", new],
        check=True,
        cwd=REPO,
    )
    print(f"\nCommitted as '{new}'. Review with: git log --oneline -1")


if __name__ == "__main__":
    main()
