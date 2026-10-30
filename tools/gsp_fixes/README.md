# GSP 1.9.0 product adapters

The product pins the official ESP32-S31 Registry archive and public headers.
`manifest.json` binds them to source commit
`1ba1758faaf5d6034dec415a6403845b91a01a59`; `upstream/` retains only the
source/header closure needed by two product adapters, with its license.

GSP 1.9.0 supplies the Drawer takeover/reversal/snapshot lifecycle repairs,
Session ownership results, paused atlas trimming, and safe decoder teardown.
The old Drawer and viewport replacement objects have been removed. These
behaviors now come from the released archive; they are not re-patched here.

Two product integrations have no equivalent public entry point in this release:

- The input adapter runs the product wake-contact guard before application
  interceptors, hit testing, and gesture observers, swallowing through release.
- The Hub capture bridge rasterizes its StackView group into the existing
  RGB565 backdrop before the Drawer gesture; it excludes independent notices.

These are product policies, not additional GSP correctness repairs. They retain
private SDK layouts and therefore require an exact archive/header/source pin.
CMake recompiles only `gsp_app.c` and `esp_gsp_input.c` in the actual GSP compile
context and replaces them in a build-directory archive copy. It never edits the
Registry package. `mosaic_gsp/libesp-gsp.json` records input/output, object and
manifest hashes plus compiler flags. An SDK change fails closed until reviewed.

The Hub allocates its 480x480 RGB565 backdrop (460,800 bytes in PSRAM) on demand,
retains it until Hub stop, and polls motion only while eligible. Allocation or
Canvas setup failure retains ordinary rendering. Shutdown fences callbacks
before releasing the source; an unsuccessful fence retains it and disables
caching for that boot.

Camera ROI is enabled for the supported SC101IOT mode now that the released SDK
has the required APIs. It retains XIP, preview UI, display buffers, and 480x480
photos; other sensors and unrecognized modes use their native format.
