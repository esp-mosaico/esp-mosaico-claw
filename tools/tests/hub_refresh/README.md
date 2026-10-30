# Hub and pull-down regression checks

From the project root, after an IDF build has generated the Hub API headers:

```sh
python3 tools/tests/hub_refresh/run_tests.py
```

Set `MOSAICO_TEST_BUILD_DIR` when using an out-of-tree build. The suite runs the
production Hub callbacks, module-notice policy, backdrop and performance sampler
with controlled providers and ASan/UBSan. It covers:

- Motion deferral without losing samples, unchanged-value suppression and
  rejected-publication retries.
- Silent boot, real hotplug, left/right queuing, removal, stale generations,
  descriptor retry, held contact, sleeping UI and Done/Back completion.
- Introduction retention across idle screen-off and Open-target revalidation
  after returning from a child App.
- Backdrop pixels and callback lifetime after a failed teardown fence.
- Bounded sampling, counter deltas, cancellation, stale callbacks, allocation
  failure and timer completion during creation.

These checks execute product code, but do not certify physical hotplug, native
gesture composition or LCD behavior. The former GSP 1.8.0 scene-only simulator
harness is no longer part of the current 1.9.0 regression workflow.

## Device diagnostics

CLI builds expose these commands through the normal console transport:

```text
ui_perf start 3500
ui_perf read
```

Start from an active, awake Hub. Capture lasts 500–4000 ms and reports at most
256 render-task timer intervals, nearest-rank P50/P95/max, frame-count delta
and drag snapshot counters. A busy task can finish slightly late. Stopping or
suspending the UI cancels sampling; the probe runs only when explicitly started
and creates no drawing damage.

`render_task_service_gap` includes scheduler, render and submission delays.
It is not physical LCD latency, CPU utilization or measured power. Frame counts
include snapshots; interval samples can include idle periods. Dividing them
does not establish peak FPS or touch-to-photon latency.

Compare the same board MAC, SDK configuration, scene, panel settings, probe
version and input plan. Retain raw intervals and per-window statistics. Verify
that each gesture was accepted, began and released in the intended direction.
In GSP logs, `commit=1` accepts a direction change; it is not the final open state.
Rejected, truncated or partly drained synthetic input invalidates a performance
comparison. Controlled replay also does not replace fast physical gestures.

Use the workspace's `mosaico.py`, identify the device and preserve user data.
Shared-device testing needs a verified backup/restore and transport release.
Physical gestures, display stability, audio/network concurrency, sleep/wake and
battery current remain separate device acceptance checks.
