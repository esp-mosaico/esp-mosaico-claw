---
{
  "name": "skill_creator",
  "description": "Create or update model-invoked functions/features/skills, workflows, and Lua-backed capabilities.",
  "metadata": {
    "cap_groups": [
      "cap_skill"
    ],
    "manage_mode": "readonly"
  }
}
---

# Skill Creator

Use this skill to create, register, update, or remove reusable model-invoked skills, including tool-like workflows, project-specific features, and Lua-backed automations.

## Required Flow

1. Define the user-facing behavior, trigger wording, prerequisites, capability groups, bundled files, and whether Lua or a launcher entry is needed.
2. Choose a new `skill_id` based on the behavior and confirm `skills/<skill_id>/` does not already exist unless the user requested an update.
3. Read only the conditional references required by the task.
4. Create the complete source files directly; do not use a preparation script or unchanged template.
5. Validate metadata, paths, Lua invocation documentation, and any launcher references.
6. Call `register_skill` after every required file exists.
7. Report the registered id, launcher inclusion, and any required image rebuild, registry reload, or device restart.

## Skill Contract

```text
skills/<skill_id>/
├── SKILL.md              # required
├── launcher.json         # optional launcher entry
├── scripts/*.lua         # optional executable payloads
├── references/*          # optional on-demand guidance
└── assets/*              # optional bundled assets
```

- Use lowercase letters, digits, underscores, or hyphens for `skill_id`; reject spaces, separators, absolute paths, `..`, and duplicate ids.
- Make `SKILL.md` frontmatter `name` equal `skill_id`.
- Write a one-sentence `description` of user intent with likely trigger words and critical prerequisites; do not describe only the implementation.
- Keep every bundled Lua file under `scripts/` and reference it as `{CUR_SKILL_DIR}/scripts/<name>.lua`.
- Document each Lua script's args, sync or async mode, timeout, exclusive group, and output/error handling.
- Put optional `launcher.json` beside `SKILL.md`. Do not use the legacy top-level `execution` field.
- Use only skill-local paths in skill files; never embed source-tree or FATFS output paths.
- Do not create a bare Lua file for an ambiguous feature request.

## Lua-Backed Skill Pattern

Use this pattern after deciding the final behavior, prerequisites, args, and script path:

````md
---
{
  "name": "skill_id",
  "description": "Describe the user-facing action, likely trigger wording, and critical prerequisites in one sentence.",
  "metadata": {
    "cap_groups": ["cap_lua"],
    "manage_mode": "readonly"
  }
}
---

# Skill Title

Run exactly one bundled Lua script for the documented user intent. Report script errors directly and do not retry with changed arguments unless the user asks.

## Script Args Schema

```json
{
  "type": "object",
  "properties": {}
}
```

## Tool Call Inputs

```json
{"path":"{CUR_SKILL_DIR}/scripts/action.lua","args":{}}
```

## Recommended Flow

1. Validate arguments against the documented schema.
2. Run `{CUR_SKILL_DIR}/scripts/action.lua` using the documented sync or async policy.
3. Report the result or error directly.
````

Replace every placeholder with final behavior. Document the exact args, execution mode, timeout, async name, exclusive group, replacement policy, expected output, and error handling. For a non-Lua skill, omit Lua-specific sections.

## Conditional References

- For a Lua-backed skill, activate `cap_lua`, then read `{CUR_SKILL_DIR}/references/write_lua.md` and `{CUR_SKILL_DIR}/references/run_lua.md` before writing files.
- For a launcher-visible skill, read `{CUR_SKILL_DIR}/references/launcher.md` before creating or publishing launcher configuration.
- For a non-Lua skill without a launcher, do not load the Lua or launcher references.

## Registration And Updates

Ensure `skills/<skill_id>/SKILL.md` and all launcher-referenced files exist, then call:

```json
{"skill_id":"weather_alerts","file":"weather_alerts/SKILL.md"}
```

Call `register_skill` with `file` exactly `<skill_id>/SKILL.md` and treat its result as the registered metadata source of truth. Confirm launcher metadata when supplied.

For updates, modify the existing source files and launcher configuration, call `unregister_skill` only when registry replacement requires it, then call `register_skill` with the same id and file.

## Failure Handling

If writing files or calling `register_skill` is impossible, provide the target relative paths and complete contents, report the blocker, and do not claim completion. Stop and report the returned error when registration or unregistration fails.
