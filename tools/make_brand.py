"""Renders every Glint brand asset from one vector bolt.

    python tools/make_brand.py

Needs Pillow. Writes:
  docs/brand/        SVG marks and icons, PNG icons and logo lockups (README)
  app/res/glint.ico  the exe and window icon (16-256 px; small sizes are bolder)
  payload/assets/banner.png  960x400 banner served by the payload
The outputs are kept in the repo, so a build never needs Python.
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BLACK = (10, 10, 10)
WHITE = (255, 255, 255)

# The bolt, traced from the design sheet (docs/brand/glint-sheet.png), in a box
# 70.2 wide and 100 tall. main.cpp and dashboard.html carry the same points.
BOLT = [(37.1, 0.0), (64.2, 0.0), (39.9, 37.6), (70.2, 37.6), (9.9, 100.0), (25.1, 56.0), (0.0, 56.0)]
BOLT_W, BOLT_H = 70.2, 100.0

RADIUS = 0.22      # icon corner radius, share of the side
BOLT_SHARE = 0.62  # bolt height, share of the icon side
SS = 4             # supersampling for smooth edges


def out(*parts):
    path = os.path.join(ROOT, *parts)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    return path


def bolt_points(cx, cy, height):
    s = height / BOLT_H
    return [(cx + (x - BOLT_W / 2) * s, cy + (y - BOLT_H / 2) * s) for x, y in BOLT]


def icon(size, dark, bold=0.0):
    """Rounded-square app icon. `bold` thickens the bolt (share of the side)."""
    big = size * SS
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    bg, fg = (BLACK, WHITE) if dark else (WHITE, BLACK)
    d.rounded_rectangle([0, 0, big - 1, big - 1], radius=RADIUS * big, fill=bg + (255,))
    if not dark:   # a hairline so the white tile shows on white
        d.rounded_rectangle([0, 0, big - 1, big - 1], radius=RADIUS * big, outline=(0, 0, 0, 40), width=max(1, SS))
    pts = bolt_points(big / 2, big / 2, big * BOLT_SHARE)
    d.polygon(pts, fill=fg + (255,))
    if bold > 0:
        d.polygon(pts, outline=fg + (255,), width=max(1, round(bold * big)))
    return img.resize((size, size), Image.LANCZOS)


def mark(height, color):
    """The bare bolt on transparent."""
    h = height * SS
    w = round(h * BOLT_W / BOLT_H) + 2 * SS
    img = Image.new("RGBA", (w, h + 2 * SS), (0, 0, 0, 0))
    ImageDraw.Draw(img).polygon(bolt_points(w / 2, (h + 2 * SS) / 2, h), fill=color + (255,))
    return img.resize((max(1, w // SS), max(1, (h + 2 * SS) // SS)), Image.LANCZOS)


def lockup(cap, color, background=None, size=None):
    """Bolt + "Glint" (Geist SemiBold). `cap` is the wordmark's cap height in px."""
    font = ImageFont.truetype(os.path.join(ROOT, "app", "vendor", "fonts", "Geist-SemiBold.ttf"), round(cap * SS / 0.71))
    text = "Glint"
    l, t, r, b = font.getbbox(text)
    tw, th = r - l, b - t
    bh = cap * SS * 1.8
    bw = bh * BOLT_W / BOLT_H
    gap = cap * SS * 0.45
    content_w, content_h = bw + gap + tw, max(bh, th)
    pad = cap * SS * 0.25
    if size:
        W, H = size[0] * SS, size[1] * SS
    else:
        W, H = round(content_w + 2 * pad), round(content_h + 2 * pad)
    img = Image.new("RGBA", (W, H), (background + (255,)) if background else (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    x0 = (W - content_w) / 2
    cy = H / 2
    d.polygon(bolt_points(x0 + bw / 2, cy, bh), fill=color + (255,))
    d.text((x0 + bw + gap - l, cy - th / 2 - t), text, font=font, fill=color + (255,))
    return img.resize((W // SS, H // SS), Image.LANCZOS)


def svg_path(scale=1.0, dx=0.0, dy=0.0):
    return "M" + " L".join(f"{x * scale + dx:.2f} {y * scale + dy:.2f}" for x, y in BOLT) + " Z"


def write_svgs():
    for name, color in (("black", "#0A0A0A"), ("white", "#FFFFFF")):
        with open(out("docs", "brand", f"glint-mark-{name}.svg"), "w", encoding="utf-8") as f:
            f.write(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {BOLT_W} {BOLT_H}">'
                    f'<path d="{svg_path()}" fill="{color}"/></svg>\n')
    s = BOLT_SHARE * 100 / BOLT_H
    path = svg_path(s, 50 - BOLT_W * s / 2, 50 - BOLT_H * s / 2)
    for name, bg, fg in (("dark", "#0A0A0A", "#FFFFFF"), ("light", "#FFFFFF", "#0A0A0A")):
        with open(out("docs", "brand", f"glint-icon-{name}.svg"), "w", encoding="utf-8") as f:
            f.write('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">'
                    f'<rect width="100" height="100" rx="{RADIUS * 100:g}" fill="{bg}"/>'
                    f'<path d="{path}" fill="{fg}"/></svg>\n')


def main():
    write_svgs()
    for dark in (True, False):
        name = "dark" if dark else "light"
        icon(1024, dark).save(out("docs", "brand", f"glint-icon-{name}.png"))
    mark(512, BLACK).save(out("docs", "brand", "glint-mark-black.png"))
    mark(512, WHITE).save(out("docs", "brand", "glint-mark-white.png"))
    # README header: dark text for GitHub's light theme, white for its dark theme.
    lockup(64, BLACK).save(out("docs", "brand", "glint-logo-light.png"))
    lockup(64, WHITE).save(out("docs", "brand", "glint-logo-dark.png"))

    # Windows icon: the dark tile shows on both taskbar themes. Below 48 px the
    # bolt gets thicker so it does not thin out to a scratch.
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    frames = [icon(s, True, bold=0.05 if s <= 24 else 0.03 if s <= 40 else 0.0) for s in sizes]
    frames[-1].save(out("app", "res", "glint.ico"), sizes=[(s, s) for s in sizes], append_images=frames[:-1])

    lockup(92, WHITE, background=BLACK, size=(960, 400)).convert("RGB").save(out("payload", "assets", "banner.png"), optimize=True)


if __name__ == "__main__":
    main()
