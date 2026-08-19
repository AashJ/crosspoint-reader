# AGENTS.md migration map

This ledger proves that the 14 sections of the former 1,032-line root guide were retained.

| Former section | New home |
| --- | --- |
| AI Agent Identity and Cognitive Rules | Root `AGENTS.md`, plus the mandatory firmware handoff |
| Development Environment Awareness | `environment.md` |
| Platform and Hardware Constraints | `hardware-resources.md` |
| Project Architecture | `architecture-hal.md` |
| Coding Standards | `coding-standards.md` |
| UI and Orientation Guidelines | `ui-activities.md` |
| Common Patterns | `ui-activities.md` |
| Testing and Debugging | `testing-debugging.md` |
| Git Workflow and Repository Awareness | `git-workflow.md` |
| Generated Files and Build Artifacts | `generated-files-cache.md` |
| Local Development Configuration | `environment.md` |
| Testing and Verification Workflow | `testing-debugging.md` and `firmware-handoff` |
| Serial Monitoring and Live Debugging | `testing-debugging.md` |
| Cache Management and Invalidation | `generated-files-cache.md` |

The detailed rule files were copied by exact original section boundaries. The
root file replaces the former 17-line introduction and identity block with the
same project identity, evidence, API-verification, resource-justification, and
verification duties plus the new human-ownership and review policy.

One preserved rule was intentionally superseded: local commits no longer require
completed device testing. They require explicit human approval; actual hardware
testing remains a human-owned prerequisite for opening a PR.
