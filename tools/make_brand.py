"""Renders every Glint brand asset from the design sources in docs/brand/source.

    python tools/make_brand.py

Needs Pillow and NumPy. The bolt is a vector (traced from
bolt-black-on-white.png); the "Glint" lettering is a cleaned alpha mask cut from
lockup-black-on-white.png. Writes:
  docs/brand/        SVG marks and icons, PNG icons and logo lockups (README)
  app/res/glint.ico  the exe and window icon (16-256 px; small sizes are bolder)
  payload/assets/banner.png  960x400 banner served by the payload
The outputs are kept in the repo, so a build never needs Python.
"""
import os
import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "docs", "brand", "source")
BLACK = (0, 0, 0)
WHITE = (255, 255, 255)

# The bolt in a box 62.74 wide and 100 tall. main.cpp and dashboard.html carry
# the same points.
BOLT = [(24.26, 0.0), (56.08, 0.0), (34.17, 33.28), (62.74, 33.28), (7.15, 100.0), (24.52, 52.15), (0.0, 52.15)]
BOLT_W, BOLT_H = 62.74, 100.0

RADIUS = 0.22      # icon corner radius, share of the side
BOLT_SHARE = 0.64  # bolt height, share of the icon side
SS = 4             # supersampling for smooth edges

# Lockup proportions measured on lockup-black-on-white.png, in text heights:
# the bolt is 1.355 tall, starts 0.083 above the lettering, and sits 0.136 before it.
LOCK_BOLT = 1.355
LOCK_RISE = 0.083
LOCK_GAP = 0.136
TEXT_BOX = (792, 225, 1634, 490)   # the lettering in the source image (x0, y0, x1, y1)


def out(*parts):
    path = os.path.join(ROOT, *parts)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    return path


def bolt_points(x, y, height):
    """Bolt `height` tall with its box's top-left corner at (x, y)."""
    s = height / BOLT_H
    return [(x + px * s, y + py * s) for px, py in BOLT]


def wordmark_mask():
    """The lettering as a clean 2x alpha mask: compression noise ramped away,
    edges re-sharpened after the upscale."""
    src = Image.open(os.path.join(SOURCE, "lockup-black-on-white.png")).convert("L")
    x0, y0, x1, y1 = TEXT_BOX
    pad = 6
    crop = src.crop((x0 - pad, y0 - pad, x1 + 1 + pad, y1 + 1 + pad))
    big = crop.resize((crop.width * 2, crop.height * 2), Image.LANCZOS)
    ink = 255.0 - np.asarray(big, dtype=np.float32)          # lettering is dark
    alpha = np.clip((ink - 96.0) / 64.0, 0.0, 1.0)          # 96..160 -> 0..1
    alpha = alpha * alpha * (3 - 2 * alpha)                  # smoothstep
    mask = Image.fromarray((alpha * 255).astype(np.uint8), "L")
    p2 = pad * 2
    return mask.crop((p2, p2, mask.width - p2, mask.height - p2))


WORDMARK = None


def icon(size, dark, bold=0.0):
    """Rounded-square app icon. `bold` thickens the bolt (share of the side)."""
    big = size * SS
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    bg, fg = (BLACK, WHITE) if dark else (WHITE, BLACK)
    d.rounded_rectangle([0, 0, big - 1, big - 1], radius=RADIUS * big, fill=bg + (255,))
    if not dark:   # a hairline so the white tile shows on white
        d.rounded_rectangle([0, 0, big - 1, big - 1], radius=RADIUS * big, outline=(0, 0, 0, 40), width=max(1, SS))
    h = big * BOLT_SHARE
    pts = bolt_points((big - h * BOLT_W / BOLT_H) / 2, (big - h) / 2, h)
    d.polygon(pts, fill=fg + (255,))
    if bold > 0:
        d.polygon(pts, outline=fg + (255,), width=max(1, round(bold * big)))
    return img.resize((size, size), Image.LANCZOS)


