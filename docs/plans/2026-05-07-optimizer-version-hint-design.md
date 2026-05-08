# Design: Accept OPTIMIZER_VERSION Statement Hint

## Summary

Production Spanner queries using `@{OPTIMIZER_VERSION=latest}` fail on the emulator with "invalid hint". This is the **only** missing feature from the full-text search emulation proposal — the other 8 features already work. Fix: add `optimizer_version` to the accepted hints whitelist. One file, ~3 lines.

## Context

Production Spanner supports `@{OPTIMIZER_VERSION=latest}` as a statement-level hint. The emulator rejects it with "invalid hint" because `optimizer_version` is not in the `CheckSpannerHintName` whitelist in `query_validator.cc`.

## Investigation

The original proposal (spanner-fulltext-emulation/proposal.md) listed 9 missing features. Deep codebase exploration revealed that 8 of 9 already work in the current emulator (ZetaSQL 2025.09.1):

### Already Supported Features

| Feature | How It Works | Location |
|---------|-------------|----------|
| TOKENLIST type | Proto type 22, full DDL support | `backend/schema/ddl/operations.proto` |
| TOKENIZE_NGRAMS() | Search function catalog | `backend/query/search/search_function_catalog.cc` |
| TOKENIZE_FULLTEXT() | Search function catalog | `backend/query/search/search_function_catalog.cc` |
| SEARCH_NGRAMS() | N-gram search evaluator | `backend/query/search/search_ngrams_evaluator.cc` |
| SCORE_NGRAMS() | N-gram scoring evaluator | `backend/query/search/search_ngrams_evaluator.cc` |
| SOUNDEX() | ZetaSQL `FN_SOUNDEX_STRING` (1069) | Enabled via `FEATURE_ADDITIONAL_STRING_FUNCTIONS` |
| SAFE_DIVIDE() | ZetaSQL builtin | Enabled via `FEATURE_SAFE_FUNCTION_CALL` |
| NORMALIZE(str, form) | ZetaSQL `FN_NORMALIZE_STRING` (1043) | Enabled via `FEATURE_ADDITIONAL_STRING_FUNCTIONS` |
| FORCE_INDEX hint | Hint validator whitelist | `backend/query/query_validator.cc` line 68/224/242 |
| HIDDEN columns | Column attribute | `backend/schema/catalog/column.h` line 189/317 |
| Named args (=> syntax) | Language feature | `FEATURE_NAMED_ARGUMENTS` in analyzer_options.cc |
| Unicode regex (\pM) | RE2 engine | RE2 supports Unicode property classes |
| SEARCH INDEX DDL | DDL parser | `CreateSearchIndex` in operations.proto |
| Generated columns | Expression evaluator | `backend/actions/evaluated_column.cc` |

### Only Gap Found

`OPTIMIZER_VERSION` statement hint — not in `CheckSpannerHintName` whitelist (query_validator.cc line 240-258).

## Change

**File:** `backend/query/query_validator.cc`

1. Add constant `kHintOptimizerVersion = "optimizer_version"` at line 138 alongside other hint constants
2. Add `kHintOptimizerVersion` to `RESOLVED_QUERY_STMT` entry in `CheckSpannerHintName` at line 258

Hint is parsed by ZetaSQL, accepted by whitelist, silently ignored. No value validation needed — emulator has no optimizer.

## Non-Goals

- No optimizer behavior emulation
- No hint value validation (accept any type/value)
- No new tests (existing hint infrastructure is well-tested)
