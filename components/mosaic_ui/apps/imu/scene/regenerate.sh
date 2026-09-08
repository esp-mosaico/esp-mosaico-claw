#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
source "$(cd "$(dirname "$0")/../../../common" && pwd)/run_gspc.sh"
cd "$(dirname "$0")"
PROFILE="${MOSAIC_SCENE_PROFILE:-$(cd ../../../common && pwd)/mosaic_rgb565_auto.yaml}"
STEM=imu
APP_DIR="$(cd .. && pwd)"
GENERATED_DIR="${MOSAIC_GENERATED_DIR:-$APP_DIR/generated}"

python3 gen_scene.py
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

gspc build "${STEM}_480.json" --scene-id 0 \
    --profile "$PROFILE" -o "$OUT"
gspc build "${STEM}_assets_480.json" --scene-id 1 \
    --profile "$PROFILE" -o "$OUT"

mkdir -p "$GENERATED_DIR"
gspc pack "${STEM}_480.json" \
    --deployable --profile "$PROFILE" --gen-dir "$OUT" \
    -o "$GENERATED_DIR/${STEM}.gspb"

cp "$OUT/${STEM}_binds.h" "$GENERATED_DIR/${STEM}_binds.h"
cp "$OUT/${STEM}_actions.h" "$GENERATED_DIR/${STEM}_actions.h"
cp "$OUT/${STEM}_objects.h" "$GENERATED_DIR/${STEM}_objects.h"

echo "app bundle: $GENERATED_DIR/${STEM}.gspb"
ls -la "$GENERATED_DIR/${STEM}.gspb"
