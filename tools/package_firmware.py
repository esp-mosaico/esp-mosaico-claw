#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

import csv
import datetime
import json
import os
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path, PurePosixPath, PureWindowsPath

APPLICATION_NAME = "ESP-Mosaico_Factory_Firmware_ESP-Claw"
BOARD_NAME = "ESP-Mosaico"
BOARD_BRAND = "espressif"
TARGET = "esp32s31"


def log(message: str) -> None:
    print(message, file=sys.stderr)


def git_refs() -> str:
    tag = os.getenv("GITHUB_REF_NAME", "").strip()
    if tag and os.getenv("GITHUB_REF_TYPE", "").strip() == "tag":
        return tag

    for command in (("git", "describe", "--tags", "--long", "--always"),
                    ("git", "rev-parse", "--short", "HEAD")):
        try:
            result = subprocess.run(
                command, check=True, capture_output=True, text=True,
            )
            value = result.stdout.strip()
            if value:
                return value
        except (subprocess.CalledProcessError, OSError):
            continue

    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d_%H%M%S")


def git_commit_timestamp() -> str:
    try:
        result = subprocess.run(
            ["git", "log", "-1", "--format=%cI"], check=True,
            capture_output=True, text=True,
        )
    except (subprocess.CalledProcessError, OSError):
        return datetime.datetime.now(datetime.timezone.utc).isoformat()
    value = result.stdout.strip()
    if value:
        return value
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def load_flasher_data(build_dir: Path) -> tuple[dict, str]:
    flasher_path = build_dir / "flasher_args.json"
    flash_args_path = build_dir / "flash_args"
    if not flasher_path.is_file():
        raise RuntimeError(f"missing flasher_args.json: {flasher_path}")
    if not flash_args_path.is_file():
        raise RuntimeError(f"missing flash_args: {flash_args_path}")

    with flasher_path.open("r", encoding="utf-8") as stream:
        data = json.load(stream)

    flash_settings = data.get("flash_settings")
    flash_size = flash_settings.get("flash_size") if isinstance(flash_settings, dict) else None
    if not isinstance(flash_size, str) or not flash_size.strip():
        raise RuntimeError("flash_settings.flash_size not found or invalid")

    flash_files = data.get("flash_files")
    if not isinstance(flash_files, dict) or not flash_files:
        raise RuntimeError("flash_files not found or invalid")
    for address, relative_path in flash_files.items():
        if not isinstance(relative_path, str) or not relative_path.strip():
            raise RuntimeError(f"flash_files[{address!r}] is invalid")
        _, archive_path = validate_flash_file_path(build_dir, relative_path)
        # Keep metadata consistent with the normalized path stored in the tar.
        flash_files[address] = archive_path

    return data, flash_size.strip()


def validate_flash_file_path(build_dir: Path, raw_path: str) -> tuple[Path, str]:
    """Return a safe source path and POSIX-relative archive name."""
    # flasher_args.json is generated on POSIX, but reject Windows separators
    # after normalizing them as well so the archive remains safe cross-platform.
    normalized = raw_path.replace("\\", "/")
    archive_path = PurePosixPath(normalized)
    windows_path = PureWindowsPath(normalized)
    if (archive_path.is_absolute() or windows_path.drive or not archive_path.parts
            or ".." in archive_path.parts):
        raise RuntimeError(f"unsafe flash file path: {raw_path!r}")

    build_root = build_dir.resolve()
    source_path = (build_root / Path(*archive_path.parts)).resolve()
    try:
        source_path.relative_to(build_root)
    except ValueError as error:
        raise RuntimeError(f"flash file escapes build directory: {raw_path!r}") from error
    if not source_path.is_file():
        raise RuntimeError(f"flash file not found: {build_dir / raw_path}")

    return source_path, archive_path.as_posix()


def load_sdkconfig(build_dir: Path) -> dict:
    path = build_dir / "config" / "sdkconfig.json"
    if not path.is_file():
        raise RuntimeError(f"missing sdkconfig.json: {path}")
    with path.open("r", encoding="utf-8") as stream:
        data = json.load(stream)
    if not isinstance(data, dict):
        raise TypeError(f"invalid sdkconfig.json object: {path}")
    return data


def resolve_console_output(sdkconfig: dict) -> str:
    if sdkconfig.get("ESP_CONSOLE_UART") is True:
        return "UART"
    if sdkconfig.get("ESP_CONSOLE_USB_SERIAL_JTAG") is True:
        return "Serial JTAG"
    return "unknown"


