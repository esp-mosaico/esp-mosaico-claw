#!/usr/bin/env python3
"""Stage a Claw ota_0/ui_apps/system ESP-Iris System Update manifest."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import shutil
from pathlib import Path


EXPECTED_LAYOUT = {
    "ota_0": (0x300000, 6656 * 1024),
    "ui_apps": (0x980000, 3200 * 1024),
    "system": (0xCA0000, 3000 * 1024),
}
PARTITION_TABLE_REGION_BYTES = 0x1000


def _integer(value: str) -> int:
    return int(value.strip().removesuffix("K"), 0) * (
        1024 if value.strip().endswith("K") else 1
    )


def _read_layout(path: Path) -> dict[str, tuple[int, int]]:
    rows: dict[str, tuple[int, int]] = {}
    with path.open(encoding="utf-8", newline="") as handle:
        for row in csv.reader(line for line in handle if not line.lstrip().startswith("#")):
            if not row or not row[0].strip():
                continue
            rows[row[0].strip()] = (_integer(row[3]), _integer(row[4]))
    return rows


def _require_image(path: Path, partition: str, capacity: int) -> None:
    if not path.is_file() or path.stat().st_size == 0:
        raise ValueError(f"{partition} image is missing or empty: {path}")
    if path.stat().st_size > capacity:
        raise ValueError(
            f"{partition} image is {path.stat().st_size} bytes, capacity is {capacity}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--partition-csv", type=Path, required=True)
    parser.add_argument("--partition-table", type=Path, required=True)
    parser.add_argument("--application", type=Path, required=True)
    parser.add_argument("--ui-apps", type=Path, required=True)
    parser.add_argument("--system", type=Path, required=True)
    parser.add_argument("--stage-dir", type=Path, required=True)
    parser.add_argument("--release", required=True)
    args = parser.parse_args()

    actual_layout = _read_layout(args.partition_csv)
    for name, expected in EXPECTED_LAYOUT.items():
        if actual_layout.get(name) != expected:
            raise ValueError(
                f"unexpected {name} layout: {actual_layout.get(name)!r}, expected {expected!r}"
            )

    partition_table = args.partition_table.read_bytes()
    if not partition_table or len(partition_table) > PARTITION_TABLE_REGION_BYTES:
        raise ValueError("partition table must fit its 4 KiB Flash sector")
    layout_sha256 = hashlib.sha256(
        partition_table.ljust(PARTITION_TABLE_REGION_BYTES, b"\xff")
    ).hexdigest()

    images = {
        "ota_0.bin": (args.application, "ota_0"),
        "ui_apps.bin": (args.ui_apps, "ui_apps"),
        "system.bin": (args.system, "system"),
    }
    for source, partition in images.values():
        _require_image(source, partition, EXPECTED_LAYOUT[partition][1])

    stage_dir = args.stage_dir.resolve()
    stage_dir.mkdir(parents=True, exist_ok=True)
    for filename, (source, _) in images.items():
        shutil.copyfile(source, stage_dir / filename)

    manifest = {
        "schema": "esp-iris-system-update/v1",
        "release": args.release,
        "minimum_recovery_version": "2.3.0-recovery",
        "target": {"chip_id": 0x20, "flash_size": 16 * 1024 * 1024},
        "source_layout_sha256": [layout_sha256],
        "target_layout_sha256": layout_sha256,
        "components": [
            {
                "id": 1,
                "kind": "application",
                "target_offset": EXPECTED_LAYOUT["ota_0"][0],
                "file": "ota_0.bin",
            },
            {
                "id": 2,
                "kind": "data",
                "target_offset": EXPECTED_LAYOUT["ui_apps"][0],
                "file": "ui_apps.bin",
            },
            {
                "id": 3,
                "kind": "data",
                "target_offset": EXPECTED_LAYOUT["system"][0],
                "file": "system.bin",
            },
        ],
    }
    (stage_dir / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
