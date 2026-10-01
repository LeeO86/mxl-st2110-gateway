#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generates docs/configuration.md from schema/gateway-config.schema.json (SPECIFICATION.md §18).

    python3 tools/gen_config_docs.py           # regenerate
    python3 tools/gen_config_docs.py --check   # fail if the committed file is out of date

The environment variable names follow §9.1 (they mirror src/config/env.cpp; the unit test
"configuration reference lists every environment variable" keeps the two in sync).
"""

import argparse
import difflib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = ROOT / "schema" / "gateway-config.schema.json"
OUTPUT = ROOT / "docs" / "configuration.md"

ALIASES = {
    "node.http_port": ["MXLGW_HTTP_PORT"],
    "node.log_level": ["MXLGW_LOG_LEVEL"],
    "mxl.scan_path": ["MXL_DOMAIN_SCAN_PATH"],
    "mxl.default_read_offset_grains": ["MXL_READ_OFFSET_GRAINS"],
    "mxl.default_read_offset_ns": ["MXL_READ_OFFSET_MS (milliseconds)"],
}
NAMED_TYPES = {
    "uuid": "UUID",
    "ipv4": "IPv4 address",
    "port": "integer 1–65535",
    "absolutePath": "absolute path",
}


def resolve(schema, node):
    while isinstance(node, dict) and "$ref" in node and len([k for k in node if k not in ("$ref", "description", "default")]) == 0:
        target = schema["$defs"][node["$ref"].split("/")[-1]]
        merged = dict(target)
        for k in ("description", "default"):
            if k in node:
                merged[k] = node[k]
        node = merged
    return node


def type_text(schema, node):
    if node is True:
        return ""
    if "$ref" in node:
        name = node["$ref"].split("/")[-1]
        if name in NAMED_TYPES:
            return NAMED_TYPES[name]
        return type_text(schema, schema["$defs"][name])
    if "enum" in node:
        return " \\| ".join(f"`{json.dumps(v)}`" for v in node["enum"])
    if "const" in node:
        return f"`{json.dumps(node['const'])}`"
    if "anyOf" in node:
        parts = [type_text(schema, n) for n in node["anyOf"]]
        return " or ".join(p for p in parts if p)
    t = node.get("type")
    if isinstance(t, list):
        base = " or ".join(x for x in t)
    else:
        base = t or ""
    if base == "array" and "items" in node:
        inner = type_text(schema, node["items"])
        base = f"array of {inner}" if inner and "object" not in inner else "array"
    limits = []
    if "minimum" in node:
        limits.append(f"≥ {node['minimum']}")
    if "maximum" in node:
        limits.append(f"≤ {node['maximum']}")
    if "maxLength" in node:
        limits.append(f"≤ {node['maxLength']} chars")
    if "maxItems" in node:
        limits.append(f"≤ {node['maxItems']} items")
    if "pattern" in node and base == "string":
        limits.append(f"pattern `{node['pattern']}`")
    return base + (f" ({', '.join(limits)})" if limits else "")


def env_names(path):
    """Environment variables of a dotted path (§9.1), or [] for file-only settings."""
    parts = path.split(".")
    if parts[0] == "groups" or path == "nic.port_pairs[].name" or path in ("mxl.domains[].name", "nic.port_pairs", "mxl.domains"):
        return []
    if path.startswith("nic.port_pairs[]."):
        if len(parts) < 4:
            return []
        side, field = parts[2], parts[3]
        return [f"MXLGW_NIC_{side.upper()}_{field.upper()}"]
    if path.startswith("mxl.domains[]."):
        return [f"MXLGW_MXL_DOMAIN_<NAME>_{parts[2].upper()}"]
    if parts[0] in ("node", "nic", "ptp", "mxl"):
        return ["MXLGW_" + "_".join(p.upper() for p in parts)] + ALIASES.get(path, [])
    return []


def rows(schema, node, path, out):
    node = resolve(schema, node)
    props = node.get("properties", {})
    for name, child in props.items():
        common = schema["$defs"]["essenceCommon"]["properties"]
        if child is True:
            # declared in essenceCommon (allOf)
            child = common[name]
        elif isinstance(child, dict) and name in common and not any(k in child for k in ("type", "$ref", "enum", "const", "anyOf")):
            child = {**common[name], **child}
        child_path = f"{path}.{name}" if path else name
        resolved = resolve(schema, child)
        is_object = resolved.get("type") == "object" or "properties" in resolved
        is_object_array = resolved.get("type") == "array" and isinstance(resolved.get("items"), dict) and (
            "properties" in resolve(schema, resolved["items"]) or resolve(schema, resolved["items"]).get("type") == "object")
        if is_object and "properties" in resolved:
            rows(schema, resolved, child_path, out)
            continue
        if "anyOf" in resolved and any("$ref" in a for a in resolved["anyOf"]):
            ref = next(a for a in resolved["anyOf"] if "$ref" in a)
            target = resolve(schema, ref)
            if "properties" in target:
                out.append((child_path, "object or null", child.get("default", resolved.get("default")), resolved.get("description", ""), env_names(child_path)))
                rows(schema, target, child_path, out)
                continue
        if is_object_array:
            items = resolve(schema, resolved["items"])
            if "allOf" in items:
                merged = dict(items)
                merged["properties"] = dict(items["properties"])
                items = merged
            out.append((child_path, "array of objects", resolved.get("default"), resolved.get("description", ""), env_names(child_path)))
            rows(schema, items, child_path + "[]", out)
            continue
        default = child.get("default", resolved.get("default", "__none__")) if isinstance(child, dict) else "__none__"
        out.append((child_path, type_text(schema, child), default, resolved.get("description", child.get("description", "") if isinstance(child, dict) else ""), env_names(child_path)))


def fmt_default(value):
    if value == "__none__":
        return ""
    return f"`{json.dumps(value, ensure_ascii=False)}`"


def render():
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    sections = [
        ("node", "Node, HTTP and NMOS"),
        ("nic", "Media NIC"),
        ("ptp", "PTP and clock"),
        ("mxl", "MXL domains"),
        ("groups", "Groups and essences"),
    ]
    lines = [
        "# Configuration reference",
        "",
        "<!-- Generated by tools/gen_config_docs.py from schema/gateway-config.schema.json. Do not edit. -->",
        "",
        "The configuration file is `/config/gateway.json` (override the path with `MXLGW_CONFIG`). It is validated against",
        "`schema/gateway-config.schema.json` (also served at `/api/schema`) and the semantic rules of SPECIFICATION.md §9.5;",
        "an invalid file stops the gateway with exit code 78 and every error printed as JSON pointer + message.",
        "",
        "**Precedence: environment variable > file > default** for every scalar setting of `node`, `nic`, `ptp` and `mxl`",
        "(§9.1). Values are parsed by type (integers, `true`/`false`, comma lists, `null`); an invalid value is a configuration",
        "error naming the variable. Settings taken from the environment are read-only in the admin UI, rejected by `/api`",
        "and never written into the file. Groups and essences live in the file only. `MXLGW_LOG_FORMAT=text` switches the",
        "log output from JSON lines to text.",
        "",
        "In the tables, `[]` marks an array element. `MXLGW_MXL_DOMAIN_<NAME>_…` uses the upper-snake domain `name`",
        "(e.g. `main` → `MXLGW_MXL_DOMAIN_MAIN_PATH`).",
        "",
    ]
    top = schema["properties"]
    lines.append(f"`schema_version`: {type_text(schema, top['schema_version'])} — {top['schema_version'].get('description', '')}")
    lines.append("")
    for key, title in sections:
        out = []
        node = top[key]
        lines.append(f"## {title} (`{key}`)")
        lines.append("")
        lines.append(resolve(schema, node).get("description", node.get("description", "")))
        lines.append("")
        if key == "groups":
            rows(schema, {"properties": {"groups": node}}, "", out)
        else:
            rows(schema, node, key, out)
        lines.append("| Setting | Type | Default | Environment | Description |")
        lines.append("|---|---|---|---|---|")
        for path, typ, default, desc, env in out:
            env_text = "<br>".join(f"`{e}`" if "(" not in e else f"`{e.split(' (')[0]}` ({e.split(' (')[1]}" for e in env)
            desc = desc.replace("|", "\\|")
            lines.append(f"| `{path}` | {typ} | {fmt_default(default)} | {env_text} | {desc} |")
        lines.append("")
    lines += [
        "## Bootstrap variables",
        "",
        "| Variable | Meaning |",
        "|---|---|",
        "| `MXLGW_CONFIG` | configuration file path (default `/config/gateway.json`) |",
        "| `MXLGW_LOG_FORMAT` | `json` (default) or `text` |",
        "",
        "## Kubernetes PCI injection",
        "",
        "`nic.port_pairs[].primary.pci` / `redundant.pci` accept `env:VARIABLE`, e.g. `\"pci\": \"env:PCIDEVICE_INTEL_COM_E810_MEDIA_P\"`,",
        "resolved at start from the variable the SR-IOV device plugin injects (`deploy/k8s`).",
        "",
    ]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    text = render()
    if args.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
        if current != text:
            sys.stdout.writelines(difflib.unified_diff(current.splitlines(True), text.splitlines(True), str(OUTPUT), "generated", n=1))
            print(f"\n{OUTPUT} is out of date: run python3 tools/gen_config_docs.py", file=sys.stderr)
            return 1
        print(f"{OUTPUT} is up to date")
        return 0
    OUTPUT.write_text(text, encoding="utf-8")
    print(f"wrote {OUTPUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
