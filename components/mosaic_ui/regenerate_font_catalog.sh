#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
GSPC="${GSPC:?set GSPC to the GSPC executable}"

ROOT="$(cd "$(dirname "$0")" && pwd)"
CLAW_ROOT="$(git -C "$ROOT" rev-parse --show-toplevel)"
WORK_ROOT="$(dirname "$CLAW_ROOT")"
GSP_ROOT="${ESP_GSP_ROOT:-$WORK_ROOT/esp-gsp-main}"
export GSP_ROOT ESP_GSP_ROOT="$GSP_ROOT"
export ESP_GSP_PLUGIN_ROOT="${ESP_GSP_PLUGIN_ROOT:-$WORK_ROOT/esp-gsp}"
export PYTHONPATH="$GSP_ROOT/tools${PYTHONPATH:+:$PYTHONPATH}"

"$ROOT/hub/scene/regenerate.sh"
for script in "$ROOT"/apps/*/scene/regenerate.sh; do
    "$script"
done

GENERATED="$ROOT/common/generated"
LINKED="$GENERATED/linked"
mkdir -p "$LINKED"

apps=("$ROOT/hub/generated/hub_480.gspb")
for manifest in "$ROOT"/apps/*/app.cmake; do
    app="$(basename "$(dirname "$manifest")")"
    apps+=("$ROOT/apps/$app/generated/$app.gspb")
done

"$GSPC" font-link "${apps[@]}" \
    --output-dir "$LINKED" \
    --catalog "$GENERATED/common-fonts.gspb" \
    --report "$GENERATED/font-link-report.md"

echo "font catalog: $GENERATED/common-fonts.gspb"
echo "font report: $GENERATED/font-link-report.md"
