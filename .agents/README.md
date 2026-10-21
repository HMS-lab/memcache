# MemCache Agent Skills

This directory contains general-purpose Agent skills for the MemCache repository. Each skill has its own directory and a `SKILL.md` file.

## Skills

| Skill | Use Case | Example |
|---|---|---|
| `memcache-code-review` | Review code changes using the repository's scripts, documentation, and configuration | `Use memcache-code-review to review these changes` |
| `memcache-release` | Prepare MemCache changelogs or release notes and list newly added PRs | `Use memcache-release to prepare the 1.0.1 release notes` |

## Usage

- To request a skill, name it explicitly, for example: `Use memcache-release ...`.
- If you are unsure which skill applies, describe the task; the Agent should select the best match based on the `description` in `SKILL.md`.
- A task can combine skills, for example, use `memcache-code-review` and `memcache-release` before a release.
- When editing or adding a skill, edit only the relevant `SKILL.md` in its directory. This repository does not retain platform-specific UI metadata such as `openai.yaml`.

## Disclaimer

- These skills guide Agent workflows; they do not replace automated testing or quality assurance.
- Claims involving Ascend/NPU, RDMA/URMA, UBSIO, Kubernetes HA, multiple nodes, or performance data must be verified in the actual environment.
- In release, benchmark, and integration workflows, explicitly identify any validation that was not run; do not assume it passed.
- Commands and paths in the skills reflect the current repository structure. Update the relevant `SKILL.md` when repository scripts, directories, or build parameters change.
- Changes to external APIs, environment variables, installation methods, and release content still require human review; do not publish based only on a skill's guidance.
