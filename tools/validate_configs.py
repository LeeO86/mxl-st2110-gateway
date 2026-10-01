#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validates every shipped gateway configuration against schema/gateway-config.schema.json.

Covers config/examples/*.json and the gateway.json seeds embedded in the Kubernetes ConfigMaps
under deploy/k8s (SPECIFICATION.md §16.1). Semantic rules (§9.5) are checked by the unit tests.

    python3 tools/validate_configs.py
"""

import json
import sys
from pathlib import Path

import jsonschema
import yaml

ROOT = Path(__file__).resolve().parents[1]


def documents():
    for path in sorted((ROOT / "config" / "examples").glob("*.json")):
        yield str(path.relative_to(ROOT)), json.loads(path.read_text(encoding="utf-8"))
    for path in sorted((ROOT / "deploy" / "k8s").rglob("*.yaml")):
        for doc in yaml.safe_load_all(path.read_text(encoding="utf-8")):
            if isinstance(doc, dict) and doc.get("kind") == "ConfigMap" and "gateway.json" in (doc.get("data") or {}):
                name = f"{path.relative_to(ROOT)} ConfigMap/{doc['metadata']['name']}"
                yield name, json.loads(doc["data"]["gateway.json"])


def main():
    schema = json.loads((ROOT / "schema" / "gateway-config.schema.json").read_text(encoding="utf-8"))
    validator_cls = jsonschema.validators.validator_for(schema)
    validator_cls.check_schema(schema)
    validator = validator_cls(schema, format_checker=validator_cls.FORMAT_CHECKER)
    failed = 0
    count = 0
    for name, doc in documents():
        count += 1
        errors = sorted(validator.iter_errors(doc), key=lambda e: list(e.absolute_path))
        if errors:
            failed += 1
            print(f"FAIL {name}")
            for e in errors:
                pointer = "/" + "/".join(str(p) for p in e.absolute_path)
                print(f"  {pointer}: {e.message}")
        else:
            print(f"ok   {name}")
    if count == 0:
        print("no configurations found", file=sys.stderr)
        return 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
