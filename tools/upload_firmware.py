#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

import json
import os
import sys
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen


def log(message: str) -> None:
    print(message, file=sys.stderr)


def upload_multipart(url: str, api_key: str, tar_path: Path, metadata: dict) -> None:
    boundary = "----FirmwareUploadBoundary7MA4YWxkTrZu0gW"
    metadata_part = (
        f"--{boundary}\r\n"
        "Content-Disposition: form-data; name=\"metadata\"\r\n"
        "Content-Type: application/json\r\n\r\n"
        f"{json.dumps(metadata, ensure_ascii=False)}\r\n"
    ).encode()
    file_header = (
        f"--{boundary}\r\n"
        f"Content-Disposition: form-data; name=\"file\"; filename=\"{tar_path.name}\"\r\n"
        "Content-Type: application/gzip\r\n\r\n"
    ).encode()
    body = metadata_part + file_header + tar_path.read_bytes() + f"\r\n--{boundary}--\r\n".encode("ascii")

    request = Request(url, data=body, method="POST")
    request.add_header("Authorization", f"Bearer {api_key}")
    request.add_header("Content-Type", f"multipart/form-data; boundary={boundary}")
    try:
        with urlopen(request, timeout=120) as response:
            response_body = response.read().decode("utf-8", errors="replace")
            if not 200 <= response.status < 300:
                raise RuntimeError(f"HTTP {response.status}: {response_body}")
            log(f"Upload response: {response_body}")
    except HTTPError as error:
        response_body = error.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {error.code}: {response_body}") from None


def main() -> int:
    api_url = os.getenv("FIRMWARE_API_URL", "").strip()
    api_key = os.getenv("FIRMWARE_API_KEY", "").strip()
    output_dir = Path(os.getenv("FIRMWARE_OUTPUT_DIR", "firmware_output")).resolve()
    if not api_url:
        log("FIRMWARE_API_URL is not set")
        return 1
    if not api_key:
        log("FIRMWARE_API_KEY is not set")
        return 1
    if not output_dir.is_dir():
        log(f"firmware output directory not found: {output_dir}")
        return 1

    metadata_files = sorted(output_dir.glob("*.json"))
    if not metadata_files:
        log(f"no metadata JSON files found in {output_dir}")
        return 1

    succeeded = 0
    for metadata_path in metadata_files:
        tar_path = metadata_path.with_suffix(".tar.gz")
        if not tar_path.is_file():
            log(f"skip {metadata_path.name}: matching tar.gz not found")
            continue
        try:
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            log(f"Uploading {tar_path.name} ({metadata.get('board_name', '?')} / {metadata.get('console_output', '?')})")
            upload_multipart(api_url, api_key, tar_path, metadata)
            succeeded += 1
        except Exception as error:  # noqa: BLE001
            log(f"upload failed for {metadata_path.name}: {error}")

    if succeeded == 0:
        log("no uploads succeeded")
        return 1
    log(f"Uploaded {succeeded}/{len(metadata_files)} firmware package(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
