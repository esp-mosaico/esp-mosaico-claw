#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
GSPC="${GSPC:?set GSPC to the GSPC executable}"
cd "$(dirname "$0")"
PROFILE="${MOSAIC_SCENE_PROFILE:-$(cd ../../../common && pwd)/mosaic_rgb565_auto.yaml}"
STEM=air_battle
APP_DIR="$(cd .. && pwd)"
GENERATED_DIR="${MOSAIC_GENERATED_DIR:-$APP_DIR/generated}"

python3 gen_scene.py
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

"$GSPC" build "${STEM}_480.json" --scene-id 0 \
    --profile "$PROFILE" -o "$OUT"

mkdir -p "$GENERATED_DIR"
"$GSPC" bundle -o "$GENERATED_DIR/${STEM}.gspb" \
    "$OUT/${STEM}.gsb" \
    "$OUT"/${STEM}_font*.gfb \
    $(if [[ -f "$OUT/${STEM}.grb" ]]; then echo "$OUT/${STEM}.grb"; fi)

cp "$OUT/${STEM}_binds.h" "$GENERATED_DIR/${STEM}_binds.h"
cp "$OUT/${STEM}_actions.h" "$GENERATED_DIR/${STEM}_actions.h"
cp "$OUT/${STEM}_objects.h" "$GENERATED_DIR/${STEM}_objects.h"

echo "app bundle: $GENERATED_DIR/${STEM}.gspb"
ls -la "$GENERATED_DIR/${STEM}.gspb"
