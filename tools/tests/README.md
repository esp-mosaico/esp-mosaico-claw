# Host regression checks

Run from the repository root with Python 3 and a C compiler supporting ASan/UBSan.
Resolve managed dependencies and build the firmware first; Hub/UI-state checks
use its generated headers. For an out-of-tree build:

```sh
export MOSAICO_TEST_BUILD_DIR=/path/to/build
python3 tools/tests/camera_session/run_tests.py
python3 tools/tests/hub_refresh/run_tests.py
```

Run all suites and the scene/tool generation checks:

```sh
for suite in tools/tests/*/run_tests.py; do
    python3 "$suite" || exit 1
done
python3 -m unittest discover -s components/mosaic_ui/tools/tests -p test_generation.py
```

| Suite | Regression boundary |
| --- | --- |
| `battery_charge` | Main-board profile transactions, learned capacity and failure rollback |
| `camera_roi` | Real sensor tables, source window, borrowed storage and I/O cleanup |
| `camera_session` | Preview/capture ownership, retry, save failures and stop fences |
| `endpoint_config` | Endpoint configuration validation and persistence |
| `hub_refresh` | Hub refreshes, module notices, backdrop and performance sampler |
| `imu_filter` | Production filter convergence across sampling intervals and read errors |
| `mosaic_runtime` | Deferred work, visit generations, recovery and shutdown ownership |
| `platform_lifecycle` | GSP 1.9.0 Session-result ownership, trim and pause failures |
| `power_platform` | Idle/wake policy and checked pause/resume failure recovery |
| `recovery_seed` | Seed installation preserves existing user files |
| `setup_network` | Provisioning retries and asynchronous scan-cache recovery |
| `trial_provider` | Trial provider configuration and error handling |
| `ui_state` | Battery/Settings/Weather callbacks and Album scene defaults |
| `weather_location` | City parsing/persistence and stale-response rejection |

Shared Host-only ESP timer/critical-section stubs live in `include/`. They do
not model real scheduling or interrupt exclusion. Test sources and fixtures are
not linked into firmware, so pruning them does not change device RAM/Flash use.

The Camera ROI suite exercises the real implementation with resolved sensor
tables; SDK compatibility is checked by the firmware's actual CMake configure,
compile and link against the pinned release. Synthetic older-SDK fixtures are
outside the supported product matrix.

Passing Host checks is not device acceptance for image quality, physical display
timing, audio continuity, current consumption or long-running memory behavior.
See [Hub diagnostics](hub_refresh/README.md) for measurement limits.
