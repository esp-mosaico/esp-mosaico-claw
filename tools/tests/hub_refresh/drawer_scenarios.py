#!/usr/bin/env python3
"""Replay native Drawer gestures on the built Hub scene; no C backend is attached."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=Path, required=True)
    parser.add_argument("--gspc", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    build = (arguments.build_dir or root / "build").resolve()
    output = arguments.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    target_bundle = build / "esp-idf/mosaic_ui/mosaic_gen/hub/hub_480.gspb"
    scene = root / "components/mosaic_ui/hub/scene/mosaic_hub_480.json"
    profile = root / "components/mosaic_ui/common/mosaic_rgb565_auto.yaml"
    bundle = output / "hub-observable.gspb"
    generated = output / "generated"
    with (output / "compile.log").open("wb") as compile_log:
        subprocess.run([str(arguments.gspc.resolve()), "build", str(scene), "--scene-id", "0",
                        "--profile", str(profile),
                        "--deployable", "--bundle", str(bundle), "-o", str(generated)],
                       cwd=scene.parent, stdout=compile_log, stderr=subprocess.STDOUT, check=True)
    header = generated / "mosaic_hub_objects.h"
    key = int(re.search(r"#define GSP_OBJ_KEY_QUICK_DRAWER (0x[0-9A-Fa-f]+)",
                        header.read_text()).group(1), 16)
    component = root / "managed_components/espressif__esp-gsp"
    sys.path.insert(0, str(component / "tools/sim_bridge"))
    from rpc_client import Peer

    report = {"bundle": str(bundle), "bundle_sha256": hashlib.sha256(bundle.read_bytes()).hexdigest(),
              "target_bundle_sha256": hashlib.sha256(target_bundle.read_bytes()).hexdigest(),
              "scene_sha256": hashlib.sha256(scene.read_bytes()).hexdigest(),
              "profile_sha256": hashlib.sha256(profile.read_bytes()).hexdigest(),
              "api_sha256": hashlib.sha256(bundle.with_suffix(".api.json").read_bytes()).hexdigest(),
              "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "compiler": str(arguments.gspc.resolve()),
              "compiler_sha256": hashlib.sha256(arguments.gspc.read_bytes()).hexdigest(),
              "simulator": str(arguments.host.resolve()),
              "simulator_sha256": hashlib.sha256(arguments.host.read_bytes()).hexdigest(),
              "evidence_layer": "scene-only simulator; matching scene/profile recompiled with deployable metadata for state queries; excludes application C callbacks and LCD timing",
              "cases": []}
    log = (output / "simulator.log").open("wb")
    process = subprocess.Popen([str(arguments.host.resolve()), "--bundle", str(bundle),
                                "--headless", "--frames", "0", "--api-listen", "stdio",
                                "--api-json", str(bundle.with_suffix(".api.json")),
                                "--input-mode", "api-exclusive", "--fail-on-error"],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log)
    peer = Peer(process.stdout, process.stdin)
    trace = (output / "rpc.jsonl").open("w")

    def call(method, parameters=None):
        response = peer.call(method, parameters, timeout=20)
        trace.write(json.dumps({"method": method, "params": parameters, "response": response}) + "\n")
        trace.flush()
        assert "error" not in response, response
        value = response["result"]
        if isinstance(value, dict) and "result_code" in value:
            assert value["result_code"] == 0, value
        return value

    def pointer(x, y, pressed, frames=1):
        call("feed_pointer", {"x": x, "y": y, "pressed": pressed})
        if frames:
            call("wait", {"frames": frames})

    def motion():
        return call("component_get_motion", {"component_key": key})

    def settle(expected, cursor=None):
        parameters = {"component_key": key, "value": expected, "max_frames": 90}
        if cursor is not None:
            parameters["after_event"] = cursor
        value = call("wait_component", parameters)
        assert value["status"] == "settled", value
        state = value["state"]
        assert state["value"] == expected and not state["dragging"] and not state["settling"], value
        if cursor is not None:
            assert value["event_observed"], value
        return value

    def capture(name):
        return call("screenshot", {"path": str(output / name), "format": "png"})

    def gesture(y1, y2, steps, *, hold=0, sample=False):
        before = motion()
        pointer(240, y1, True)
        saw_drag = False
        sequences = []
        for index in range(1, steps + 1):
            y = round(y1 + (y2 - y1) * index / steps)
            pointer(240, y, True)
            state = motion()["state"]
            saw_drag |= state["dragging"]
            if sample:
                sequences.append(call("frame_info")["seq"])
                if index == steps // 2:
                    capture("drag-midpoint.png")
        if hold:
            call("wait", {"frames": hold})
        pointer(240, y2, False)
        return {"cursor": before.get("event_cursor"), "saw_drag": saw_drag,
                "frame_sequences": sequences}

    try:
        capabilities = call("capabilities")
        report["capabilities"] = capabilities
        assert capabilities["runtime_version"] == "1.8.0", capabilities
        assert capabilities["width"] == capabilities["height"] == 480, capabilities
        assert capabilities.get("component_motion_version") == 1, capabilities
        assert capabilities.get("component_event_cursor_version") == 1, capabilities
        call("subscribe", {"events": ["callback"]})
        settle(0)
        capture("closed.png")

        opening = gesture(5, 450, 36, sample=True)
        assert opening["saw_drag"], opening
        changed = sum(a != b for a, b in zip(opening["frame_sequences"], opening["frame_sequences"][1:]))
        assert changed >= 25, opening
        observed = settle(1, opening["cursor"])
        capture("open.png")
        report["cases"].append({"name": "slow open tracks movement and settles open", "pass": True,
                                 "input_samples": 36, "changed_frame_intervals": changed,
                                 "settle": observed})

        closing = gesture(477, 15, 24)
        assert closing["saw_drag"], closing
        observed = settle(0, closing["cursor"])
        report["cases"].append({"name": "close gesture starts at the bottom edge", "pass": True, "settle": observed})

        # About 60 px/s, then a pause longer than the release-velocity grace;
        # this tests cancellation rather than an intentional opening flick.
        cancelled = gesture(5, 65, 60, hold=30)
        observed = settle(0, cancelled["cursor"])
        report["cases"].append({"name": "short slow pull cancels back to closed", "pass": True, "settle": observed})

        opened = gesture(5, 450, 5)
        observed = settle(1, opened["cursor"])
        report["cases"].append({"name": "fast full pull settles open", "pass": True, "settle": observed})

        for name, x in (("quick_brightness_input", 410), ("quick_volume_input", 306)):
            pointer(x, 440, True)
            for y in (410, 380, 350, 320, 310):
                pointer(x, y, True)
            pointer(x, 310, False)
            level = call("get_property", {"name": name, "property": "value"})
            assert 80 <= level["value"] <= 100, level
            settle(1)
            report["cases"].append({"name": name + " changes without closing the Drawer",
                                     "pass": True, "native_value": level["value"],
                                     "scope": "native input routing; excludes C fill mirror and hardware setter"})
        call("drawer_close", {"component_key": key, "animated": False})
        settle(0)

        before = motion()
        pointer(240, 5, True)
        pointer(240, 350, True, frames=3)
        pointer(240, 350, False, frames=0)
        reversing_state = motion()["state"]
        assert reversing_state["settling"], reversing_state
        # Re-grab inside the already revealed panel, not on its moving edge.
        pointer(240, 150, True)
        for y in (120, 90, 60, 30, 5):
            pointer(240, y, True)
        pointer(240, 5, False)
        try:
            observed = settle(0, before.get("event_cursor"))
            report["cases"].append({"name": "immediate reverse closes the opening Drawer", "pass": True,
                                     "state_before_reverse": reversing_state, "settle": observed})
        except AssertionError as error:
            report["cases"].append({"name": "immediate reverse closes the opening Drawer", "pass": False,
                                     "state_before_reverse": reversing_state, "error": str(error)})
            call("drawer_close", {"component_key": key, "animated": False})
            settle(0)

        for cycle in range(12):
            opened = gesture(5, 450, 4)
            settle(1, opened["cursor"])
            closed = gesture(477, 5, 4)
            settle(0, closed["cursor"])
        capture("closed-after-cycles.png")
        report["cases"].append({"name": "repeated open/close returns to a stable state", "pass": True, "cycles": 12})
        callbacks = [n for n in peer.take_notifications() if n.get("method") == "callback"]
        assert not callbacks, callbacks
        report["cases"].append({"name": "Drawer gestures do not activate application controls", "pass": True})
        report["status"] = "passed" if all(case["pass"] for case in report["cases"]) else "failed"
        call("quit")
        process.wait(timeout=10)
        assert process.returncode == 0
    except BaseException as error:
        report["status"] = "failed"
        report["error"] = repr(error)
        raise
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        trace.close()
        log.close()
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"status": report["status"], "cases": len(report["cases"]), "report": str(output / "report.json")}))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
