# NOTICE — doc-coauthoring

This skill is vendored from a third-party source and redistributed here under its
upstream license.

- Skill: `doc-coauthoring`
- Upstream source: https://github.com/anthropics/skills (`skills/doc-coauthoring/SKILL.md`)
- Copyright: © Anthropic, PBC
- License: Apache License 2.0
  - Per the upstream repository README: "Many skills in this repo are open source
    (Apache 2.0)." `doc-coauthoring` is not among the source-available
    `docx` / `pdf` / `pptx` / `xlsx` skills, so it falls under the Apache 2.0 grant.
  - Full license text: https://www.apache.org/licenses/LICENSE-2.0

Modifications:
- The `SKILL.md` body is redistributed unmodified.
- The copy placed under `.claude/skills/` adds a single `allowed-tools:` line to the
  YAML frontmatter as a Claude Code harness adaptation. The copy under `.agents/skills/`
  (Codex) and the personal `~/.claude/skills/` copy preserve the upstream frontmatter
  unchanged.
