#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
source "$ROOT/common/run_gspc.sh"

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

gspc font-link "${apps[@]}" \
    --output-dir "$LINKED" \
    --catalog "$GENERATED/common-fonts.gspb" \
    --report "$GENERATED/font-link-report.md"

echo "font catalog: $GENERATED/common-fonts.gspb"
echo "font report: $GENERATED/font-link-report.md"
