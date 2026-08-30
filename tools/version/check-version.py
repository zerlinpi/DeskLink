#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VERSION_PATTERN = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z][0-9A-Za-z.-]*))?$")


def fail(message: str) -> None:
    raise SystemExit(f"version check failed: {message}")


def load_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"unable to read {path.relative_to(ROOT)}: {exc}")
    if not isinstance(value, dict):
        fail(f"{path.relative_to(ROOT)} must contain a JSON object")
    return value


def main() -> None:
    parser = argparse.ArgumentParser(description="Validate DeskLink's single VERSION source")
    parser.add_argument("--require-stable", action="store_true")
    parser.add_argument("--tag", default="", help="optional release tag that must equal v<VERSION>")
    args = parser.parse_args()

    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    match = VERSION_PATTERN.fullmatch(version)
    if not match:
        fail("VERSION must be major.minor.patch with an optional prerelease suffix")

    prerelease = match.group(4) or ""
    if args.require_stable and prerelease:
        fail(f"stable release cannot use prerelease VERSION {version!r}")
    if args.tag and args.tag != f"v{version}":
        fail(f"tag {args.tag!r} does not match VERSION v{version}")

    package = load_json(ROOT / "apps/web/package.json")
    if package.get("version") != version:
        fail(
            f"apps/web/package.json version {package.get('version')!r} "
            f"does not match VERSION {version!r}"
        )

    lock = load_json(ROOT / "apps/web/package-lock.json")
    if lock.get("version") != version:
        fail(
            f"apps/web/package-lock.json version {lock.get('version')!r} "
            f"does not match VERSION {version!r}"
        )
    root_package = lock.get("packages", {}).get("") if isinstance(lock.get("packages"), dict) else None
    if not isinstance(root_package, dict) or root_package.get("version") != version:
        fail("apps/web/package-lock.json root package version does not match VERSION")

    if not prerelease:
        notes = ROOT / f"docs/releases/v{version}.md"
        if not notes.is_file():
            fail(f"stable VERSION requires release notes at {notes.relative_to(ROOT)}")

    print(version)


if __name__ == "__main__":
    main()
