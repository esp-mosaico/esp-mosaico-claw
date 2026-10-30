# Mosaic UI assets

The current product pins ESP-GSP 1.9.0 with GSPC 0.10.0 and Presenter ^1.2.0.
It uses the released Session trim/ownership-result APIs and enables the supported
SC101IOT equivalent window. The historical validation notes below distinguish
earlier 1.8.0 and prototype results from this integration. The two retained
input/backdrop adapters are described in [the adapter notes](../../tools/gsp_fixes/README.md).

Hub and every App remain independent source and intermediate `.gspb` packages.
Product builds add one shared Font Catalog to the same `ui_apps` mmap partition:

- `hub/generated` and `apps/*/generated` are self-contained intermediates.
- `common/generated/linked` contains product bundles after font linking.
- `common/generated/common-fonts.gspb` is the Launcher-owned Catalog.
- `common/generated/font-link-report.md` records dependencies and size savings.

Run `python regenerate_font_catalog.py --gspc /path/to/gspc` from this directory
after changing a scene (`gspc.exe` on Windows). It regenerates every
intermediate and rebuilds all linked entries, offsets, alignment, and CRC; it
does not append bytes to an existing binary.

For a single app, run, for example:

```text
python tools/generate_bundle.py --module-dir apps/settings --gspc /path/to/gspc
```
Configuration checks Pillow and the `esp-gsp-tools` API independently. If the compiler manager is missing or incompatible, run
`python -m pip install -U esp-gsp-tools` with that same interpreter. GSPC is a
separate executable whose version is selected by the component; upgrading the
manager does not replace a missing compiler release for the host platform.

The product currently pins GSP 1.8.0 with GSPC 0.9.0. Its ESP32-S31 compatibility
objects are built from a hash-checked source closure in
[`tools/gsp_fixes`](../../tools/gsp_fixes/README.md) and replace members only in
the build archive. Update the pin, patch manifest and regression evidence
together when adopting a newer runtime. Editing `managed_components` is not
part of this workflow.

The Loader opens the Catalog once and passes it to each App Context. Context
creation resolves local fonts first and Catalog fonts second. Rendering retains
a direct scene-local `font_ref` table and performs no Catalog lookup per frame.
Standalone/third-party Apps keep the default `embedded` policy. Mosaic scenes
use `auto`; large numeric and App-specific fonts remain embedded unless another
App has the exact same Font Asset ID.

Scene generators describe each font size with one of three charset policies:

- `shared_charset(...)` keeps a stable common set, normally printable ASCII at
  16/18px, so different Apps produce the same Catalog Asset ID.
- `auto_charset()` collects only static label text and the charsets declared by
  dynamic labels at that font and size.
- `explicit_charset(...)` documents a deliberately small dynamic set, such as
  `:0123456789` for a clock or `-.0123456789` for a sensor value.

Dynamic labels must still carry `font_charset` on the label itself. Asset-only
scenes do not seed fonts; this prevents their build from replacing the primary
scene's per-size subsets with a broad fallback set.

## Hub navigation and Lock Screen

`hub_stack` contains only ordinary navigation: Launcher, Notification Center,
and the extension-board introduction. Launcher owns its nested PageFlow.
AOD and CHRG are two mutually exclusive modes inside the shared native Drawer;
they are not StackView pages. The Drawer owns drag/settle input and composition.
Closing it resumes the retained StackView/PageFlow state.

Module discovery at boot is silent. After each slot has a stable baseline, a
confirmed absent-to-present transition schedules the original side animation
and single-board introduction on the awake launcher. Left/right insertions
share a 400 ms feedback window while keeping the original Open/Done layout.
Different application targets queue until the current page and its Done/Back
animation finish; equal targets share one notice. Removing a board cancels its
queued entry and invalidates its Open target. Descriptor retries and ownership
restoration do not count as new insertions.

App changes, an open Drawer, Lock Screen and screen-off discard pending feedback;
brief rendering/input motion defers it. An already open introduction is retained
across idle screen-off. The existing UI timer drives the policy, with two fixed
slot snapshots and no new task, persistent setting or I2C scan. Discovery remains
active regardless of whether an introduction can be displayed.

Idle screen-off pauses the foreground App without opening Lock Screen. Music
and Bluetooth background ticks continue, and an active camera preview inhibits
screen-off. Waking retains the current page; the waking contact is consumed
until release. `ui_perf` captures bounded render-task service gaps on the active
App; see [diagnostic limits and replay](../../tools/tests/hub_refresh/README.md).

## Product closeout snapshot (2026-10-28)

