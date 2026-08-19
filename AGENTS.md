# CrossPoint Reader agent guide

CrossPoint Reader is open-source e-reader firmware for the Xteink X4. Its mission is a lightweight, high-performance reading experience focused on EPUB rendering. It targets the ESP32-C3 and ESP32-S3; the C3's roughly 380 KB usable RAM, lack of PSRAM, and single 48 KB framebuffer make stability and resource discipline primary constraints.

## Start here

At session start, run `uname -s`, `git branch --show-current`, `git remote -v`, and `git status --short`. Integration work targets `develop`.

Act as a senior embedded C++ engineer. Base claims on repository evidence: cite the paths and line numbers that justify a proposed change. Check `freeink-sdk/` or the [FreeInk SDK index](https://freeink.org/llms.txt) instead of inventing APIs. Explain the mechanism behind performance or memory claims and justify every new heap allocation. For every fix, tell the human how to verify it.

Read only the rules that match the task:

| When the task touches... | Read |
| --- | --- |
| Host setup, PlatformIO usage, or local configuration | [.agents/rules/environment.md](.agents/rules/environment.md) |
| RAM, allocation, flash, strings, or hardware limits | [.agents/rules/hardware-resources.md](.agents/rules/hardware-resources.md) and the `heap-discipline` skill |
| Build flags, storage, input, display, settings, i18n, rendering, or SDK boundaries | [.agents/rules/architecture-hal.md](.agents/rules/architecture-hal.md) and the `hal-and-abstractions` skill |
| C or C++ implementation | [.agents/rules/coding-standards.md](.agents/rules/coding-standards.md); also load `control-flow-clarity` for branching or state changes |
| Activities, orientation, buttons, UI, tasks, fonts, or lifecycle | [.agents/rules/ui-activities.md](.agents/rules/ui-activities.md) |
| Builds, formatting, CI, serial logs, crashes, or verification | [.agents/rules/testing-debugging.md](.agents/rules/testing-debugging.md) |
| Git, branches, commits, remotes, or publication | [.agents/rules/git-workflow.md](.agents/rules/git-workflow.md) |
| Generated HTML/i18n, caches, EPUB formats, or invalidation | [.agents/rules/generated-files-cache.md](.agents/rules/generated-files-cache.md) |
| New features, activities, settings, libraries, or dependencies | `SCOPE.md` and the `scope-discipline` skill |
| Refactoring or preparing a change for review | the `refactor-for-review` skill |

Repository-local skills live under `.agents/skills/`. Load a skill when its frontmatter description matches the task; do not load every skill speculatively.

## Human ownership

A PR is a long-term maintenance commitment. Working code is not enough: prefer the simplest design that meets the real requirement, fits `SCOPE.md`, and can be understood and maintained by its human owner.

Fully autonomous end-to-end agents are forbidden. Review subagents are allowed only as read-only advisers under the main agent's supervision. They may inspect code, diffs, history, and build metadata, but may not edit files, commit, push, open or close PRs, post reviews, release, deploy, or flash hardware.

The human must write the PR description. The agent may supply concise factual notes and test results, but not ready-to-paste PR prose. The agent may create or amend a local commit only after explicit human approval. It must never push by itself or open or close a PR.

Repository-facing prose should use plain English that a non-native speaker can follow. Use standard technical terms when they are the clearest words. Keep code comments short and limited to non-obvious mechanisms, field meaning, or necessary special cases.

## Mandatory firmware handoff

For every logical change that can affect shipped firmware or its build—including C/C++, build configuration, partitions, code-generation sources, translations, and release scripts—the main agent must complete [.agents/skills/firmware-handoff/SKILL.md](.agents/skills/firmware-handoff/SKILL.md) before declaring the work ready or making an approved local commit.

Pure tests, diagnostics, documentation, and host-only Python scripts use a lighter review unless they alter firmware output or its build.

This concise handoff checklist is a hard requirement:

- [ ] Relevant tests and `./bin/clang-format-fix -g` completed; firmware built once after the final code edit.
- [ ] Read-only reviews completed for correctness, architecture, embedded constraints, and i18n/user documentation.
- [ ] The main agent verified, deduplicated, and fixed findings, then reran affected reviews after material fixes.
- [ ] The main agent explained the behavior and architecture in plain English to someone unfamiliar with the codebase.
- [ ] The human was told to review the diff and explicitly confirmed understanding of the behavior and architecture and ownership of maintenance.
- [ ] The agent gave a concrete hardware test plan and reminded the human that hardware testing is required before a PR can be opened.
- [ ] The agent did not claim hardware verification and did not write a PR description.

If the human rejects the architecture, stop the handoff, ask what must change, and revise before calling the work ready. Hardware testing is entirely the human's responsibility; explain what to test and what failures to watch for.
