#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "$0")/../../../common" && pwd)/run_gspc.sh"
cd "$(dirname "$0")"
PROFILE="$(pwd)/esp32s31_rgb565_compact.yaml"
APP_DIR="$(cd .. && pwd)"
GENERATED_DIR="${MOSAIC_GENERATED_DIR:-$APP_DIR/generated}"
TARGET_WIDTH="${MOSAIC_WELCOME_WIDTH:-480}"
SCENES=(welcome_intro welcome_keys welcome_g1 welcome_g2 welcome_g4)
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT/prepared" "$GENERATED_DIR"
PACK=()
for scene in "${SCENES[@]}"; do
    python3 gen_scene.py "$scene" "$OUT/prepared/$scene.json" "$TARGET_WIDTH"
    PACK+=("$OUT/prepared/$scene.json")
done
gspc pack "${PACK[@]}" --profile "$PROFILE" \
    -o "$GENERATED_DIR/welcome.gspb" --gen-dir "$OUT/generated" \
    --api-header "$OUT/welcome_gsp.h" --symbol welcome_bundle
find "$OUT/generated" -type f \( -name '*_actions.h' -o -name '*_binds.h' -o -name '*_objects.h' \) -exec cp {} "$GENERATED_DIR" \;