The changes cover retained idle/wake state, background audio ticks, camera
preview keep-awake, playback-state icons, empty Album actions, live Battery rows,
provisioning scan retries, time-based IMU correction, and manual Weather city
selection with stale-response protection. Hub refreshes avoid unchanged writes
and defer ordinary updates during Drawer motion. Battery sampling runs outside
the UI timer; seed upgrades preserve existing user files. Charge thresholds
remain unchanged until characterized, and DFS/automatic light sleep remain off.

The final source passed 11 host regression groups (ASan/UBSan where applicable)
and 8 generation tests. Five native scene checks passed for module feedback;
they exercise scene input/composition, without the application's C callbacks.
Exporting the committed GSP inputs also rebuilt all four replacement objects
and the patched archive independently of the existing build output.
The ESP32-S31 build fits `main_app`. The product remains version 0.6.0;
GSP changes from the previous `^1.3.0` constraint to pinned 1.8.0 with reviewed
build-local fixes, and GSPC is 0.9.0. Protocol and partition layout are unchanged.

The original closeout's ACM5 validation used MAC `30:ed:a0:f4:60:4e`, CameraBoard
on the left and InteractionBoard on the right. That candidate ELF SHA-256 was
`6473687ddb5a45732409be1a8b5f30604dba06bfdb497828d8c0c9a4eb23af1f`.
Dual-board boot, ordinary idle screen-off, consumed wake contact, subsequent
Camera entry, and 34 seconds of preview without idle screen-off passed automated
device checks. All 37 user files retained their original SHA-256 values.
Battery telemetry was fresh; USB-powered telemetry is not a current or charge
calibration measurement.

Whole-product acceptance is still pending:

| Finding | Evidence and remaining work |
| --- | --- |
| Camera resource failure (P1) | After six admitted Settings enter/exit cycles, the 1280×720 DVP backup allocation fails and retries. The previous binary also reproduced this failure class. Investigate simultaneous owners and contiguous DMA-capable memory; total free PSRAM does not establish capacity or a leak. |
| Drawer fluency (P2) | Four windows / eight queued and released swipes measured render-task service-gap P95 83.581–84.000 ms, max 105.778 ms; these are not LCD frame times. Native snapshot attempts were 8, started 0. Investigate admission and composition stalls. |
| Settings timeouts (P2) | QR canvas publication and snapshot refresh returned timeouts during navigation; focused reproduction is still required. |
| Initialization warnings (P3) | Duplicate GPIO ISR-service initialization and GPIO45 ownership warnings occur on previous and current firmware; audit ownership separately. |

The device run above predates later detection refinements; its timing and
acceptance apply to the recorded ELF rather than every later commit.

Physical simultaneous dual-board hotplug, LCD tearing and touch-to-photon timing,
phone Bluetooth continuity, battery-powered current and
endurance, full-charge calibration, and long-term heap behavior remain unverified.
Local commits preserve the tested work; they do not establish release acceptance.

## Frozen scope (2026-10-29)

The parent snapshot was frozen for local closeout, not whole-product release
acceptance. Its PSRAM experiments were separate candidates: Camera RAW/MMAP
saved 450 KiB of decoded payload, and omitting unused Breakout dynamic-font
state saved about 106 KiB in that workload. Neither was included in that snapshot.
GSP remains pinned to the reviewed archive with its existing compatibility fixes.

On ACM5 (`30:ed:a0:f4:60:4e`, SC101IOT connected), four diagnostic controls each
admitted six repeated Camera visits. After PageFlow and Drawer exercise, all
four failed the Camera DVP backup allocation; a further RAW/MMAP icon candidate
also reproduced it. The controls isolated console-history allocations from
PSRAM. The first failed request was 1,843,200 bytes with SPIRAM | DMA caps.
The generic largest-block reading was sampled after rollback and is rounded;
it does not identify the failure-time layout or an exact memory shortfall.
Later tracing identified the surviving native input, preview outputs, TE
compose buffers and viewport atlas; total free memory alone cannot describe
their allocation-time layout.

The SDK audit found a bounded, reusable 795 KiB PageFlow atlas that is released
on Host destruction in its native tests. No SDK correctness defect or leak was
confirmed. A public controlled trim operation was proposed separately from
those correctness fixes; it was not part of this frozen product baseline.

## Camera capture follow-up (2026-10-29)

Capture admission now requires a converted preview frame. A request remains
busy through encoding and saving, and its completion or failure is published
to the existing Camera status text by the loader's 250 ms App tick. No extra
task or GSP timer is created. File creation is exclusive, failed new files are
removed, and names include uptime and a sequence to protect same-second shots.
Preview allocation rolls back newly allocated outputs if the pair cannot be
completed. Resolution, JPEG quality, preview/TE buffer counts and scene layout
are unchanged.

