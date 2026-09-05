# Launcher Definitions

Read this reference when a skill should appear in launcher consumers and start directly without model interpretation.

## Source Format

Create `skills/<skill_id>/launcher.json` after its entry script and optional icon exist:

```json
{
  "schema_version": 1,
  "entry": "scripts/action.lua",
  "display_name": "My App",
  "args": {},
  "order": 10,
  "visible": true
}
```

Rules:

- Require integer `schema_version: 1`.
- Require `entry` as an existing skill-relative `.lua` path.
- Allow optional `icon` only as an existing skill-relative `.jpg` or `.jpeg` path.
- Allow optional non-empty `display_name`; omission uses the skill id.
- Allow optional `args` only as a JSON object.
- Allow optional integer `order`; omission uses registry order.
- Allow optional boolean `visible`; omission defaults to `true`.
- Reject absolute paths, `..`, backslashes, and unknown fields.

## Source And Runtime Flows

For a skill included in firmware or a file image, write `launcher.json` beside `SKILL.md` as a source file.

For a runtime-managed skill, write `launcher.json` beside `SKILL.md`, then call `publish_skill`. Do not place launcher behavior in the legacy top-level `execution` frontmatter field.
