# CrossPoint Reader agent instructions

The root `AGENTS.md` is the always-loaded policy and router. Detailed repository facts live in `rules/`; reusable procedures and review axes live in `skills/`.

Load rules and skills only when the root routing table or skill description matches the task. The only mandatory multi-skill load is the firmware handoff gate.

## Rules

- `environment.md`: host detection, PlatformIO, and local configuration
- `hardware-resources.md`: target hardware and resource protocol
- `architecture-hal.md`: build flags, repository structure, and HAL boundaries
- `coding-standards.md`: C/C++ conventions and platform pitfalls
- `ui-activities.md`: orientation, input, UI, activities, tasks, and fonts
- `testing-debugging.md`: build, formatting, CI, serial, crash, and verification procedures
- `git-workflow.md`: repository context, branches, commits, and publication controls
- `generated-files-cache.md`: generated sources, i18n, local artifacts, and cache formats

## Mandatory firmware review

`firmware-handoff` dispatches four independent read-only axes:

- `review-correctness`
- `review-architecture`
- `review-embedded`
- `review-i18n-docs`

The main agent verifies and fixes their findings before asking the human to review the diff and architecture.
