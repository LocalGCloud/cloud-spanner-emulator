#!/usr/bin/env python3
"""Validate and render the Spanner feature coverage inventory."""
from __future__ import annotations

import argparse, collections, copy, json, re, sys
from pathlib import Path
from typing import Any, Iterable

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_INVENTORY = ROOT / "docs" / "feature-coverage.yaml"
DEFAULT_MARKDOWN = ROOT / "docs" / "feature-coverage.md"
STATUSES = {
    "supported": "Behavior needed for local development is implemented and has representative evidence.",
    "partial": "A useful subset works, but documented semantics or variants are missing.",
    "accepted-no-op": "Syntax or API input is accepted for compatibility, but documented behavior is intentionally not performed.",
    "unsupported": "Applicable to local development but not implemented.",
    "not-applicable": "Production-only behavior has no meaningful local emulator equivalent.",
    "unknown": "The audit did not find enough evidence to classify the feature safely.",
}
VERIFICATIONS = {
    "tested": "A repository test exercises the public or representative behavior.",
    "implemented": "Implementation evidence exists, but representative behavior was not confirmed by the audit.",
    "documented": "Only project documentation supports the claim.",
    "unverified": "No reliable repository evidence was identified.",
}
APPLICABILITIES = {"local-development", "compatibility-only", "cloud-only"}
DIALECTS = {"googlesql", "postgresql", "api", "emulator", "all"}
ID_RE = re.compile(r"^[a-z0-9]+(?:[._-][a-z0-9]+)*$")
REQUIRED_FEATURE_FIELDS = {"id", "category", "feature", "status", "docs", "evidence", "verification", "notes"}

class CoverageError(Exception): pass

def load_inventory(path: Path) -> dict[str, Any]:
    try:
        with path.open(encoding="utf-8") as fh: data = json.load(fh)
    except json.JSONDecodeError as exc:
        raise CoverageError(f"{path}: inventory must be JSON-compatible YAML: {exc}") from exc
    except OSError as exc:
        raise CoverageError(f"{path}: cannot read inventory: {exc}") from exc
    if not isinstance(data, dict): raise CoverageError("inventory root must be an object")
    return data

def category_map(data: dict[str, Any]) -> dict[str, dict[str, Any]]:
    cats = data.get("categories")
    if not isinstance(cats, list) or not cats: raise CoverageError("categories must be a non-empty list")
    seen = {}
    for i, cat in enumerate(cats):
        if not isinstance(cat, dict): raise CoverageError(f"categories[{i}] must be an object")
        name = cat.get("name")
        cid = cat.get("id", name)
        if not isinstance(cid, str) or not cid.strip(): raise CoverageError(f"categories[{i}].id is missing or malformed")
        if "id" in cat and not ID_RE.match(cid): raise CoverageError(f"categories[{i}].id is missing or malformed")
        if not isinstance(name, str) or not name.strip(): raise CoverageError(f"category {cid} must have a non-empty name")
        if cid in seen: raise CoverageError(f"duplicate category id: {cid}")
        item = dict(cat); item.setdefault("id", cid)
        seen[cid] = item
    return seen

def iter_features(data: dict[str, Any]) -> list[dict[str, Any]]:
    if isinstance(data.get("features"), list): return data["features"]
    features = []
    for cat in data.get("categories", []):
        if isinstance(cat, dict) and isinstance(cat.get("features"), list):
            for feature in cat["features"]:
                item = copy.deepcopy(feature); item.setdefault("category", cat.get("id")); features.append(item)
    return features

def _repo_path(raw: str) -> str:
    no_anchor = raw.split("#", 1)[0]
    if re.match(r"^[A-Za-z]:", no_anchor): return no_anchor
    return no_anchor.split(":", 1)[0]

