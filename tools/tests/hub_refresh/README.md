# Hub and pull-down regression checks

Run these from the project root after a normal build has generated the Hub API
headers:

```sh
python3 tools/tests/hub_refresh/run_tests.py
python3 tools/tests/ui_probe/run_tests.py
```

For an out-of-tree IDF build, set `MOSAICO_TEST_BUILD_DIR` to that build
directory. The Hub and UI-state tests use its matching generated API headers.

The first suite compiles the production Hub callbacks with controlled providers
and motion state, using ASan/UBSan. It checks that dragging/settling defers ordinary
refreshes without consuming pending samples, clock text only republishes when
needed, failed publications retry, and unchanged Wi-Fi bar levels avoid visibility
writes. Battery sampling and critical shutdown run separately from the UI timer.

The same suite exercises the production module-notice policy and Hub publication:
silent single/dual-board boot, initial empty-slot snapshots, the first real
hotplug, merging/expiry, descriptor retry, claim/release, unplug during feedback,
stale generations, inactive/sleeping UI, open Drawer and failed UI writes.
It checks one original single-board page per application, queued second insertions,
duplicate application targets, stale clicks, Done/Back animation completion,
queued removal and generation-checked publication acknowledgement.
An existing introduction survives idle screen-off; pending work does not replay.
The retained Hub page also keeps its Open target across child Apps and
revalidates a removal before accepting that target on return.
Queued applications do not expire while the current introduction is being read.
An eligible insertion also waits through a held contact without expiring;
unplugging before release cancels it instead of replaying a stale introduction.
These host checks do not certify physical hotplug or display behavior.

The same runner checks Hub backdrop teardown with an injected fence failure:
callback data stays alive and timer handles never cross into a replacement Hub.

The probe suite checks bounded sampling, measured gaps, counter deltas, cancellation,
stale callbacks, allocation failure, and timer completion during creation.

## Native component scenarios

```sh
python3 tools/tests/hub_refresh/drawer_scenarios.py \
  --host /path/to/sim-v1.8.0 \
  --gspc /path/to/gspc-v0.9.0 \
  --output /path/to/a/new/output-directory
```

Use the simulator matching the project's GSP runtime. The runner compiles the
generated target scene and profile with deployable metadata, and uses the matching
bundle-level API sidecar. The report records input/tool hashes and explicitly
identifies its evidence as scene-only. It exercises native input and component
motion/state; it does not execute the application's C callbacks or measure the LCD.

Checks include slow movement, short-pull cancellation, fast opening, closing from
the bottom edge, native brightness/volume inputs, reversal during settling,
12 open/close cycles, and accidental application callbacks. State, events and
settling are checked in addition to screenshots. All cases run even when reversal
fails, and any failed case makes the runner exit nonzero.

With the unpatched Registry runtime 1.8.0, reversal while opening is still
settling fails: the Drawer continues to open. The product applies the pinned
[compatibility fixes](../../gsp_fixes/README.md); a stock simulator does not
include those replacements. Record which implementation was tested and keep
the reversal assertion. Ordinary open/close success does not establish that
this case is fixed.

## Real-device diagnostics

CLI builds expose these commands through the project's normal console transport:

```text
ui_perf start 3500
ui_perf read
```

Start from the active, awake Hub. Capture lasts 500–4000 ms, and reports at most
256 render-task timer intervals, nearest-rank P50/P95/max, rendered frame-count
delta and drag snapshot counters. A capture can finish slightly after its requested
duration when the render task is busy. Stopping or suspending the UI cancels it.
The probe creates no drawing damage and runs only when explicitly started.

`render_task_service_gap` is an interval between callbacks on the render task.
It includes scheduler, render and submission delays; it is not physical LCD frame
latency, instantaneous CPU utilization, or a power measurement. The frame count
includes snapshots, while the interval samples can include idle periods. Dividing
these values does not establish a peak FPS or touch-to-photon latency.

For comparisons, use the same board MAC, SDK configuration, scene, panel settings,
probe version and input plan. Keep raw intervals and per-window statistics. Check
that every injected gesture was queued and actually began/released, in the correct
direction and alternating starting open state. In GSP gesture logs, `commit=1`
means accepting the direction change; it applies to both successful opening and
closing. It is not the terminal open state.

Dense synthetic input can outpace the touch playback queue on a slow renderer.
Rejected, truncated or partially drained input invalidates a performance comparison;
retain that run as a stress diagnostic. Slower controlled replay verifies repeatable
workloads but does not replace testing fast physical gestures.

Use the consuming workspace's `mosaico.py` for device operations. Identify the MAC,
back up data, keep a verified restore image, and close temporary testing by checking
the original image's startup/identity and releasing the transport. Do not erase
credentials or user content to make a test pass.

Product acceptance still needs physical gestures, LCD timing/observation, playback
and network workloads, sleep/wake transitions, and measured current. These are
separate from passing the host and scene checks above.
