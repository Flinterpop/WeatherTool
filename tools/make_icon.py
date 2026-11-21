"""Generate src/WeatherTool.ico: a thermometer and a snowflake on a rounded
tile shading from warm orange (top) to cool blue (bottom), the app's two
chart colours. The two smallest sizes keep only the thermometer.

Each size is rendered at 4x and downsampled so edges stay crisp. Run from the
repo root:  python tools/make_icon.py   (needs Pillow)
"""

from __future__ import annotations

import math
from pathlib import Path

from PIL import Image, ImageDraw

SIZES: tuple[int, ...] = (16, 24, 32, 48, 64, 128, 256)
SUPERSAMPLE: int = 4
SIMPLE_BELOW: int = 32          # sizes under this drop the snowflake

WARM: tuple[int, int, int, int] = (235, 104, 52, 255)    # chart orange
COOL: tuple[int, int, int, int] = (42, 120, 214, 255)    # chart blue
WHITE: tuple[int, int, int, int] = (255, 255, 255, 255)
MERCURY: tuple[int, int, int, int] = (214, 58, 47, 255)

Colour = tuple[int, int, int, int]


def _lerp(a: Colour, b: Colour, t: float) -> Colour:
    assert 0.0 <= t <= 1.0
    r, g, bl, al = (int(round(x + (y - x) * t)) for x, y in zip(a, b))
    return (r, g, bl, al)


def _tile(px: int) -> Image.Image:
    """Rounded square with a vertical warm-to-cool gradient."""
    assert px > 0
    grad = Image.new("RGBA", (px, px))
    draw = ImageDraw.Draw(grad)
    for y in range(px):
        draw.line([(0, y), (px, y)], fill=_lerp(WARM, COOL, y / max(px - 1, 1)))
    mask = Image.new("L", (px, px), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, px - 1, px - 1], radius=int(px * 0.22), fill=255)
    out = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    out.paste(grad, (0, 0), mask)
    return out


def _thermometer(draw: ImageDraw.ImageDraw, px: int, cx: float, simple: bool) -> None:
    """White thermometer (tube + bulb) with a red mercury column."""
    assert px > 0
    tube_w = px * (0.17 if simple else 0.13)
    top = px * (0.14 if simple else 0.16)
    bulb_r = px * (0.17 if simple else 0.13)
    bulb_cy = px * (0.74 if simple else 0.72)
    draw.rounded_rectangle([cx - tube_w / 2, top, cx + tube_w / 2, bulb_cy], radius=tube_w / 2, fill=WHITE)
    draw.ellipse([cx - bulb_r, bulb_cy - bulb_r, cx + bulb_r, bulb_cy + bulb_r], fill=WHITE)
    inner_w = tube_w * 0.42
    inner_r = bulb_r * 0.62
    mercury_top = px * (0.36 if simple else 0.38)
    draw.rounded_rectangle([cx - inner_w / 2, mercury_top, cx + inner_w / 2, bulb_cy], radius=inner_w / 2, fill=MERCURY)
    draw.ellipse([cx - inner_r, bulb_cy - inner_r, cx + inner_r, bulb_cy + inner_r], fill=MERCURY)


def _snowflake(draw: ImageDraw.ImageDraw, px: int, cx: float, cy: float, radius: float) -> None:
    """Six arms, each with a pair of side branches."""
    assert radius > 0
    width = max(int(px * 0.045), 1)
    for k in range(6):
        a = math.radians(90 + 60 * k)
        ex, ey = cx + radius * math.cos(a), cy - radius * math.sin(a)
        draw.line([(cx, cy), (ex, ey)], fill=WHITE, width=width)
        bx, by = cx + radius * 0.6 * math.cos(a), cy - radius * 0.6 * math.sin(a)
        for side in (-1, 1):
            b = a + side * math.radians(40)
            draw.line([(bx, by), (bx + radius * 0.32 * math.cos(b), by - radius * 0.32 * math.sin(b))],
                      fill=WHITE, width=width)
    dot = width * 0.9
    draw.ellipse([cx - dot, cy - dot, cx + dot, cy + dot], fill=WHITE)


def render(size: int) -> Image.Image:
    """One icon image at `size` pixels."""
    assert size in SIZES
    px = size * SUPERSAMPLE
    img = _tile(px)
    draw = ImageDraw.Draw(img)
    simple = size < SIMPLE_BELOW
    if simple:
        _thermometer(draw, px, px * 0.5, simple=True)
    else:
        _thermometer(draw, px, px * 0.30, simple=False)
        _snowflake(draw, px, px * 0.65, px * 0.48, px * 0.24)
    out = img.resize((size, size), Image.Resampling.LANCZOS)
    assert out.size == (size, size)
    return out


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    target = root / "src" / "WeatherTool.ico"
    images = [render(s) for s in SIZES]
    largest = images[-1]
    largest.save(target, format="ICO", sizes=[(s, s) for s in SIZES], append_images=images[:-1])
    assert target.stat().st_size > 0
    preview = root / "build" / "icon-preview.png"
    preview.parent.mkdir(exist_ok=True)
    sheet = Image.new("RGBA", (sum(SIZES) + 10 * len(SIZES), max(SIZES)), (250, 250, 250, 255))
    x = 0
    for img in images:
        sheet.paste(img, (x, 0), img)
        x += img.width + 10
    sheet.save(preview)
    print(f"wrote {target} ({target.stat().st_size} bytes) and {preview}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
