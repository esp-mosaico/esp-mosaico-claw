#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
cd "$(dirname "$0")"
python3 gen_scene.py
scene_build_dir=$(mktemp -d)
trap 'rm -rf "$scene_build_dir"' EXIT
"${GSPC:?set GSPC}" build interact_480.json --scene-id 0 --profile "${MOSAIC_SCENE_PROFILE:-../../../common/mosaic_rgb565_auto.yaml}" -o "$scene_build_dir"
generated_dir="${MOSAIC_GENERATED_DIR:-../generated}"
mkdir -p "$generated_dir"
"$GSPC" bundle -o "$generated_dir/interact.gspb" "$scene_build_dir/interact.gsb" "$scene_build_dir"/*.gfb "$scene_build_dir"/*.grb
cp "$scene_build_dir"/*_binds.h "$scene_build_dir"/*_actions.h "$scene_build_dir"/*_objects.h "$generated_dir/"
