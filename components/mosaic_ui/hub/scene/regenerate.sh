#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
GSPC="${GSPC:?set GSPC to the GSPC executable}"
cd "$(dirname "$0")"
PROJECT_ROOT="$(cd ../../../.. && pwd)"
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
export ESP_GSP_ROOT="$GSP_ROOT"
export PYTHONPATH="$GSP_ROOT/tools${PYTHONPATH:+:$PYTHONPATH}"
PROFILE="${MOSAIC_SCENE_PROFILE:-$(cd ../../common && pwd)/mosaic_rgb565_auto.yaml}"
HUB_DIR="$(cd .. && pwd)"
GENERATED_DIR="${MOSAIC_GENERATED_DIR:-$HUB_DIR/generated}"

python3 gen_scenes.py

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

"$GSPC" build mosaic_hub_480.json --scene-id 0 \
    --profile "$PROFILE" \
    -o "$OUT"

"$GSPC" bundle -o "$GENERATED_DIR/hub_480.gspb" \
    "$OUT/mosaic_hub.gsb" \
    "$OUT"/mosaic_hub_font*.gfb \
    "$OUT/mosaic_hub.grb"

cp "$OUT/mosaic_hub_binds.h" "$GENERATED_DIR/mosaic_hub_binds.h"
cp "$OUT/mosaic_hub_actions.h" "$GENERATED_DIR/mosaic_hub_actions.h"
cp "$OUT/mosaic_hub_objects.h" "$GENERATED_DIR/mosaic_hub_objects.h"
cp "$OUT/mosaic_hub_templates.h" "$GENERATED_DIR/mosaic_hub_templates.h"

echo "hub bundle: $GENERATED_DIR/hub_480.gspb (1 scene: StackView + PageFlow + Drawer)"
ls -la "$GENERATED_DIR/hub_480.gspb"