Camera static images now use RAW/MMAP while retaining the small compressed
thumbnail seed. Matched-source Host checks produced byte-identical pixels in
18 states, including the capture notices. Decoded Camera payload drops from
517,680 to 4,608 bytes. The UI image grows by 512,192 bytes to 3,267,920, leaving
8,880 bytes in the existing partition; the asset build enforces this limit.
Other application bundles and the shared font catalog are unchanged.

The pinned GSP 1.8.0 archive still lacks the new public cache-trim operation;
RAW alone still failed the warm startup control. These changes therefore do not
resolve warm Camera startup by themselves. A matched
local SDK prototype released the inactive 814,080-byte atlas after Hub suspend
and before Camera App creation. In one same-ELF off/on/off device experiment,
both off controls reproduced the startup allocation failure; on admitted all
14 App-return routes and 24 ordinary JPEGs decoded completely at 480x480.
That result applies to its recorded workload: a later return path admitted
preview but failed the additional 460,800-byte JPEG allocation. RAW/MMAP was
added to address this separate peak. Prototype SDK libraries and diagnostic
hooks remain outside the product checkout.

Subsequent same-binary RAW+trim diagnostics covered all 14 default App-return
routes across separate runs: preview, saved JPEG and continued preview passed.
Controlled allocation, encoder, encoding, destination, short-write and close
failures allowed a successful retry without retaining a bad new file. Flash
capture, failed-flash-save recovery and existing-file protection also passed.
All 27 recorded JPEGs, including controls and two UI shutter captures, decoded
completely at 480x480 across these same-candidate runs. Sequential completed shots are
valid and are not treated as overlapping saves. Matched Host checks additionally
exercise readiness, cancellation, capture sequence wrap and 500 unique ordered
names at the same time value.

The first return to Hub started viewport snapshots in both sampled windows
(1 and 2 starts, 40 and 41 software updates; service-gap P95 34.016 and
35.906 ms). Page/offset samples confirm movement, but some were captured during
dragging rather than after a committed page change. This is not physical LCD
frame timing or an acceptance claim for continuous bidirectional finger drag.

Formal SDK adoption must preserve Session ownership on failed recovery, including
an empty Session after a pre-start trim failure. The product lifecycle handling
below now covers these failures; normal-path prototype results alone do not
validate them.

SDK adoption must first establish which existing fixes are superseded, then
repeat warm Camera admission, first-return swipe/Drawer performance and the
relevant application regressions. Preserve the original detection UI and buffer
strategy. Do not delete compatibility objects or claim release acceptance from
the SDK merge alone; the physical, audio and battery checks above remain open.

## Stability integration (2026-10-29)

Camera completes native open and STREAMON before reserving its two RGB565
preview outputs. STREAMON allocates the DVP backup; opening the device alone
does not establish the complete native budget. A separate JPEG workspace was
removed after repeated full-photo Album visits exposed allocation failures:
2.64 MiB remained free, but the largest allocatable block was only 448 KiB,
smaller than the third 450 KiB working buffer.

During a photo, Camera publishes the captured RGB frame and waits up to one
second for GSP to release the other preview buffer. Only that released buffer
is lent to the JPEG encoder, then returned to the capture task. The captured
source remains immutable while display and encoder read it. Recognition does
not draw onto that published capture frame; it resumes on the next preview.
JPEG output is never published as RGB. This removes one 460,800-byte allocation
from steady preview and capture peak, retaining preview double buffering,
480x480 JPEGs, quality 90 and the existing scene geometry. Rejected publication,
retirement timeout and stop cancel safely; encoder/I/O failures remain reported.
This saving does not guarantee the native high-resolution STREAMON budget under
fragmentation. The default native profile remains unchanged; a smaller profile
is being compared on-device and needs image review before default adoption.

Initialization retries three times with 500 ms spacing. Before another native
open, a failed start stops the preview canvas, waits for its flush and release
callbacks, then frees the preview buffers. A transient first-frame
timeout must not leave that workspace fragmenting the next native allocation.
While the module/UI is available, valid-frame loss uses a 10-second deadline
that survives native reopen attempts. Successful frames refresh it; waiting for
an unavailable module begins a new window. Exhausted retries expose
`Tap shutter to retry` in the existing status area. The shutter first completes
checked cleanup before restarting. Status writes rejected by a busy GSP queue
retry through the existing App tick, always using the latest desired text.

Deferred loader work uses four FIFO slots and runs at most one callback per
owner-task iteration after navigation commands drain. This prevents a pending
Setup cancellation from replacing or rejecting the following Camera start.
Camera start/retry callbacks carry a visit generation, so old work cannot act
on a later visit. Waiting work shows a loading status; an inactive session whose
start never ran remains retryable. Recognition and flash intent are restored
after successful checked restart.

