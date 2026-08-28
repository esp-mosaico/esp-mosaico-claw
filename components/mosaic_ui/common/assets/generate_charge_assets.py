#!/usr/bin/env python3
"""Generate the two-color charging silhouette from the web specification."""

from pathlib import Path

from PIL import Image, ImageDraw

ROWS = (
    "0:180,200,220;20:160,180,200,220;40:140,160,180,200;"
    "60:140,160,180,200;63.41:s123.41;80:120,140,160,180,200;"
    "100:100,120,140,160,180,200;120:80,100,120,140,160,180,200;"
    "140:80,100,120,140,160,180;160:60,80,100,120,140,160,180,200,220,240,260,280;"
    "180:40,60,80,100,120,140,160,180,200,220,240,260,280;"
    "200:40,60,80,100,120,140,160,180,200,220,240,260;"
    "220:20,40,60,80,100,120,140,160,180,200,220,240;"
    "240:0,20,40,60,80,100,120,140,160,180,200,220;242.04:m242.04;"
    "260:100,120,140,160,180,200,220;280:100,120,140,160,180,200;"
    "300:100,120,140,160,180;320:100,120,140,160,180;"
    "340:80,100,120,140,160;360:80,100,120,140;380:80,100,120,140;"
    "400:80,100,120;420:80,100;440:80,100;460:60,80;480:60;500:60"
)

OUT = Path(__file__).resolve().parent
SCALE = 0.64
WIDTH = 192
HEIGHT = 328


def generate(name: str, color: tuple[int, int, int, int]) -> None:
    canvas = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    draw = ImageDraw.Draw(canvas)
    for row in ROWS.split(";"):
        y_text, cells = row.split(":")
        y = float(y_text)
        for cell in cells.split(","):
            kind = cell[0] if cell[0] in "sm" else ""
            x = float(cell[1:] if kind else cell)
            size = 3.76 if kind == "s" else 6.5 if kind == "m" else 10.58
            box = (
                round(x * SCALE),
                round(y * SCALE),
                round((x + size) * SCALE),
                round((y + size) * SCALE),
            )
            draw.rectangle(box, fill=color)
    canvas.save(OUT / name, optimize=True)


generate("charge_shape_gray.png", (59, 60, 61, 255))
generate("charge_shape_yellow.png", (255, 196, 0, 255))
