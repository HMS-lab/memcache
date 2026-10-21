---
name: memcache-code-review
description: "Use when the user requests a MemCache code review, change review, or review of newly added code. Check changes against this repository's existing docs/zh/c_cpp_naming.md, .clang-format, .pre-commit-config.yaml, pre-commit/pyproject.toml, script/run_ut.sh, docs/zh/SECURITYNOTE.md, and API/installation documentation."
---

# memcache-code-review — Code Review

## Goal

Review added or changed code for compliance with this repository's conventions, scripts, and documentation requirements. Identify issues that could affect builds, tests, API documentation, or security notes.

## Sources

Use only sources already present in this project:

- C/C++ naming, magic numbers, and test naming: `docs/zh/c_cpp_naming.md`.
- C/C++ formatting: `.clang-format` and `.pre-commit-config.yaml`.
- Python checks: `pre-commit/pyproject.toml`.
- Unit test and coverage gates: `script/run_ut.sh`.
- Security notes: `docs/zh/SECURITYNOTE.md`.
- Installation, configuration, and API documentation: `docs/zh/install_whl.md`, `docs/zh/install_run.md`, `docs/zh/memcache_config.md`, `docs/zh/memcache_c_api.md`, `docs/zh/memcache_c++_api.md`, `docs/zh/memcache_python_api.md`, and `docs/zh/memcache_restful_api.md`.

## Scope

- Focus on added or changed lines; do not require a full-file refactor.
- Briefly mention similar issues in existing code when useful, but do not expand the scope of changes.
- Prefer recommendations that follow the project's existing macros, logging, error codes, naming, and scripts.

## Review Checklist

### 1. Naming and Magic Numbers

- Check filenames, types, functions, variables, constants, macros, enumerations, test files, and Mock files against `docs/zh/c_cpp_naming.md`.
- Except for conventional values such as `0` and `-1`, extract numbers with independent semantic meaning into named constants or enumeration values.
- Check test code for magic numbers too, especially ports, retry counts, wait counts, timeouts, multipliers, and thresholds.

### 2. Formatting and Static Checks

- Follow `.clang-format` and `.pre-commit-config.yaml` for C/C++ line width, indentation, braces, and formatting.
- Follow `pre-commit/pyproject.toml` for Python line width and Ruff, Pylint, and Bandit settings.
- When suggesting checks, prefer `TARGET_BRANCH=develop bash script/ci-pre-commit-pr.sh` or `pre-commit run --files ...`.

### 3. Error Handling and Logging

- Use existing project error and success code semantics, such as `MMC_OK`, `MMC_ERROR`, `MMC_NOT_INITIALIZED`, `MMC_INVALID_ARGUMENT`, and `MMC_UNMATCHED_KEY`.
- Prefer existing macros and logging conventions in error paths, such as `MMC_VALIDATE_RETURN`, `MMC_ASSERT*`, and `MMC_LOG_ERROR`.
- If a new error path lacks a diagnosable reason, return code, or key context, report the risk. Do not introduce logging frameworks or fields that the project does not use.

### 4. Tests and Coverage

- Use `script/run_ut.sh` as the main entry point for C++ unit tests.
- Follow the script's coverage gates: at least 70% line coverage and 40% branch coverage.
- Name new C++ test files `*_test.cpp`. Existing `test_*.cpp` files do not need to be renamed solely to meet the naming convention.
- If Python tests require Ascend/NPU, MetaService, a mock server, or Kubernetes, state clearly when they were not run.

### 5. Documentation Updates

- For C API changes, check `docs/zh/memcache_c_api.md`.
- For C++ API changes, check `docs/zh/memcache_c++_api.md`.
- For Python API changes, check `docs/zh/memcache_python_api.md`.
- For REST/MetaService behavior changes, check `docs/zh/memcache_restful_api.md`.
- For installation, configuration, environment variable, or security behavior changes, check `docs/zh/install_whl.md`, `docs/zh/install_run.md`, `docs/zh/memcache_config.md`, and `docs/zh/SECURITYNOTE.md`.

### 6. Security Notes

- Use `docs/zh/SECURITYNOTE.md` when reviewing changes involving TLS, certificates, private keys, port ranges, runtime users, or file permissions.
- Do not include unredacted private keys, certificate passwords, tokens, full configuration values, complete environment dumps, or raw cache/shared-memory data in review output.

## Output Format

When issues are found, list them in severity order:

```markdown
### Location
`path/to/file:line`

### Issue
Explain which existing project convention, script, or documentation requirement is violated.

### Recommendation
Suggest the smallest fix and cite the source file that supports it.
```

When no issues are found, state the scope reviewed, the source files used, and any checks that were not run.

## Principles

- Do not add gate rules that the repository does not already have.
- Do not make unrelated refactors or broad formatting changes just to satisfy a rule.
- Do not report unit tests, hardware checks, Kubernetes checks, or performance checks as passing if they were not run.
