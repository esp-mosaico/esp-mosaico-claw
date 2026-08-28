# Mosaic UI assets

Hub and every App remain independent source and intermediate `.gspb` packages.
Product builds add one shared Font Catalog to the same `ui_apps` mmap partition:

- `hub/generated` and `apps/*/generated` are self-contained intermediates.
- `common/generated/linked` contains product bundles after font linking.
- `common/generated/common-fonts.gspb` is the Launcher-owned常驻 Catalog.
- `common/generated/font-link-report.md` records dependencies and size savings.

Run `./regenerate_font_catalog.sh` after changing a scene. It regenerates every
intermediate and rebuilds all linked entries, offsets, alignment, and CRC; it
does not append bytes to an existing binary.

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

`hub_stack` contains only ordinary navigation: Launcher, Settings,
Notification Center, and Insert Notice. Launcher owns its nested PageFlow.
AOD and CHRG are two mutually exclusive modes of the top-level `lock_screen`
visibility group; they are not StackView pages and never participate in
PageFlow transforms. While visible, the Hub input interceptor consumes every
pointer sample, with only the demo mode button and tap/swipe-up unlock handled
locally. Hiding Lock Screen therefore resumes the exact StackView/PageFlow
state that was present before locking.
