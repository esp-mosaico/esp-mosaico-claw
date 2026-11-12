#!/usr/bin/env python3
"""Package an existing ESP-IDF build as an ESP-Iris System Update bundle."""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Optional


ROOT = Path(__file__).resolve().parents[1]
UTILS = ROOT / "third-party/esp-mosaico-utils"


def run(script: Path, *arguments: object) -> None:
    subprocess.run([sys.executable, str(script), *map(str, arguments)], check=True)


def build_irisfw(build_dir: Path, output: Path, release: Optional[str] = None) -> Path:
    build = build_dir.resolve()
    description = json.loads((build / "project_description.json").read_text(encoding="utf-8"))
    flasher = json.loads((build / "flasher_args.json").read_text(encoding="utf-8"))
    version = release or description["project_version"]
    application = build / description["app_bin"]
    ui_apps = build / flasher["ui_apps"]["file"]
    system = build / flasher["system"]["file"]
    partition_table = build / flasher["partition-table"]["file"]
    destination = output.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="irisfw-") as temporary:
        stage = Path(temporary) / "firmware"
        run(UTILS / "mosaico-tools/tools/prepare_system_update.py",
            "--partition-csv", ROOT / "partitions_16MB.csv",
            "--partition-table", partition_table,
            "--application", application,
            "--ui-apps", ui_apps,
            "--data", f"system={system}",
            "--release", version,
            "--stage-dir", stage)
        iris = UTILS / "ESP-Iris/components/esp_iris/tools/esp_iris.py"
        run(iris, "bundle", "build", stage / "manifest.json", "--component-root", stage, "--output", destination)
        run(iris, "bundle", "inspect", destination)

    return destination


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--release")
    args = parser.parse_args()

    build = args.build_dir.resolve()
    description = json.loads((build / "project_description.json").read_text(encoding="utf-8"))
    output = args.output or build / f"{description['project_name']}-system-update.irisfw"
    bundle = build_irisfw(build, output, args.release)
    print(f"Created {bundle} ({bundle.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"Iris firmware packaging failed: {error}", file=sys.stderr)
        sys.exit(1)
