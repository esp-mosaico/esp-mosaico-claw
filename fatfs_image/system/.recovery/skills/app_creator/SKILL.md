---
{
  "name": "app_creator",
  "description": "Create, update, publish, or remove launcher-visible Lua Apps stored as independent App packages.",
  "metadata": {
    "cap_groups": [
      "cap_files",
      "cap_lua",
      "cap_app_manage"
    ]
  }
}
---

# App Creator

Use this skill only for launcher-visible Lua Apps. Apps are independent packages, not Skills.

## App Contract

Derive the writable DATA root by removing `/skills/app_creator` from the expanded `{CUR_SKILL_DIR}` path. Store each App under `<DATA>/apps/<app_id>/`:

```text
<DATA>/apps/<app_id>/
├── launcher.json
├── scripts/main.lua
└── assets/*
```

- `app_id` must match `^[A-Za-z0-9_-]{1,63}$` and equal the directory name.
- `launcher.json` uses `schema_version: 1` and requires `id` and `entry`.
- Optional fields are `display_name`, `icon`, `args`, `order`, and `visible`.
- `entry` and `icon` are package-relative paths without absolute prefixes, `..`, or backslashes.
- Launcher icons must be JPEG files.
- Do not add `simulator` or `peripherals`; the simulator infers peripherals from imported Lua modules.
- Never write an App under `/system` or inside a Skill directory.

Minimal manifest:

```json
{
  "schema_version": 1,
  "id": "example_app",
  "display_name": "Example App",
  "entry": "scripts/main.lua",
  "visible": true
}
```

## Required Flow

1. Resolve `<DATA>/apps/<app_id>/` and inspect any existing package before an update.
2. Write the complete `launcher.json`, Lua scripts, and optional assets with file capabilities.
3. Activate `builtin_lua_modules` and read only the documentation needed for imported modules before writing minimal Lua code.
4. Call `publish_app` with only `app_id` after every required file exists.
5. Treat the tool result as the source of truth and report publication errors directly.

For updates, replace complete files and publish the same id again. Call `remove_app` only when the user explicitly requests deletion; it deletes the entire writable App directory. Do not run a long-lived UI App only to validate its package.
