#!/usr/bin/env python3
"""Render the app icon: a macOS-shaped rounded square holding a drive glyph
and the letters NTFS. Deterministic, no external assets, so the icon can be
regenerated from source and diffed.

  python3 fskit/Design/make-icon.py            -> fskit/Design/AppIcon-1024.png
                                                  + fskit/Design/AppIcon.icns
"""
import math, os, subprocess, sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
S = 1024
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

# --- macOS icon plate: rounded square, ~82% of the canvas, corner radius 22.5%
pad = int(S * 0.09); r = int((S - 2 * pad) * 0.225)
plate = (pad, pad, S - pad, S - pad)

# vertical gradient, deep slate to near-black, drawn line by line
top, bot = (38, 50, 66), (16, 20, 28)
grad = Image.new("RGBA", (S, S), (0, 0, 0, 0)); gd = ImageDraw.Draw(grad)
for y in range(pad, S - pad):
    t = (y - pad) / (S - 2 * pad)
    c = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)) + (255,)
    gd.line([(pad, y), (S - pad, y)], fill=c)
mask = Image.new("L", (S, S), 0); ImageDraw.Draw(mask).rounded_rectangle(plate, radius=r, fill=255)
img.paste(grad, (0, 0), mask)

# soft inner highlight along the top edge
hl = Image.new("RGBA", (S, S), (0, 0, 0, 0)); hd = ImageDraw.Draw(hl)
hd.rounded_rectangle((pad + 6, pad + 6, S - pad - 6, pad + int(S * 0.28)), radius=r, fill=(255, 255, 255, 26))
hl = hl.filter(ImageFilter.GaussianBlur(28))
img.alpha_composite(Image.composite(hl, Image.new("RGBA", (S, S), (0, 0, 0, 0)), mask))

# --- drive glyph: a rounded slab with a status light, centred in the upper half
gw, gh = int(S * 0.52), int(S * 0.16)
gx, gy = (S - gw) // 2, int(S * 0.27)
slab = (gx, gy, gx + gw, gy + gh)
d.rounded_rectangle(slab, radius=int(gh * 0.28), fill=(226, 230, 236, 255))
d.rounded_rectangle((gx + 4, gy + 4, gx + gw - 4, gy + gh - 4), radius=int(gh * 0.25), outline=(180, 188, 198, 255), width=3)
# a second, slightly recessed slab beneath: a stack, i.e. storage
d.rounded_rectangle((gx + int(gw*0.06), gy + gh + 18, gx + gw - int(gw*0.06), gy + gh + 18 + int(gh*0.55)),
                    radius=int(gh * 0.2), fill=(196, 202, 212, 255))
# status light
lr = int(gh * 0.16); lx, ly = gx + gw - int(gh * 0.55), gy + gh // 2
d.ellipse((lx - lr, ly - lr, lx + lr, ly + lr), fill=(72, 199, 142, 255))
glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ImageDraw.Draw(glow).ellipse((lx - lr*2.2, ly - lr*2.2, lx + lr*2.2, ly + lr*2.2), fill=(72, 199, 142, 110))
img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(14)))

# --- wordmark
def font(size):
    for f in ("/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/HelveticaNeue.ttc", "/System/Library/Fonts/Helvetica.ttc"):
        try: return ImageFont.truetype(f, size, index=0)
        except Exception: pass
    return ImageFont.load_default()
f = font(int(S * 0.235))
text = "NTFS"
bbox = d.textbbox((0, 0), text, font=f)
tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
tx, ty = (S - tw) // 2 - bbox[0], int(S * 0.585) - bbox[1]
# faint shadow, then the letters
sh = Image.new("RGBA", (S, S), (0, 0, 0, 0)); ImageDraw.Draw(sh).text((tx, ty + 6), text, font=f, fill=(0, 0, 0, 140))
img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(10)))
d = ImageDraw.Draw(img)
d.text((tx, ty), text, font=f, fill=(240, 243, 247, 255))

png = os.path.join(HERE, "AppIcon-1024.png"); img.save(png)
print("wrote", png)

# --- .icns via iconutil
iconset = os.path.join(HERE, "AppIcon.iconset"); os.makedirs(iconset, exist_ok=True)
for base in (16, 32, 128, 256, 512):
    for scale in (1, 2):
        px = base * scale
        name = f"icon_{base}x{base}{'@2x' if scale == 2 else ''}.png"
        img.resize((px, px), Image.LANCZOS).save(os.path.join(iconset, name))
icns = os.path.join(HERE, "AppIcon.icns")
subprocess.check_call(["iconutil", "-c", "icns", iconset, "-o", icns])
print("wrote", icns)
