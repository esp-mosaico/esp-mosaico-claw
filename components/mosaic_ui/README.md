# Mosaic UI assets

The product pins ESP-GSP 1.9.0 with GSPC 0.10.0 and Presenter ^1.2.0.
It uses the released Session trim and ownership-result APIs. The remaining
product input/backdrop adapters are documented in
[the adapter notes](../../tools/gsp_fixes/README.md); they are applied to the
build archive without editing `managed_components`.

## Assets and generation

Hub and every App remain independent source and intermediate `.gspb` packages.
Product builds add one shared Font Catalog to the same `ui_apps` mmap partition:

- `hub/generated` and `apps/*/generated` are self-contained intermediates.
- `common/generated/linked` contains product bundles after font linking.
- `common/generated/common-fonts.gspb` is the Launcher-owned Catalog.
- `common/generated/font-link-report.md` records dependencies and size savings.

Run this from the component directory after changing a scene (`gspc.exe` on
Windows):

```sh
python3 regenerate_font_catalog.py --gspc /path/to/gspc
```

It regenerates every intermediate and rebuilds all linked entries, offsets,
alignment and CRC. For one App:

```sh
python3 tools/generate_bundle.py --module-dir apps/settings --gspc /path/to/gspc
```

Configuration checks Pillow and the `esp-gsp-tools` API independently. If the
compiler manager is missing or incompatible, run `python3 -m pip install -U esp-gsp-tools`
with that same interpreter. GSPC is a separate executable selected by the
component; updating the manager does not replace a missing compiler release.

The Loader opens the Catalog once and passes it to each App Context. Context
creation resolves local fonts first and Catalog fonts second. Rendering retains
a direct scene-local `font_ref` table without a Catalog lookup per frame.
Standalone Apps keep the default `embedded` policy. Mosaic scenes use `auto`;
large numeric and App-specific fonts remain embedded unless another App has
the exact same Font Asset ID.

Scene generators use three charset policies:

- `shared_charset(...)`: a stable common set, normally printable ASCII at 16/18px.
- `auto_charset()`: static labels and the declared dynamic charsets at that size.
- `explicit_charset(...)`: a small dynamic set, such as `:0123456789` for a clock.

Dynamic labels must also carry `font_charset`. Asset-only scenes do not seed
fonts, so their builds cannot replace the primary scene's subsets with a broad
fallback set.

## Navigation, power and module feedback

`hub_stack` contains Launcher, Notification Center and the extension-board
introduction. Launcher owns its nested PageFlow. AOD and CHRG are mutually
exclusive modes inside the native Drawer; the Drawer owns drag/settle input
and composition. Closing it resumes the retained StackView/PageFlow state.

Idle screen-off pauses the foreground App without opening Lock Screen. Music
and Bluetooth background ticks continue; an active camera preview inhibits
screen-off. Wake restores the current page and consumes the waking contact
until release. DFS and automatic light sleep remain disabled.

Module discovery at boot is silent. After a stable slot baseline, a confirmed
absent-to-present transition schedules the original side animation and
single-board introduction on the awake Launcher. Left/right insertions share
a 400 ms feedback window. Different App targets queue until the current page
and Done/Back animation finish; equal targets share one notice. Removal cancels
queued feedback and invalidates its Open target. Descriptor retries and ownership
restoration do not count as insertions.

App changes, an open Drawer, Lock Screen and screen-off discard pending feedback;
brief rendering/input motion defers it. An existing introduction survives idle
screen-off. The UI timer drives this policy using fixed slot snapshots, without
a new task, persistent setting or I2C scan. Discovery remains active.

## Camera and resource ownership

Camera completes native open and STREAMON before reserving its two RGB565
preview outputs. During capture, the published source frame remains immutable;
only the other output, after GSP releases it, is lent to the JPEG encoder.
This avoids a third 460,800-byte allocation while retaining preview double
buffering, 480x480 photographs and JPEG quality 90.

Start failures retire canvas readers before freeing preview memory or retrying
native open. Initialization retries are bounded. Exhaustion exposes the existing
shutter retry hint; a retry first completes checked cleanup. Deferred starts carry
a visit generation, and cancelled work cannot start a later App visit.

Stop failures retain buffers, synchronization objects and callback owners until
cleanup succeeds. Native close errors keep the session and power vote alive for
another close attempt. Save failures release the borrowed output and preserve
existing files. See [camera profiles](../mosaico_camera_service/README.md) for
the default SC101IOT equivalent sensor window and its memory budget.

The platform retains empty Sessions, pause tokens and shared font/MMAP sources
when recovery or destruction fails. Callback removal needs a successful flush
before its owner is freed. An incomplete pause cannot become successful
screen-off or presenter handoff. Camera admission trims inactive snapshot caches;
direct App replacement pauses, trims and resumes the exact new Camera UI before
native START. These barriers can add entry latency and preserve resource ownership.
PSRAM XIP remains enabled.

## Scrolling and validation

Settings compares displayed row values before publishing. Unchanged notifications
preserve scrolling snapshots; changed rows update immediately. Rejected commands
and asynchronous binding failures retry through the App tick. Hub refreshes defer
ordinary updates during motion, while battery sampling runs outside the UI timer.
The backdrop coalesces full-width RGB565 tiles and retains cleanup fences.

Display staging uses 16 rows with TE and two compose buffers. Compared with
10 rows, the two staging buffers cost 11,520 additional bytes. The selected
configuration preserves the existing scenes and module-detection effects.

See [Host regressions](../../tools/tests/README.md) for commands and coverage,
including generated-scene checks. The [Hub diagnostics guide](../../tools/tests/hub_refresh/README.md)
describes bounded `ui_perf` sampling and controlled replay. Host checks and
render-task service gaps do not establish physical LCD FPS, touch-to-photon
latency, Bluetooth continuity or measured battery current. Record device,
firmware/configuration and workload when collecting those results.
