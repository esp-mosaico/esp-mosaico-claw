#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
GSPC="${GSPC:?set GSPC to the GSPC executable}"

ROOT="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$ROOT/../.." && pwd)"
WORK_ROOT="$(dirname "$PROJECT_ROOT")"
GSP_ROOT="${ESP_GSP_ROOT:-$PROJECT_ROOT/managed_components/espressif__esp-gsp}"
if [[ ! -d "$GSP_ROOT/tools" ]]; then
    if [[ -d "$WORK_ROOT/esp-gsp-main/tools" ]]; then
        GSP_ROOT="$WORK_ROOT/esp-gsp-main"
    elif [[ -d "$WORK_ROOT/esp-gsp/tools" ]]; then
        GSP_ROOT="$WORK_ROOT/esp-gsp"
    else
        echo "error: cannot locate esp-gsp tools" >&2
        exit 1
    fi
fi
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
