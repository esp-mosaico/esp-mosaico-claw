#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
GSPC="${GSPC:?set GSPC to the GSPC executable}"
cd "$(dirname "$0")"
GSP_ROOT="${GSP_ROOT:-$(cd ../../../../../.. && pwd)}"
export PYTHONPATH="$GSP_ROOT/tools"
PROFILE="${MOSAIC_SCENE_PROFILE:-$(cd ../../../common && pwd)/mosaic_rgb565_auto.yaml}"
STEM=settings
APP_DIR="$(cd .. && pwd)"
GENERATED_DIR="${MOSAIC_GENERATED_DIR:-$APP_DIR/generated}"

python3 gen_scene.py
python3 generate_root_icon_data.py
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

"$GSPC" build "${STEM}_480.json" --scene-id 0 \
    --profile "$PROFILE" -o "$OUT"
"$GSPC" build "${STEM}_assets_480.json" --scene-id 1 \
    --profile "$PROFILE" -o "$OUT"

mkdir -p "$GENERATED_DIR"
"$GSPC" bundle -o "$GENERATED_DIR/${STEM}.gspb" \
    "$OUT/${STEM}.gsb" \
    "$OUT"/${STEM}_font*.gfb \
    $(if [[ -f "$OUT/${STEM}_assets.grb" ]]; then echo "$OUT/${STEM}_assets.grb"; fi)

cp "$OUT/${STEM}_binds.h" "$GENERATED_DIR/${STEM}_binds.h"
cp "$OUT/${STEM}_actions.h" "$GENERATED_DIR/${STEM}_actions.h"
cp "$OUT/${STEM}_objects.h" "$GENERATED_DIR/${STEM}_objects.h"
cp "$OUT/${STEM}_templates.h" "$GENERATED_DIR/${STEM}_templates.h"

echo "app bundle: $GENERATED_DIR/${STEM}.gspb"
ls -la "$GENERATED_DIR/${STEM}.gspb"
