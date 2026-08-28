# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Repository-owned fonts used by Mosaic scene generators."""

from pathlib import Path

FONT_DIR = Path(__file__).resolve().parent / "fonts"

NOTO_SANS = str(FONT_DIR / "NotoSans-Regular.ttf")
DEJAVU_SANS = str(FONT_DIR / "DejaVuSans.ttf")
DEJAVU_SANS_BOLD = str(FONT_DIR / "DejaVuSans-Bold.ttf")
