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
  - The full Apache License 2.0 text is bundled alongside this notice in the
    `LICENSE` file (per Apache-2.0 §4, redistributions include a copy of the License).
    Upstream reference: https://www.apache.org/licenses/LICENSE-2.0

Placement and modifications:
- This is a Claude-authored workflow skill (it launches `Task` sub-agents for reader
  testing and uses Claude editing tools such as `str_replace`). It is vendored into
  Claude Code locations only — `.claude/skills/` and the personal `~/.claude/skills/` —
  and is intentionally NOT placed in the Codex `.agents/skills/` tree, where those
  Claude-specific instructions do not apply.
- The `SKILL.md` body is redistributed unmodified.
- The `.claude/skills/` copy's frontmatter grants
  `allowed-tools: Read, Edit, Write, Grep, Glob, Task` so the drafting and sub-agent
  reader-testing stages work under Claude Code. The `~/.claude/skills/` copy preserves
  the upstream frontmatter (unrestricted).
