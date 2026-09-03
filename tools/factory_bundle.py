#!/usr/bin/env python3
"""Validate the checked-in ESP-Mosaico Claw Factory Recovery bundle."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
from typing import Any


ESP_IMAGE_MAGIC = 0xE9
ESP32S31_CHIP_ID = 32
ESP_PARTITION_MAGIC = 0x50AA
ESP_PARTITION_MD5_MAGIC = 0xEBEB
FLASH_SIZE = 0x1000000
IRIS_REVISION = "9d1d2119e9626f9b53d8b18fc02b91a04d47ad3e"
RECOVERY_SOURCE_COMMIT = "7637ce39dfd61b09ef9ad1a61181f4d45cdb8fed"

IMAGE_LAYOUT = {
    "bootloader": ("bootloader.bin", 0x2000),
    "partition_table": ("partition-table.bin", 0x8000),
    "ota_data": ("ota_data_initial.bin", 0x9000),
    "recovery": ("factory.bin", 0x20000),
}

# label: (type, subtype, offset, size)
EXPECTED_PARTITIONS = {
    "otadata": (0x01, 0x00, 0x9000, 0x2000),
    "phy_init": (0x01, 0x01, 0xB000, 0x1000),
    "sysmeta": (0x01, 0x02, 0xC000, 0x14000),
    "factory": (0x00, 0x00, 0x20000, 0x200000),
    "coredump": (0x01, 0x03, 0x220000, 0xD0000),
    "nvs": (0x01, 0x02, 0x2F0000, 0x10000),
    "ota_0": (0x00, 0x10, 0x300000, 0x680000),
    "ui_apps": (0x01, 0x41, 0x980000, 0x320000),
    "system": (0x01, 0x83, 0xCA0000, 0x2EE000),
}

PROVISION_OFFSETS = {0x2000, 0x8000, 0x9000, 0x20000, 0x980000, 0xCA0000}


class BundleError(RuntimeError):
    """A reviewed Recovery bundle invariant was violated."""


def parse_int(value: Any) -> int:
    if isinstance(value, int):
        return value
    if not isinstance(value, str):
        raise BundleError(f"invalid integer value: {value!r}")
    try:
        return int(value, 0)
    except ValueError as error:
        raise BundleError(f"invalid integer value: {value!r}") from error


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def mapping(parent: dict[str, Any], key: str) -> dict[str, Any]:
    value = parent.get(key)
    if not isinstance(value, dict):
        raise BundleError(f"manifest field {key!r} must be an object")
    return value


def parse_partition_binary(path: Path) -> dict[str, tuple[int, int, int, int]]:
    data = path.read_bytes()
    result: dict[str, tuple[int, int, int, int]] = {}
    md5_seen = False
    for offset in range(0, len(data), 32):
        entry = data[offset : offset + 32]
        if len(entry) != 32:
            break
        magic = struct.unpack_from("<H", entry)[0]
        if magic == 0xFFFF:
            break
        if magic == ESP_PARTITION_MD5_MAGIC:
            expected_md5 = entry[16:32]
            if hashlib.md5(data[:offset]).digest() != expected_md5:
                raise BundleError("partition-table MD5 does not match its entries")
            md5_seen = True
            break
        if magic != ESP_PARTITION_MAGIC:
            raise BundleError(f"invalid partition entry magic at 0x{offset:x}")
        _, part_type, subtype, address, size, raw_label, _ = struct.unpack(
            "<HBBLL16sL", entry
        )
        label = raw_label.split(b"\0", 1)[0].decode("ascii")
        if label in result:
            raise BundleError(f"duplicate partition label: {label}")
        result[label] = (part_type, subtype, address, size)
    if not md5_seen:
        raise BundleError("partition-table has no MD5 entry")
    return result


def parse_size(value: str) -> int:
    normalized = value.strip().upper()
    multiplier = 1
    if normalized.endswith("K"):
        multiplier = 1024
        normalized = normalized[:-1]
    elif normalized.endswith("M"):
        multiplier = 1024 * 1024
        normalized = normalized[:-1]
    return int(normalized, 0) * multiplier


def validate_partition_csv(path: Path) -> None:
    rows: dict[str, tuple[int, int]] = {}
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.reader(line for line in stream if not line.lstrip().startswith("#")):
            if not row or not row[0].strip():
                continue
            label = row[0].strip()
            rows[label] = (parse_size(row[3]), parse_size(row[4]))
    expected = {name: (item[2], item[3]) for name, item in EXPECTED_PARTITIONS.items()}
    if rows != expected:
        raise BundleError("partitions_16MB.csv does not match the reviewed layout")

    ordered = sorted((offset, offset + size, name) for name, (offset, size) in rows.items())
    previous_end = 0
    for offset, end, name in ordered:
        if offset < previous_end:
            raise BundleError(f"partition overlaps its predecessor: {name}")
        if end > FLASH_SIZE:
            raise BundleError(f"partition exceeds 16 MB Flash: {name}")
        previous_end = end


def validate_flash_args(path: Path) -> None:
    offsets: set[int] = set()
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or "--no-progress" not in lines[0].split():
        raise BundleError("factory-provision must suppress esptool progress output")
    for line in lines:
        first = line.split(maxsplit=1)[0] if line.split() else ""
        if re.fullmatch(r"0x[0-9a-fA-F]+", first):
            offsets.add(int(first, 16))
    if offsets != PROVISION_OFFSETS:
        raise BundleError(
            "factory-provision must write only bootloader, partition-table, "
            "otadata, factory, ui_apps and system"
        )


def validate_disabled_flash_args(path: Path) -> None:
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if fields and re.fullmatch(r"0x[0-9a-fA-F]+", fields[0]):
            raise BundleError(
                f"disabled ESP-IDF flash target still contains an image: {path}"
            )


def validate_bundle(
    bundle: Path,
    partition_csv: Path,
    flash_args: Path | None,
    disabled_flash_args: list[Path],
    current_images: dict[str, Path],
    current_app: Path | None,
) -> None:
    manifest_path = bundle / "manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise BundleError(f"cannot read {manifest_path}: {error}") from error
    if not isinstance(manifest, dict):
        raise BundleError("manifest root must be an object")

    expected_top_level = {
        "schema_version": 2,
        "project": "factory",
        "profile": "recovery",
        "version": "2.3.0-recovery",
        "target": "esp32s31",
        "esp_iris_revision": IRIS_REVISION,
    }
    for key, expected in expected_top_level.items():
        if manifest.get(key) != expected:
            raise BundleError(
                f"manifest {key} mismatch: expected {expected!r}, got {manifest.get(key)!r}"
            )

    source = mapping(manifest, "source")
    if (source.get("commit") != RECOVERY_SOURCE_COMMIT or
            source.get("dirty") is not False):
        raise BundleError("Recovery source provenance changed")
    security = mapping(manifest, "security")
    if security != {"flash_encryption": False, "secure_boot": False}:
        raise BundleError("Recovery security configuration is incompatible")
    bootloader = mapping(manifest, "bootloader")
    if (bootloader.get("factory_recovery_gpio") != 7 or
            bootloader.get("factory_recovery_gpio_active_low") is not True):
        raise BundleError("bootloader must select Recovery with the active-low AI button")

    layout = mapping(manifest, "layout")
    recovery_partition = mapping(layout, "recovery_partition")
    application_partition = mapping(layout, "application_partition")
    if recovery_partition != {
        "name": "factory",
        "offset": "0x20000",
        "size": "0x200000",
    }:
        raise BundleError("manifest factory layout is incompatible")
    if application_partition != {"name": "ota_0", "offset": "0x300000"}:
        raise BundleError("manifest ota_0 layout is incompatible")

    initial = mapping(manifest, "initial_boot")
    if initial.get("partition") != "factory" or initial.get("expected_mode") != "recovery":
        raise BundleError("bundle does not initialize into Factory Recovery")

    images = mapping(manifest, "images")
    resolved: dict[str, Path] = {}
    for name, (filename, expected_offset) in IMAGE_LAYOUT.items():
        path = bundle / filename
        if not path.is_file() or path.stat().st_size == 0:
            raise BundleError(f"missing or empty image: {path}")
        resolved[name] = path
        item = mapping(images, name)
        if item.get("file") != filename or parse_int(item.get("offset")) != expected_offset:
            raise BundleError(f"manifest image layout mismatch: {name}")
        if item.get("size") != path.stat().st_size or item.get("sha256") != sha256(path):
            raise BundleError(f"manifest size or SHA-256 mismatch: {name}")

    if resolved["recovery"].stat().st_size > EXPECTED_PARTITIONS["factory"][3]:
        raise BundleError("factory.bin is larger than the factory partition")
    if resolved["bootloader"].stat().st_size > 0x8000 - IMAGE_LAYOUT["bootloader"][1]:
        raise BundleError("bootloader.bin overlaps the partition table")
    if resolved["partition_table"].stat().st_size > 0x1000:
        raise BundleError("partition-table.bin overlaps otadata")
    if resolved["ota_data"].stat().st_size > EXPECTED_PARTITIONS["otadata"][3]:
        raise BundleError("ota_data_initial.bin is larger than otadata")
    for name in ("bootloader", "recovery"):
        image_header = resolved[name].read_bytes()[:14]
        if len(image_header) != 14 or image_header[0] != ESP_IMAGE_MAGIC:
            raise BundleError(f"invalid ESP image magic: {resolved[name]}")
        if struct.unpack_from("<H", image_header, 12)[0] != ESP32S31_CHIP_ID:
            raise BundleError(f"image does not target ESP32-S31: {resolved[name]}")

    if current_app is not None:
        if not current_app.is_file():
            raise BundleError(f"missing current application image: {current_app}")
        if current_app.stat().st_size > EXPECTED_PARTITIONS["ota_0"][3]:
            raise BundleError("current Claw application is larger than ota_0")
        app_header = current_app.read_bytes()[:14]
        if (len(app_header) != 14 or app_header[0] != ESP_IMAGE_MAGIC or
                struct.unpack_from("<H", app_header, 12)[0] != ESP32S31_CHIP_ID):
            raise BundleError("current Claw application is not an ESP32-S31 image")

    validate_partition_csv(partition_csv)
    actual_partitions = parse_partition_binary(resolved["partition_table"])
    if actual_partitions != EXPECTED_PARTITIONS:
        raise BundleError("prebuilt partition-table.bin does not match partitions_16MB.csv")
    for name, current in current_images.items():
        if current.read_bytes() != resolved[name].read_bytes():
            raise BundleError(
                f"checked-in {IMAGE_LAYOUT[name][0]} differs from the current build"
            )
    if flash_args is not None:
        validate_flash_args(flash_args)
    for path in disabled_flash_args:
        validate_disabled_flash_args(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--partition-csv", type=Path, required=True)
    parser.add_argument("--flash-args", type=Path)
    parser.add_argument("--disabled-flash-args", type=Path, action="append", default=[])
    parser.add_argument("--current-bootloader", type=Path)
    parser.add_argument("--current-partition-table", type=Path)
    parser.add_argument("--current-ota-data", type=Path)
    parser.add_argument("--current-app", type=Path)
    arguments = parser.parse_args()
    try:
        current_images = {
            name: path
            for name, path in {
                "bootloader": arguments.current_bootloader,
                "partition_table": arguments.current_partition_table,
                "ota_data": arguments.current_ota_data,
            }.items()
            if path is not None
        }
        validate_bundle(
            arguments.bundle,
            arguments.partition_csv,
            arguments.flash_args,
            arguments.disabled_flash_args,
            current_images,
            arguments.current_app,
        )
    except (BundleError, OSError, ValueError) as error:
        print(f"factory bundle error: {error}", file=sys.stderr)
        return 1
    print(f"factory bundle valid: {arguments.bundle}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