Camera stop returns a status. A task timeout, failed canvas command/flush or
borrowed pixel retains buffers and synchronization objects for another close
attempt. The runtime waits for that boundary before destroying Camera UI.
Ordinary save failures release the borrowed output slot and preserve existing files.
The task also publishes its native close result. An SCCB/STREAMOFF error cannot
declare Camera closed or release its power vote: the owner retries native close
before retiring the session. Persistent failure retains the session and exposes
a Back retry hint. Device close gates inspect the public native streaming,
buffer and outstanding-frame counters, separately from the preview workspace.

The ESP platform retains callback owners, empty Sessions, pause tokens and shared
font/MMAP sources when recovery or destruction fails. Retained Hub callback
removal needs a successful flush before its wrapper is freed. An incomplete
screen pause cannot be promoted to a completed screen-off or presenter handoff.
The owner task makes up to three recovery attempts at 500 ms intervals; explicit
navigation can rearm recovery. Stopped logic and stale-generation events are
not dispatched while recovery is pending.

When the SDK header declares public trim APIs, Hub entry trims the inactive
snapshot before Camera creation. Direct Album-to-Camera replacement instead
pauses the exact new Camera UI, trims its shared atlas and resumes it before
native START. A failed resume retains that preparation pause with the App.
These low-frequency synchronization points can increase entry/return latency;
scene geometry, detection effects, capture quality and display buffers remain
unchanged.

The optional public Session result API is detected through
`ESP_GSP_ESP_LCD_SESSION_RESULT_INIT`. Its `_ex` operations update the owned
Session pointer and explicitly distinguish retained/empty/consumed outcomes.
Legacy replacement errors cannot always distinguish those lifetimes. Such an
ambiguous error preserves both possible callback owners and blocks further
lifecycle operations; it cannot be advertised as fully recoverable on 1.8.0.
The normal legacy success path remains available for the Host compatibility
checks. The product now pins 1.9.0 and uses its public trim/result APIs. Drawer
and viewport repairs come from that release; only the product wake-input and
Hub backdrop bridges remain as exact-version adapters.

The product retains PSRAM XIP. Disabling it is a separate system configuration
change: PSRAM task stacks and NOR/NVS concurrency require checking the actual
Flash suspend and cache handling paths. Passing Camera tests alone does not
establish that configuration's system safety.

The optional SC101IOT equivalent sensor window is documented in
[the camera service](../mosaico_camera_service/README.md). It retains the
768x720 source area already consumed by preview/photo conversion while reducing
native capture-buffer memory. It defaults on with the pinned 1.9.0 release and
requires its public GSP trim and Session ownership-result APIs. Earlier ROI-only
tests on 1.8.0 failed repeated full-photo Album returns; historical prototype
results and the released SDK's device regression remain separate evidence.

Focused host ownership and failure gates:

```sh
python3 tools/tests/camera_session/run_tests.py
python3 tools/tests/platform_lifecycle/run_tests.py
python3 tools/tests/mosaic_runtime/run_tests.py
python3 tools/tests/camera_roi/run_tests.py
```

These exercise controlled failures with sanitizers. They do not establish
physical display quality, Bluetooth audio concurrency or long-term device
acceptance. Target candidates and their device evidence are tracked separately.

## Bounded scrolling updates (2026-10-30)

Settings root-list refreshes now compare the values actually shown in its rows.
Duplicate model notifications keep the scrolling snapshot intact; changed values
still refresh immediately. Cache state resets for a new App visit. Rejected
commands and asynchronous row-binding failures retry, including failures between
command enqueue and cache assignment, using the existing App tick.

The Hub backdrop coalesces contiguous full-width RGB565 tiles into one copy.
Cropped, padded and empty tiles retain the original pixel behavior and cleanup
fences. No task, image buffer or SDK patch is added by these changes.

The display staging height is 16 rows, retaining TE and two compose buffers.
Compared with 10 rows, two staging buffers require 11,520 additional bytes.
Same-board software service-gap samples improved Settings list scrolling while
PageFlow remained within the original controls. The 20/30-row experiments were
rejected after PageFlow service gaps worsened; Drawer had no clear measured gain.
These measurements do not establish physical LCD FPS or touch-to-photon latency.

For host checks using an out-of-tree IDF build:

```sh
MOSAICO_TEST_BUILD_DIR=/path/to/build python3 tools/tests/ui_state/run_tests.py
MOSAICO_TEST_BUILD_DIR=/path/to/build python3 tools/tests/hub_refresh/run_tests.py
```