def validate(data: dict[str, Any], root: Path = ROOT) -> list[dict[str, Any]]:
    metadata = data.get("metadata")
    if not isinstance(metadata, dict): raise CoverageError("metadata must be an object")
    required_any = (("schema_version",), ("last_reviewed", "inventory_review_date"), ("documentation_baseline", "official_documentation_baseline"))
    for aliases in required_any:
        if not any(metadata.get(field) for field in aliases):
            raise CoverageError(f"metadata.{aliases[0]} is required")
    cats, features = category_map(data), iter_features(data)
    if not features: raise CoverageError("features must be a non-empty list, either top-level or inside categories")
    seen = set()
    for i, f in enumerate(features):
        if not isinstance(f, dict): raise CoverageError(f"features[{i}] must be an object")
        missing = sorted(REQUIRED_FEATURE_FIELDS - f.keys())
        if missing: raise CoverageError(f"features[{i}] missing required fields: {', '.join(missing)}")
        fid = f["id"]
        if not isinstance(fid, str) or not ID_RE.match(fid): raise CoverageError(f"feature id is missing or malformed at index {i}")
        if fid in seen: raise CoverageError(f"duplicate feature id: {fid}")
        seen.add(fid)
        if f["category"] not in cats: raise CoverageError(f"{fid}: unknown category {f['category']!r}")
        if not isinstance(f["feature"], str) or not f["feature"].strip(): raise CoverageError(f"{fid}: feature must be non-empty text")
        if f["status"] not in STATUSES: raise CoverageError(f"{fid}: unknown status {f['status']!r}")
        if f["verification"] not in VERIFICATIONS: raise CoverageError(f"{fid}: unknown verification {f['verification']!r}")
        if f.get("applicability") is not None and f["applicability"] not in APPLICABILITIES: raise CoverageError(f"{fid}: unknown applicability {f['applicability']!r}")
        dialects = f.get("dialects", [])
        if dialects is not None and (not isinstance(dialects, list) or any(d not in DIALECTS for d in dialects)): raise CoverageError(f"{fid}: dialects must use known values")
        docs = f["docs"]
        if not isinstance(docs, list): raise CoverageError(f"{fid}: docs must be a list")
        for url in docs:
            if not isinstance(url, str) or not url.startswith("https://"): raise CoverageError(f"{fid}: documentation links must be HTTPS")
            if "spanner" not in url and f.get("category") != "emulator-specific": raise CoverageError(f"{fid}: documentation links must reference Spanner unless emulator-specific")
        evidence = f["evidence"]
        if not isinstance(evidence, dict): raise CoverageError(f"{fid}: evidence must be an object")
        evidence_paths = []
        for key in ("implementation", "tests", "documentation"):
            vals = evidence.get(key, []) or []
            if not isinstance(vals, list) or any(not isinstance(v, str) or not v for v in vals): raise CoverageError(f"{fid}: evidence.{key} must be a list of paths")
            evidence_paths.extend(vals)
        for raw in evidence_paths:
            path = (root / _repo_path(raw)).resolve()
            try: path.relative_to(root.resolve())
            except ValueError as exc: raise CoverageError(f"{fid}: evidence path escapes repository: {raw}") from exc
            if not path.exists(): raise CoverageError(f"{fid}: evidence path does not exist: {raw}")
        if f["status"] == "supported" and not evidence_paths: raise CoverageError(f"{fid}: supported records require evidence")
        if not isinstance(f["notes"], str): raise CoverageError(f"{fid}: notes must be text")
        if f["status"] in {"unsupported", "not-applicable"} and not f["notes"].strip(): raise CoverageError(f"{fid}: {f['status']} records require explanatory notes")
    return features

def md_escape(text: Any) -> str: return str(text).replace("\n", " ").strip().replace("|", "\\|")
def link_list(items: Iterable[str], repo_relative: bool = False) -> str:
    vals = list(items)
    if not vals: return "—"
    return "<br>".join(f"[{md_escape(x)}]({_repo_path(x) if repo_relative else x})" for x in vals)

