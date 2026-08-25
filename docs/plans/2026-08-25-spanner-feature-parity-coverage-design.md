# Spanner feature parity coverage design

**Date:** 2026-08-25  
**Status:** Approved for implementation  
**Audience:** Emulator maintainers and contributors

## 1. Purpose

Create an evidence-based, maintainable view of this emulator's parity with the documented Google Cloud Spanner product surface. The coverage system must answer:

1. Which documented Spanner capabilities exist?
2. Which are fully emulated, partially emulated, accepted without behavior, unsupported, or not applicable locally?
3. What source code and tests justify each classification?
4. When and against which documentation baseline was the classification reviewed?
5. What should be implemented or verified next?

The inventory covers the entire documented Spanner surface. Cloud-only operational capabilities remain visible and are classified as not applicable when local emulation would not be meaningful.

## 2. Scope

### Included

- Spanner data and admin RPC/REST API methods
- GoogleSQL and PostgreSQL dialects
- SQL lexical, query, expression, function, DDL, and DML capabilities
- Data types and schema objects
- Reads, mutations, transactions, partitioning, and batching
- Information schema and `SPANNER_SYS`
- Change streams
- Full-text search and search indexes
- Vector search and approximate nearest-neighbor indexes
- Spanner Graph and GQL
- ML integration and model schema objects
- Instance, database, backup, operation, and session administration
- Security, IAM, encryption, observability, quotas, replication, availability, and other cloud operations
- Client and driver compatibility
- Emulator-specific capabilities such as local persistence and packaging

### Excluded

- Performance parity with the distributed production service
- Claims about undocumented internal Spanner behavior
- Automatically asserting semantic parity from the existence of a parser or handler alone
- Reproducing the full official documentation in this repository

## 3. Architecture

The coverage system has three layers.

### 3.1 Canonical inventory

`docs/feature-coverage.yaml` is the source of truth. It contains metadata, categories, and stable feature records. Records are intentionally small enough to review in pull requests and rich enough to support generated documentation and future automation.

Top-level metadata records:

- schema version
- inventory review date
- official documentation baseline
- upstream emulator revision
- GoogleSQL dependency version when known
- status and evidence legends

Each feature record contains:

```yaml
- id: query.googlesql.select
  category: query-language
  feature: SELECT queries
  status: supported
  applicability: local-development
  dialects: [googlesql, postgresql]
  docs:
    - https://docs.cloud.google.com/spanner/docs/reference/standard-sql/query-syntax
  evidence:
    implementation:
      - backend/query/query_engine.cc
    tests:
      - tests/conformance/cases/query.cc
  verification: tested
  notes: Core query behavior is covered by the conformance suite.
```

Required fields are `id`, `category`, `feature`, `status`, `docs`, `evidence`, `verification`, and `notes`. Empty evidence lists are allowed for unsupported, unknown, and not-applicable records.

Stable IDs use lowercase dotted names. Existing IDs must not be renamed casually because issues and future tooling may refer to them.

### 3.2 Generated reference

`docs/feature-coverage.md` is generated from the YAML inventory. It provides:

- scope and interpretation guidance
- status and evidence legends
- review baseline and date
- aggregate counts by status
- one table per MECE feature category
- official documentation links
- concise notes and repository evidence links
- a section explaining how to update the inventory

The top-level taxonomy follows official Spanner documentation and API structure:

1. Data API and sessions
2. Instance administration
3. Database administration, operations, and backups
4. Schema and DDL
5. Types and values
6. GoogleSQL query language and functions
7. PostgreSQL dialect
8. DML, mutations, and batch operations
9. Transactions, timestamps, and concurrency
10. Indexes, generated values, and constraints
11. Change streams
12. Full-text search
13. Vector search
14. Spanner Graph and GQL
15. ML and external integrations
16. Metadata, information schema, and `SPANNER_SYS`
17. Security and governance
18. Operations, replication, availability, and observability
19. Clients, drivers, and tooling
20. Emulator-specific capabilities

Categories are mutually exclusive at the tracked feature level and collectively cover the documented product surface. Large surfaces, especially functions and RPC methods, may use subcategories while remaining within one top-level category.

### 3.3 Validator and generator

`tools/feature_coverage.py` uses only the Python standard library except for YAML parsing. To avoid adding an undeclared dependency, the canonical file may use the JSON-compatible subset of YAML and be parsed with Python's `json` module, or the repository may explicitly declare PyYAML. The preferred implementation is JSON-compatible YAML so validation works in existing development and CI environments without installation steps.

Commands:

```bash
python3 tools/feature_coverage.py validate
python3 tools/feature_coverage.py generate
python3 tools/feature_coverage.py check
python3 tools/feature_coverage.py summary
```

- `validate` checks schema and repository evidence.
- `generate` writes deterministic Markdown.
- `check` validates and fails if generated output differs from the committed file.
- `summary` prints status totals for maintainers and CI logs.

## 4. Classification model

### Status

