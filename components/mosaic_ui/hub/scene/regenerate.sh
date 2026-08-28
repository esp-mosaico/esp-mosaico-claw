#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
GSPC="${GSPC:?set GSPC to the GSPC executable}"
cd "$(dirname "$0")"
CLAW_ROOT="$(git -C "$(pwd)" rev-parse --show-toplevel)"
WORK_ROOT="$(dirname "$CLAW_ROOT")"
GSP_ROOT="${ESP_GSP_ROOT:-$WORK_ROOT/esp-gsp-main}"
if [[ ! -d "$GSP_ROOT/tools" ]]; then
    GSP_ROOT="$WORK_ROOT/esp-gsp"
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
