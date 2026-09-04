from __future__ import annotations

import json
from pathlib import Path
import runpy
import subprocess
import sys
import unittest


REPOSITORY = Path(__file__).resolve().parents[2]
TOOL_ROOT = REPOSITORY / "third-party" / "esp-mosaico-tools"
sys.path.insert(0, str(TOOL_ROOT / "tools"))

from mosaico_cli.project import resolve_project
from mosaico_cli.registry import select_model
from mosaico_cli.workspace import load_workspace


class ToolSubmoduleIntegrationTests(unittest.TestCase):
    def test_system_update_authorizes_the_legacy_whole_ota_layout(self) -> None:
        namespace = runpy.run_path(str(REPOSITORY / "tools" / "prepare_system_update.py"))
        target = "1c8c4109ee5232ee43508eef3f60bd127de9349fab991b7722a84a4f25889f4c"
        self.assertEqual(
            namespace["authorized_source_layouts"](target),
            [
                target,
                "068246e2e1f05b063b0c5cef5ee80633b1a9e0dee834dd8eb91249b1e84b0ed2",
            ],
        )

    def test_workspace_resolves_claw_resources(self) -> None:
        workspace = load_workspace(TOOL_ROOT, explicit=str(REPOSITORY))
        model = select_model(workspace, None)

        self.assertEqual(workspace.root, REPOSITORY)
        self.assertEqual(
            workspace.esp_iris_path, REPOSITORY / "third-party" / "esp-iris"
        )
        self.assertEqual(
            workspace.build_runner,
            TOOL_ROOT
            / "skills"
            / "idf-low-noise-build"
            / "scripts"
            / "idf_low_noise_build.py",
        )
        self.assertEqual(
            workspace.resolve(model.recovery_project),
            REPOSITORY / "tools" / "mosaico_recovery",
        )
        self.assertEqual(
            workspace.resolve(model.recovery_dir),
            REPOSITORY / "prebuilt" / "recovery",
        )

    def test_root_is_selected_as_the_default_application(self) -> None:
        workspace = load_workspace(TOOL_ROOT, explicit=str(REPOSITORY))
        self.assertEqual(resolve_project(workspace, None, REPOSITORY), REPOSITORY)

    def test_root_launcher_uses_pinned_tool_checkout(self) -> None:
        result = subprocess.run(
            [sys.executable, str(REPOSITORY / "mosaico.py"), "--version"],
            cwd=REPOSITORY,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertRegex(result.stdout.strip(), r"^mosaico\.py \d+\.\d+\.\d+$")

    def test_workspace_file_uses_the_supported_schema(self) -> None:
        value = json.loads((REPOSITORY / ".mosaico.json").read_text(encoding="utf-8"))
        self.assertEqual(value["schema_version"], 1)
        self.assertTrue(value["devices"])


if __name__ == "__main__":
    unittest.main()