| Status | Meaning |
| --- | --- |
| `supported` | Behavior needed for local development is implemented and has representative evidence. |
| `partial` | A useful subset works, but documented semantics or variants are missing. |
| `accepted-no-op` | Syntax or API input is accepted for compatibility, but its documented behavior is intentionally not performed. |
| `unsupported` | Applicable to local development but not implemented. |
| `not-applicable` | Production-only behavior has no meaningful local emulator equivalent. |
| `unknown` | The audit did not find enough evidence to classify the feature safely. |

`accepted-no-op` is separate from `partial` because accepting a hint, option, graph algorithm, or search parameter without executing its behavior is important compatibility information.

### Verification

| Verification | Meaning |
| --- | --- |
| `tested` | A repository test exercises the public or representative behavior. |
| `implemented` | Implementation evidence exists, but representative behavior was not confirmed by the audit. |
| `documented` | Only project documentation supports the claim. |
| `unverified` | No reliable repository evidence was identified. |

Status and verification are independent. For example, an unsupported feature can be `tested` when a conformance test verifies the expected unimplemented response.

### Applicability

Records may identify `local-development`, `compatibility-only`, or `cloud-only`. Applicability helps users distinguish product completeness from useful emulator completeness.

## 5. Evidence and audit rules

1. A parser production or generated API stub alone does not prove support.
2. `supported` normally requires a representative test. Exceptions must explain why implementation evidence is sufficient.
3. A source TODO or rejection path is evidence for `unsupported` or `partial`, not support.
4. Accepted-but-ignored options are `accepted-no-op` and must name the ignored behavior.
5. Cloud-only systems such as global replication, SLA guarantees, managed monitoring, and physical infrastructure controls are normally `not-applicable`.
6. API resources and methods are tracked separately when their support differs.
7. GoogleSQL and PostgreSQL are classified separately where behavior differs.
8. Evidence paths must exist in the repository. Optional line anchors are descriptive and are not validated because line numbers drift.
9. Official links must use HTTPS and point to Spanner documentation or API references unless the feature is emulator-specific.
10. Unknown classifications are valid baseline findings and must not be converted to support based on inference.

## 6. Initial audit strategy

The first inventory is a broad baseline, not a claim that every individual SQL function has already been behaviorally compared with production.

The audit will seed records from:

- official Spanner documentation navigation and reference pages
- Spanner RPC/REST service definitions
- frontend handlers and server registration
- DDL parser and schema updater coverage
- GoogleSQL language options and supported-function filters
- PostgreSQL parser, translator, catalog, and conformance tests
- `tests/conformance/cases`, schema-change test data, and component tests
- existing README feature and limitation claims
- fork-specific persistence, backup, change-stream, packaging, and compatibility work

Fine-grained areas can initially contain explicit `unknown` records. This prevents overclaiming and creates a visible verification backlog.

## 7. Documentation integration

The README retains a concise overview. Its feature section will link to the generated matrix as the authoritative source rather than duplicating a long, drifting list.

Known stale or contradictory README claims must be reconciled against code and tests. In particular:

- persistence is implemented by this fork, so the inherited statement that all data is discarded must be qualified or removed
- backup handlers and persistence code exist, so the blanket statement that Backup APIs are unsupported must be replaced with the audited per-method classification
- accepted-but-ignored graph, optimizer, and search behaviors must be described as partial or accepted-no-op rather than simply supported

## 8. Validation and CI

The validator rejects:

- duplicate or malformed IDs
- unknown status, verification, applicability, category, or dialect values
- missing required fields
- missing implementation or test evidence paths
- non-HTTPS documentation links
- unsupported and not-applicable records without explanatory notes
- supported records with no evidence
- stale generated Markdown

Unit tests use Python's standard `unittest` framework and temporary fixture files. Tests cover successful generation and each important validation failure.

The existing Docker workflow should not become responsible for every documentation check. A small dedicated GitHub Actions workflow will run the unit tests and `check` command on relevant pull requests and pushes. Local validation remains a single command.

## 9. Maintenance workflow

When adding or changing a Spanner feature:

1. Add or update implementation and representative tests.
2. Update the canonical inventory in the same change.
3. Run `python3 tools/feature_coverage.py generate`.
4. Run the tool's unit tests and `check`.
5. Review status, notes, official source, and evidence together.

Periodic parity reviews compare the inventory with:

- Spanner release notes
- official documentation navigation and reference pages
- current API service definitions
- GoogleSQL dependency upgrades
- upstream Cloud Spanner emulator changes

The inventory's `last_reviewed` and baseline fields are updated only during such a review, not for an isolated feature edit.

## 10. Future automation

The initial implementation validates manually curated records. Later work may add importers that compare the inventory against:

- protobuf service methods
- REST discovery resources and methods
- supported GoogleSQL function catalogs
- DDL parser statement types
- conformance test labels

Generated discoveries should report missing inventory entries rather than silently modifying classifications. Human review remains required because code presence does not establish semantic parity.

## 11. Success criteria

- The repository has one canonical, reviewable feature inventory.
- The generated matrix covers all major documented Spanner feature areas, including cloud-only areas.
- Every classification links to official documentation and records its evidence quality.
- Validation detects malformed records, missing evidence files, and stale generated output.
- README claims no longer contradict implemented persistence or audited API behavior.
- Maintainers have a documented update workflow and CI prevents accidental drift.