def render_markdown(data: dict[str, Any], features: list[dict[str, Any]] | None = None) -> str:
    features = validate(data) if features is None else features
    cats, counts = category_map(data), collections.Counter(f["status"] for f in features)
    by_cat = collections.defaultdict(list)
    for f in sorted(features, key=lambda x: x["id"]): by_cat[f["category"]].append(f)
    m = data["metadata"]
    lines = ["# Spanner emulator feature coverage", "", "<!-- Generated by tools/feature_coverage.py. Do not edit by hand. -->", "", "## Baseline", "", f"- Schema version: {m.get('schema_version')}", f"- Last reviewed: {m.get('last_reviewed')}", f"- Documentation baseline: {m.get('documentation_baseline')}"]
    for key in ("upstream_emulator_revision", "googlesql_dependency_version"):
        if m.get(key): lines.append(f"- {key.replace('_', ' ').title()}: {m[key]}")
    lines += ["", "## Status legend", "", "| Status | Meaning |", "| --- | --- |"]
    lines += [f"| `{k}` | {md_escape(v)} |" for k, v in STATUSES.items()]
    lines += ["", "## Verification legend", "", "| Verification | Meaning |", "| --- | --- |"]
    lines += [f"| `{k}` | {md_escape(v)} |" for k, v in VERIFICATIONS.items()]
    lines += ["", "## Summary", "", "| Status | Count |", "| --- | ---: |"]
    lines += [f"| `{k}` | {counts.get(k, 0)} |" for k in STATUSES]
    lines += [f"| **Total** | **{len(features)}** |", "", "## Feature matrix"]
    for cid, cat in cats.items():
        lines += ["", f"### {cat['name']}", ""]
        if cat.get("description"): lines += [str(cat["description"]).strip(), ""]
        lines += ["| ID | Feature | Status | Applicability | Dialects | Docs | Evidence | Verification | Notes |", "| --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
        for f in by_cat.get(cid, []):
            ev = f.get("evidence", {}); paths = []
            for key in ("implementation", "tests", "documentation"): paths.extend(ev.get(key) or [])
            row = [f"`{md_escape(f['id'])}`", md_escape(f["feature"]), f"`{f['status']}`", f"`{f['applicability']}`" if f.get("applicability") else "—", ", ".join(f"`{d}`" for d in f.get("dialects", [])) or "—", link_list(f.get("docs", [])), link_list(paths, True), f"`{f['verification']}`", md_escape(f.get("notes", "")) or "—"]
            lines.append("| " + " | ".join(row) + " |")
    lines += ["", "## Updating this file", "", "1. Edit `docs/feature-coverage.yaml`.", "2. Run `python3 tools/feature_coverage.py generate`.", "3. Run `python3 tools/feature_coverage.py check` and the unit tests.", ""]
    return "\n".join(lines)

def cmd_validate(args): print(f"Validated {len(validate(load_inventory(Path(args.inventory)), Path(args.root)))} feature records."); return 0
def cmd_generate(args):
    data = load_inventory(Path(args.inventory)); features = validate(data, Path(args.root)); out = Path(args.output); out.parent.mkdir(parents=True, exist_ok=True); out.write_text(render_markdown(data, features), encoding="utf-8"); print(f"Wrote {out}"); return 0
def cmd_check(args):
    data = load_inventory(Path(args.inventory)); features = validate(data, Path(args.root)); expected = render_markdown(data, features); out = Path(args.output); actual = out.read_text(encoding="utf-8") if out.exists() else ""
    if actual != expected: raise CoverageError(f"{out}: generated Markdown is stale; run tools/feature_coverage.py generate")
    print(f"{out} is up to date."); return 0
def cmd_summary(args):
    features = validate(load_inventory(Path(args.inventory)), Path(args.root)); counts = collections.Counter(f["status"] for f in features)
    for s in STATUSES: print(f"{s}: {counts.get(s, 0)}")
    print(f"total: {len(features)}"); return 0

def build_parser():
    p = argparse.ArgumentParser(description=__doc__); p.add_argument("--inventory", default=str(DEFAULT_INVENTORY)); p.add_argument("--output", default=str(DEFAULT_MARKDOWN)); p.add_argument("--root", default=str(ROOT)); sub = p.add_subparsers(dest="command", required=True)
    for name, func in (("validate", cmd_validate), ("generate", cmd_generate), ("check", cmd_check), ("summary", cmd_summary)):
        sp = sub.add_parser(name); sp.set_defaults(func=func)
    return p

def main(argv=None):
    args = build_parser().parse_args(argv)
    try: return args.func(args)
    except CoverageError as exc: print(f"error: {exc}", file=sys.stderr); return 1
if __name__ == "__main__": raise SystemExit(main())
