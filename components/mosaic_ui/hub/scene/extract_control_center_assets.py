#!/usr/bin/env python3
"""Extract the prototype's original C1 SVG artwork as PNG scene assets."""

import json
import re
from pathlib import Path

import cairosvg


HERE = Path(__file__).resolve().parent
PROTOTYPE = HERE.parent.parent / "docs" / "mosaico-prototype.html"
OUTPUT = HERE.parent.parent / "common" / "assets" / "control_center"

SIZES = {
    "wifi": (44, 44), "airdrop": (44, 44), "bt": (44, 44),
    "batt": (44, 44), "mpRing": (139, 139), "prev": (48, 48),
    "pause": (48, 48), "play": (48, 48), "next": (48, 48),
    "bellOn": (44, 44), "bellOff": (44, 44),
    "vibOn": (44, 44), "vibOff": (44, 44),
    "slotL": (94, 94), "slotR": (94, 94),
    "sun": (48, 48), "sunSemi": (48, 48), "sunOff": (48, 48),
    "sound": (48, 48), "soundLow": (48, 48), "soundOff": (48, 48),
}


def main():
    text = PROTOTYPE.read_text(encoding="utf-8")
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for name, (width, height) in SIZES.items():
        match = re.search(
            rf"^  {re.escape(name)}:\s*(\"<svg.*?</svg>\"),?$",
            text, re.MULTILINE,
        )
        if match is None:
            raise RuntimeError(f"prototype SVG not found: {name}")
        svg = json.loads(match.group(1))
        cairosvg.svg2png(
            bytestring=svg.encode(), write_to=str(OUTPUT / f"{name}.png"),
            output_width=width, output_height=height,
        )
        if name in {"wifi", "airdrop", "bt", "batt", "bellOff", "vibOn",
                    "sun", "sunSemi", "sunOff", "sound", "soundLow",
                    "soundOff"}:
            active = re.sub(r"#(?:FCFCFF|F9FAFB)", "#FF4C01", svg,
                            flags=re.IGNORECASE)
            cairosvg.svg2png(
                bytestring=active.encode(),
                write_to=str(OUTPUT / f"{name}_active.png"),
                output_width=width, output_height=height,
            )
    # Outside-corner mask: leaves the 102x204 capsule transparent and paints
    # only the pixels outside it with the C1 page background.
    mask = """<svg width="102" height="204" viewBox="0 0 102 204"
      xmlns="http://www.w3.org/2000/svg">
      <!-- Calibrated for the RGB565 image path so the mask matches the
           #272727 scene primitive after both are quantized. -->
      <path fill="#2A2A2A" fill-rule="evenodd"
        d="M0 0h102v204H0z M51 0a51 51 0 0 0-51 51v102a51 51 0 0 0
        51 51 51 51 0 0 0 51-51V51A51 51 0 0 0 51 0z"/>
    </svg>"""
    cairosvg.svg2png(bytestring=mask.encode(),
                     write_to=str(OUTPUT / "capsule_mask.png"),
                     output_width=102, output_height=204)


if __name__ == "__main__":
    main()