def parse_size(value: str) -> int:
    text = value.strip().upper()
    if text.endswith("K"):
        return int(text[:-1], 0) * 1024
    if text.endswith("M"):
        return int(text[:-1], 0) * 1024 * 1024
    return int(text, 0)


def hex_size(value: int) -> str:
    return f"0x{value:x}"


def extract_partition_table(build_dir: Path, flasher_data: dict) -> list[dict]:
    partition_info = flasher_data.get("partition-table")
    if not isinstance(partition_info, dict):
        raise TypeError("partition-table entry missing in flasher_args.json")
    partition_file = partition_info.get("file")
    if not isinstance(partition_file, str) or not partition_file.strip():
        raise RuntimeError("partition-table.file missing or invalid")

    partition_bin = build_dir / partition_file
    if not partition_bin.is_file():
        raise RuntimeError(f"partition table binary not found: {partition_bin}")

    with tempfile.NamedTemporaryFile(prefix="partition_", suffix=".csv", delete=False) as temp:
        csv_path = Path(temp.name)
    try:
        subprocess.run(
            ["gen_esp32part.py", str(partition_bin), str(csv_path)],
            cwd=build_dir, check=True, capture_output=True, text=True,
        )
        with csv_path.open("r", encoding="utf-8") as stream:
            lines = [line for line in stream if line.strip() and not line.lstrip().startswith("#")]
        entries = []
        for row in csv.reader(lines):
            if len(row) >= 5:
                entries.append({
                    "name": row[0].strip(),
                    "type": row[1].strip(),
                    "subtype": row[2].strip(),
                    "offset": row[3].strip(),
                    "size": row[4].strip(),
                })
        return entries
    finally:
        csv_path.unlink(missing_ok=True)


def extract_nvs_info(partition_table: list[dict]) -> dict[str, str] | None:
    for entry in partition_table:
        if entry["name"] == "nvs":
            return {
                "start_addr": hex_size(parse_size(entry["offset"])),
                "size": hex_size(parse_size(entry["size"])),
            }
    return None


def create_tarball(build_dir: Path, flasher_data: dict, output: Path) -> None:
    with tarfile.open(output, "w:gz") as archive:
        archive.add(build_dir / "flasher_args.json", arcname="flasher_args.json")
        archive.add(build_dir / "flash_args", arcname="flash_args")
        for relative_path in flasher_data["flash_files"].values():
            source_path, archive_path = validate_flash_file_path(build_dir, relative_path)
            archive.add(source_path, arcname=archive_path)


def main() -> int:
    build_dir = Path(os.getenv("FIRMWARE_BUILD_DIR", "build-ci")).resolve()
    output_dir = Path(os.getenv("FIRMWARE_OUTPUT_DIR", "firmware_output")).resolve()
    if not build_dir.is_dir():
        log(f"build directory not found: {build_dir}")
        return 1
    output_dir.mkdir(parents=True, exist_ok=True)

    try:
        flasher_data, flash_size = load_flasher_data(build_dir)
        sdkconfig = load_sdkconfig(build_dir)
        partition_table = extract_partition_table(build_dir, flasher_data)
        console_output = resolve_console_output(sdkconfig)
        nvs_info = extract_nvs_info(partition_table)

        safe_console = console_output.replace(" ", "_").lower()
        basename = f"{BOARD_NAME}__{safe_console}"
        tar_path = output_dir / f"{basename}.tar.gz"
        metadata_path = output_dir / f"{basename}.json"
        create_tarball(build_dir, flasher_data, tar_path)

        metadata = {
            "refs": git_refs(),
            "application": APPLICATION_NAME,
            "chip": TARGET,
            "brand": BOARD_BRAND,
            "board_name": BOARD_NAME,
            "console_output": console_output,
            "commit_timestamp": git_commit_timestamp(),
            "partition_table": partition_table,
            "flash_files": flasher_data["flash_files"],
            "flash_settings": flasher_data["flash_settings"],
            "min_flash_size": flash_size,
            "min_psram_size": 8 if sdkconfig.get("SPIRAM_XIP_FROM_PSRAM") is True else 4 if sdkconfig.get("SPIRAM") is True else 0,
        }
        if nvs_info:
            metadata["nvs_info"] = nvs_info
        metadata_path.write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        log(f"Packaged {tar_path} and {metadata_path}")
    except Exception as error:  # noqa: BLE001
        log(f"Packaging failed: {error}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