def mark(height, color):
    """The bare bolt on transparent."""
    h = height * SS
    w = round(h * BOLT_W / BOLT_H)
    img = Image.new("RGBA", (w + 2 * SS, h + 2 * SS), (0, 0, 0, 0))
    ImageDraw.Draw(img).polygon(bolt_points(SS, SS, h), fill=color + (255,))
    return img.resize((img.width // SS, img.height // SS), Image.LANCZOS)


def lockup(text_h, color, background=None, size=None):
    """Bolt + "Glint" with the lettering `text_h` px tall."""
    global WORDMARK
    if WORDMARK is None:
        WORDMARK = wordmark_mask()
    th = text_h * SS
    tw = th * WORDMARK.width / WORDMARK.height
    bh = th * LOCK_BOLT
    bw = bh * BOLT_W / BOLT_H
    gap = th * LOCK_GAP
    top = -th * LOCK_RISE                 # bolt top, relative to the lettering's top
    content_w = bw + gap + tw
    content_top, content_bottom = min(top, 0), max(top + bh, th)
    content_h = content_bottom - content_top
    if size:
        W, H = size[0] * SS, size[1] * SS
    else:
        pad = th * 0.12
        W, H = round(content_w + 2 * pad), round(content_h + 2 * pad)
    img = Image.new("RGBA", (W, H), (background + (255,)) if background else (0, 0, 0, 0))
    x0 = (W - content_w) / 2
    text_top = (H - content_h) / 2 - content_top
    ImageDraw.Draw(img).polygon(bolt_points(x0, text_top + top, bh), fill=color + (255,))
    letters = WORDMARK.resize((round(tw), round(th)), Image.LANCZOS)
    img.paste(Image.new("RGBA", letters.size, color + (255,)), (round(x0 + bw + gap), round(text_top)), letters)
    return img.resize((W // SS, H // SS), Image.LANCZOS)


def svg_path(scale=1.0, dx=0.0, dy=0.0):
    return "M" + " L".join(f"{x * scale + dx:.2f} {y * scale + dy:.2f}" for x, y in BOLT) + " Z"


def write_svgs():
    for name, color in (("black", "#000000"), ("white", "#FFFFFF")):
        with open(out("docs", "brand", f"glint-mark-{name}.svg"), "w", encoding="utf-8") as f:
            f.write(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {BOLT_W} {BOLT_H}">'
                    f'<path d="{svg_path()}" fill="{color}"/></svg>\n')
    s = BOLT_SHARE * 100 / BOLT_H
    path = svg_path(s, 50 - BOLT_W * s / 2, 50 - BOLT_H * s / 2)
    for name, bg, fg in (("dark", "#000000", "#FFFFFF"), ("light", "#FFFFFF", "#000000")):
        with open(out("docs", "brand", f"glint-icon-{name}.svg"), "w", encoding="utf-8") as f:
            f.write('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">'
                    f'<rect width="100" height="100" rx="{RADIUS * 100:g}" fill="{bg}"/>'
                    f'<path d="{path}" fill="{fg}"/></svg>\n')


def main():
    write_svgs()
    for dark in (True, False):
        name = "dark" if dark else "light"
        icon(1024, dark).save(out("docs", "brand", f"glint-icon-{name}.png"))
    mark(1024, BLACK).save(out("docs", "brand", "glint-mark-black.png"))
    mark(1024, WHITE).save(out("docs", "brand", "glint-mark-white.png"))
    # README header (about 4x its shown size): black for GitHub's light theme,
    # white for its dark theme.
    lockup(240, BLACK).save(out("docs", "brand", "glint-logo-light.png"))
    lockup(240, WHITE).save(out("docs", "brand", "glint-logo-dark.png"))

    # Windows icon: the dark tile shows on both taskbar themes. Below 48 px the
    # bolt gets thicker so it does not thin out to a scratch.
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    frames = [icon(s, True, bold=0.05 if s <= 24 else 0.03 if s <= 40 else 0.0) for s in sizes]
    frames[-1].save(out("app", "res", "glint.ico"), sizes=[(s, s) for s in sizes], append_images=frames[:-1])

    lockup(118, WHITE, background=BLACK, size=(960, 400)).convert("RGB").save(out("payload", "assets", "banner.png"), optimize=True)


if __name__ == "__main__":
    main()
