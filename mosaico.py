#!/usr/bin/env python3
"""Repository-local launcher for the pinned ESP-Mosaico tools submodule."""

from __future__ import annotations

from pathlib import Path
import sys


REPOSITORY_ROOT = Path(__file__).resolve().parent
TOOLS_ROOT = REPOSITORY_ROOT / "third-party" / "esp-mosaico-tools"
PACKAGE_ROOT = TOOLS_ROOT / "tools"
if not (PACKAGE_ROOT / "mosaico_cli" / "cli.py").is_file():
    print(
        "mosaico: the esp-mosaico-tools submodule is unavailable; run "
        "'git submodule update --init third-party/esp-mosaico-tools'.",
        file=sys.stderr,
    )
    raise SystemExit(3)
sys.path.insert(0, str(PACKAGE_ROOT))

from mosaico_cli.cli import main  # noqa: E402


if __name__ == "__main__":
    raise SystemExit(main(tool_root=TOOLS_ROOT))
