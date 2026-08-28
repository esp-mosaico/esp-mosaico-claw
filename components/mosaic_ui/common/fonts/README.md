# Build-time fonts

These fonts are inputs to Mosaic scene generation. Keeping them in the
repository makes scene builds independent of the fonts installed on the build
host. The scene compiler subsets the fonts into generated `.gfb` assets; the
complete font file is not embedded in the firmware.

## Noto Sans Regular

- Version: 2.015
- Variant: hinted TrueType
- Upstream release: <https://github.com/notofonts/latin-greek-cyrillic/releases/tag/NotoSans-v2.015>
- Release archive path: `NotoSans/hinted/ttf/NotoSans-Regular.ttf`
- SHA-256: `478c558ea716033cd60c03438f628dfa75694dcf6b5f6d505a2f05fd2b4f3823`
- License: SIL Open Font License 1.1; see `OFL.txt`

When updating the font, update the version, source path, and checksum above in
the same change so builds continue to use a reviewable, fixed artifact.

## DejaVu Sans

- Version: 2.37
- Files: `DejaVuSans.ttf`, `DejaVuSans-Bold.ttf`
- Upstream: <https://dejavu-fonts.github.io/>
- Regular SHA-256: `690243adfefe0ce154b547db6205794bd30ac4277275179517a90994f4980648`
- Bold SHA-256: `d1c3ff99f1e1ce1827a33efd4dad81f40babda06bff9e43bd7591c86662a287b`
- License: Bitstream Vera license with DejaVu changes in the public domain; see `LICENSE-DejaVu.txt`
