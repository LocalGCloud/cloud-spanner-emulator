# ZetaSQL `constexpr` build fix (GCC 12)

## Problem

The Docker-based offline build failed with:

```
external/com_google_zetasql/zetasql/public/timestamp_picos_value.h:64:40:
  error: invalid return type 'zetasql::TimestampPicosValue' of 'constexpr'
  function 'static constexpr zetasql::TimestampPicosValue
  zetasql::TimestampPicosValue::MinValue()'

external/com_google_zetasql/zetasql/public/timestamp_picos_value.h:67:40:
  error: invalid return type 'zetasql::TimestampPicosValue' of 'constexpr'
  function 'static constexpr zetasql::TimestampPicosValue
  zetasql::TimestampPicosValue::MaxValue()'
```

The compiler notes explain the chain:

```
note: 'zetasql::TimestampPicosValue' is not literal because:
note:   non-static data member 'zetasql::TimestampPicosValue::time_'
        has non-literal type
note: 'zetasql::PicoTime' is not literal because:
note:   'zetasql::PicoTime' is not an aggregate, does not have a trivial
        default constructor, and has no 'constexpr' constructor that is
        not a copy or move constructor
```

## Root cause

GCC 12 enforces that a `constexpr` function can only return a *literal type*.
`PicoTime` is non-literal because its constructor body calls
`absl::FromUnixSeconds()` and `absl::Nanoseconds()` — neither of which is
`constexpr`.  Since `TimestampPicosValue` has a `PicoTime` data member, it
inherits non-literal status.  The `constexpr` annotations on `MinValue()` and
`MaxValue()` in both classes are therefore invalid.

This is a **pre-existing issue in ZetaSQL 2025.09.1**, not caused by any
emulator-local changes.  It surfaced now because the Docker image uses
GCC 12, which is stricter about `constexpr` correctness than the Clang
toolchain Google uses to develop ZetaSQL.

## Files affected (all under `@com_google_zetasql`)

| File | Method / member | Fix |
|------|-----------------|-----|
| `zetasql/public/pico_time.h` | `MinValue()` (line 97) | remove `constexpr` |
| `zetasql/public/pico_time.h` | `MaxValue()` (line 100) | remove `constexpr` |
| `zetasql/public/pico_time.h` | `PicoTime(int64_t, uint64_t)` constructor (line 136) | remove `constexpr` |
| `zetasql/public/timestamp_picos_value.h` | `MinValue()` (line 64) | remove `constexpr` |
| `zetasql/public/timestamp_picos_value.h` | `MaxValue()` (line 67) | remove `constexpr` |

## What was fixed in `build/bazel/zetasql.patch`

### 1. Broken patch entries repaired (pre-existing bugs in the patch file)

Three entries had **fake git index hashes** (`index 1234567..89abcde`) and
the `pico_time.h` entry also had **malformed hunk headers** with zero context
(e.g. `@@ -97 +97 @@`).  These hunks were silently ignored by `git apply`,
so the `pico_time.h` fixes were never actually being applied.

| File in patch | Issue | Fix |
|---------------|-------|-----|
| `type.h` | Fake index hash | `0000000..0000000` |
| `value_inl.h` | Fake index hash | `0000000..0000000` |
| `pico_time.h` | Fake index hash + broken hunks | Rewritten with proper context |

### 2. Missing `timestamp_picos_value.h` entry added

A new patch entry removes `constexpr` from `TimestampPicosValue::MinValue()`
and `TimestampPicosValue::MaxValue()`.  This was the **actual source of the
build error** and was not covered by any existing patch entry.

### 3. Functional impact: zero

Removing `constexpr` from these methods has **no runtime effect**.  They were
never usable in compile-time constant expressions because they depend on
non-`constexpr` abseil functions internally.  The methods return the same
values, with the same signatures — only the `constexpr` keyword is removed.

## Build environment

- **GCC**: 12 (Ubuntu 22.04)
- **Bazel**: 6.5.0
- **C++ standard**: C++17 (`-std=c++17`)
- **ZetaSQL version**: 2025.09.1
