# Launcher Definitions

Read this reference when a skill should appear in launcher consumers and start directly without model interpretation.

## Source Format

Create `skills/<skill_id>/launcher.json` after its entry script and optional icon exist:

```json
{
  "schema_version": 1,
  "entry": "scripts/action.lua",
  "args": {},
  "exclusive": "display",
  "replace": false,
  "order": 10,
  "visible": true
}
```

Rules:

- Require integer `schema_version: 1`.
- Require `entry` as an existing skill-relative `.lua` path.
- Allow optional `icon` only as an existing skill-relative `.jpg` or `.jpeg` path.
- Allow optional `args` only as a JSON object.
- Allow optional `exclusive` with 1 to 31 characters for singleton resources such as `display` or `audio`.
- Allow optional integer `order`; omission uses registry order.
- Allow optional boolean `visible`; omission defaults to `true`.
- Allow optional boolean `replace`; omission defaults to `false`. Set it only when launch should stop a conflicting job first.
- Reject absolute paths, `..`, backslashes, and unknown fields.

## Source And Runtime Flows

For a skill included in firmware or a file image, write `launcher.json` beside `SKILL.md` as a source file.

For a runtime-managed skill, prefer passing a `launcher` object to `publish_skill` when available. The firmware validates referenced files and generates `launcher.json`; omit `schema_version` from the published object because the firmware adds it.

Do not write `launcher.json` and publish a launcher object in the same flow. Do not place launcher behavior in the legacy top-level `execution` frontmatter field.
