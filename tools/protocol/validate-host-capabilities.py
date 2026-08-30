#!/usr/bin/env python3
from __future__ import annotations

import json
from pathlib import Path

from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parents[2]
SCHEMA = ROOT / "packages/protocol/host-capabilities-v1.schema.json"
FIXTURE = ROOT / "apps/web/src/fixtures/host-capabilities-v1.windows.json"


def load_json(path: Path) -> object:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def main() -> None:
    schema = load_json(SCHEMA)
    fixture = load_json(FIXTURE)
    Draft202012Validator.check_schema(schema)
    validator = Draft202012Validator(schema)
    errors = sorted(validator.iter_errors(fixture), key=lambda error: list(error.absolute_path))
    if errors:
        for error in errors:
            location = ".".join(str(value) for value in error.absolute_path) or "<root>"
            print(f"HostCapabilitiesV1 schema violation at {location}: {error.message}")
        raise SystemExit(1)
    print("Windows HostCapabilitiesV1 fixture conforms to protocol schema.")


if __name__ == "__main__":
    main()
