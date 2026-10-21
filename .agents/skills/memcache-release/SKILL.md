---
name: memcache-release
description: "Use when preparing MemCache changelogs or release notes. Base them only on the version, date, commit/PR differences, and change information provided by the user. This skill does not cover release checks, build matrices, artifact verification, or executing a release."
---

# MemCache Release Notes

## Scope

This skill is only for preparing changelogs or release notes for MemCache / memcache_hybrid.

It does not:

- Create or maintain release checklists.
- Change version numbers, release branches, tags, or release pages.
- Build `.run` packages, wheels, or release artifacts.
- Upload to PyPI, create a GitCode/GitHub release, or execute a release.
- Decide whether unrun tests, hardware gates, build matrices, or performance checks passed.

## Information to Gather

Before drafting, confirm:

- `VERSION`: the target version, such as `1.0.1`.
- `YYYY-MM-DD`: the release date. Use the current date if the user did not specify one.
- Comparison range: the previous release tag/commit through the current release tag/commit, or a PR/commit list supplied by the user.
- Numbers, links, and titles of all PRs added in this release.
- Key changes identified by the user. If provided, prioritize these and use git/PR differences to fill in omissions.

If the comparison range or PR list is missing, first check local git, remote PR information, or ask the user for the details. If they cannot be confirmed, clearly mark the missing information in the result.

## Writing Principles

- Include only sections with changes; omit empty sections.
- Describe behavior changes, usage impact, and compatibility impact for users. Do not mechanically list commits.
- Put each change in the single most appropriate section to avoid duplication.
- Preserve necessary API names, configuration options, environment variables, commands, paths, error codes, and PR numbers.
- If classification is uncertain, prefer `Changed` and explain the basis when needed.
- `References` must include every PR added in this release.

## Categories

- `Summary`: Overall summary of the release.
- `Feature`: New capabilities, APIs, operating modes, platform/hardware support, tools, or integrations.
- `Changed`: Changes to existing behavior, configuration, builds, performance, logging, defaults, internal implementation, or other non-bug-fix changes.
- `Fixed`: Bug, stability, or error-handling fixes, including crashes, hangs, timeouts, and incorrect data.
- `Compatibility`: ABI/API, Python version, hardware/driver/CANN, dependency, package name, installation method, or behavior compatibility notes.
- `Documentation`: Updates to README files, installation guides, API/configuration documentation, examples, and release documents.
- `References`: All PRs added in this release.

## Output Format

Use this format exactly. Include every changed section except that `Installation` and `References` are always present. Omit sections with no changes.

```markdown
# MemCache v{VERSION} - {YYYY-MM-DD}

## Installation

- PyPI: [memcache-hybrid](https://pypi.org/project/memcache-hybrid/)
- Install: `pip install memcache-hybrid`

## Summary

Release summary

## Feature

- New capability

## Changed

- Change description

## Fixed

- Fix description

## Compatibility

- Compatibility note

## Documentation

- Documentation change

## References

- [!123](https://gitcode.com/Ascend/memcache/pull/123) — PR title
```

## Reference Rules

- Sort PRs by number or merge time, and use the same ordering throughout a release note.
- Use the actual PR title; do not rewrite it in a way that changes its meaning.
- If a PR link is unavailable but its number and title are confirmed, still include a link in the standard format.
- If a change came from a direct commit rather than a PR, add a note after `References`; do not invent a PR.

## Final Check

Before output, confirm:

- The version and date in the title are correct.
- Empty sections are omitted.
- The PyPI link and installation command under `Installation` use the required format.
- `References` includes all PRs added in this release.
- Every change description is supported by a PR, commit, or information provided by the user.
- No validation that was not run is described as passing.
