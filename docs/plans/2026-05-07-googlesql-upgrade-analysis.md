# GoogleSQL Upgrade Analysis: 2025.09.1 → 2026.01.1

## Current State

- **Version**: googlesql 2025.09.1 (Sep 15, 2025)
- **Latest**: googlesql 2026.01.1 (Jan 30, 2026)
- **Intermediate**: 2025.11.1, 2025.11.2, 2025.12.1
- **Build system**: Bazel with WORKSPACE (not Bzlmod)
- **Patch file**: `build/bazel/zetasql.patch` (~40+ patches)
- **Migration guide**: https://github.com/google/googlesql/blob/master/zetasql_to_googlesql_migration.md

## Three Upgrade Blockers

### 1. Bzlmod Required (2025.12.1+)

GoogleSQL 2025.12.1+ requires Bzlmod. Emulator uses WORKSPACE. This is a full Bazel build system migration affecting the entire dependency graph — not just ZetaSQL.

### 2. Namespace Rename (2026.01.1)

2026.01.1 renames everything:

| Old | New |
|-----|-----|
| `zetasql::` | `googlesql::` |
| `#include "zetasql/..."` | `#include "googlesql/..."` |
| `ZETASQL_` macros | `GOOGLESQL_` macros |
| `zetasql_base` | `googlesql_base` |
| `@zetasql` (Bazel) | `@googlesql` (Bazel) |

Emulator has hundreds of references across `backend/`, `frontend/`, `common/`, `third_party/spanner_pg/`.

### 3. Heavy Custom Patch File

`build/bazel/zetasql.patch` contains ~40+ patches that all need rebasing:

**GCC12 compilation fixes (~15 patches):**
- `const Type* type()` → `const zetasql::Type* type()` in value.h, value_inl.h, constant.h, simple_catalog.h, sql_constant.h, function_signature.h, input_argument_type.h, property_graph.h, resolved_column.h, operator.h, coercer.cc, resolver_expr.cc, test_value.h
- Designated initializer fixes: `{.field = val}` → `TypeName{.field = val}` in expr_resolver_helper.cc, name_scope.cc, internal_value.h, value.cc

**Build system patches:**
- Visibility overrides: `//visibility:public` on 7+ BUILD files (base, base/testing, common, parser, types, resolved_ast x2)
- LLVM toolchain removal from zetasql_deps_step_{1,2,3}.bzl
- `go_register_toolchains` removal from step_2
- grpc++ link dependency addition in base/testing/BUILD

**ICU build fixes:**
- macOS AR flag handling in bazel/icu.BUILD
- ARFLAGS patch in bazel/icu4c-64_2.patch

**Custom behavioral changes:**
- INSERT IGNORE: `return_all_insert_rows_insert_ignore_dml` option added to evaluator_base.h, evaluator_base.cc, evaluation.h, value_expr.cc
- `HasFloatingPointFields()` made public in type.h
- `constexpr` fix on `raw_type()` in value_inl.h
- `unique_ptr` logging overload in base/logging.h
- `SimpleTable` constructor fix in simple_catalog.cc
- grpc Status → absl::Status conversion operator in grpc_extra_deps.patch

## Effort Estimate

| Task | Effort | Risk |
|------|--------|------|
| WORKSPACE → Bzlmod migration | 2-4 days | High — affects all deps |
| Namespace rename in emulator code | 2-4 hours | Low — mechanical sed |
| Patch rebase onto new GoogleSQL | 1-2 days | Medium — conflicts from rename |
| `third_party/spanner_pg/` compat | Unknown | High — heavy ZetaSQL consumer |
| Docker build + test verification | 4-8 hours | Medium — slow build cycle |
| **Total** | **4-7 days** | |

## Feature Benefit

**Unclear.** Release notes for all intermediate versions say "export of internal changes, bug fixes, documentation improvements" with no detailed changelogs.

Current 2025.09.1 already supports everything needed:
- Full-text search (TOKENLIST, SEARCH_NGRAMS, SCORE_NGRAMS, TOKENIZE_*)
- SOUNDEX, NORMALIZE, SAFE_DIVIDE
- FORCE_INDEX hints, HIDDEN columns, named arguments
- Unicode regex via RE2

## Recommendation

**Don't upgrade unless:**
1. Google drops support for 2025.09.1
2. A specific needed feature lands only in newer versions
3. Bzlmod migration is already planned for other reasons

## Possible Upgrade Path (if needed later)

1. **Step to 2025.11.2 first** — last WORKSPACE-compatible version, skip Bzlmod migration
2. Rebase patches onto 2025.11.2
3. Verify build + tests
4. Then plan WORKSPACE → Bzlmod as separate project
5. Then step to 2026.01.1 with namespace rename
