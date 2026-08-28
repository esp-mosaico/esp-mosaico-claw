#!/usr/bin/env python3
"""Resolve and cache the prebuilt GSPC executable for the build host."""

from __future__ import annotations

import argparse
import os
import platform
import sys
import tempfile
import urllib.request
from pathlib import Path


VERSION = "v0.2.4"
BASE_URL = f"https://dl.espressif.com/AE/gsp/gspc/{VERSION}/"
LICENSE_NAME = "THIRD_PARTY_LICENSES.txt"
ARTIFACTS = {
    ("Linux", "x86_64"): f"gspc_{VERSION}_x86_64-unknown-linux",
    ("Linux", "amd64"): f"gspc_{VERSION}_x86_64-unknown-linux",
    ("Linux", "aarch64"): f"gspc_{VERSION}_aarch64-unknown-linux",
    ("Linux", "arm64"): f"gspc_{VERSION}_aarch64-unknown-linux",
    ("Windows", "AMD64"): f"gspc_{VERSION}_x86_64-pc-windows-gnu.exe",
    ("Windows", "x86_64"): f"gspc_{VERSION}_x86_64-pc-windows-gnu.exe",
    ("Windows", "amd64"): f"gspc_{VERSION}_x86_64-pc-windows-gnu.exe",
    ("Darwin", "x86_64"): f"gspc_{VERSION}_universal2-apple-darwin",
    ("Darwin", "arm64"): f"gspc_{VERSION}_universal2-apple-darwin",
    ("Darwin", "aarch64"): f"gspc_{VERSION}_universal2-apple-darwin",
}


def artifact_name() -> str:
    system = platform.system()
    machine = platform.machine()
    try:
        return ARTIFACTS[(system, machine)]
    except KeyError as error:
        supported = ", ".join(
            f"{host}/{arch}" for host, arch in sorted(ARTIFACTS)
        )
        raise RuntimeError(
            f"unsupported GSPC build host: {system}/{machine}; "
            f"supported hosts: {supported}"
        ) from error


def download_cached(destination: Path, url: str, *, executable: bool) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_dir():
        raise RuntimeError(f"GSPC cache path is a directory: {destination}")
    if destination.is_file():
        return

    fd, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.", suffix=".download",
        dir=destination.parent,
    )
    temporary = Path(temporary_name)
    try:
        request = urllib.request.Request(
            url,
            headers={"User-Agent": "esp-mosaico-gspc-fetcher/1"},
        )
        with os.fdopen(fd, "wb") as output:
            with urllib.request.urlopen(request, timeout=120) as response:
                while True:
                    chunk = response.read(1024 * 1024)
                    if not chunk:
                        break
                    output.write(chunk)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, destination)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise

    if executable and os.name != "nt":
        destination.chmod(0o755)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    name = artifact_name()
    destination = args.output_dir.resolve() / name
    license_path = args.output_dir.resolve() / LICENSE_NAME
    try:
        if destination.is_file():
            print(f"mosaic: using cached GSPC {destination}", file=sys.stderr)
        else:
            executable_url = BASE_URL + name
            print(
                f"mosaic: downloading GSPC from {executable_url}",
                file=sys.stderr,
            )
            download_cached(destination, executable_url, executable=True)

        if license_path.is_file():
            print(
                f"mosaic: using cached GSPC license {license_path}",
                file=sys.stderr,
            )
        else:
            license_url = BASE_URL + LICENSE_NAME
            print(
                f"mosaic: downloading GSPC license from {license_url}",
                file=sys.stderr,
            )
            download_cached(license_path, license_url, executable=False)
    except Exception as error:
        print(f"mosaic: failed to prepare GSPC: {error}", file=sys.stderr)
        return 1

    print(destination)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
