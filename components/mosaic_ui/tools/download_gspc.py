#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Resolve the component-matched signed GSPC through esp-gsp-tools."""
import argparse
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--component-dir", type=Path,
        default=Path(__file__).resolve().parents[3]
        / "managed_components/espressif__esp-gsp",
    )
    args = parser.parse_args()
    try:
        from gsp.execute import executable_from_environment
        version = (args.component_dir / ".gspc_version").read_text().strip()
        print(executable_from_environment("gspc", version=version))
    except (ImportError, OSError, ValueError, RuntimeError) as exc:
        print(f"mosaic: cannot resolve GSPC: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
