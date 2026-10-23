# Draws the README's banner: original art only (gradient, speed streaks, the port's own "T2" icon
# from src/setup/ttt2.ico and plain typography) - no game artwork.
#     python port/tools/make_banner.py .github/assets/banner.png [.github/assets/download.png]
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = sys.argv[1]
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ICON = os.path.join(ROOT, "port", "src", "setup", "ttt2.ico")
W, H = 2560, 840


def font(names, size):
    for n in names:
        try:
            return ImageFont.truetype(os.path.join("C:/Windows/Fonts", n), size)
        except OSError:
            pass
    return ImageFont.load_default()


BLACK = ["seguibl.ttf", "ariblk.ttf", "arialbd.ttf"]
BOLD = ["segoeuib.ttf", "arialbd.ttf"]
SEMI = ["seguisb.ttf", "segoeui.ttf", "arial.ttf"]

# Background: deep blue-violet gradient, darker toward the edges.
bg = Image.new("RGB", (W, H))
px = bg.load()
for y in range(H):
    for x in range(0, W):
        t = x / W
        v = 1.0 - 0.55 * ((y / H - 0.5) ** 2) * 4
        r = int((16 + 18 * t) * v)
        g = int((17 + 6 * t) * v)
        b = int((30 + 20 * (1 - t)) * v)
        px[x, y] = (r, g, b)
img = bg.convert("RGBA")

# Speed streaks + the orange slash (the icon's motif), blurred copies underneath for glow.
layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
d = ImageDraw.Draw(layer)
slant = 0.55
def band(x0, width, color):
    d.polygon([(x0, 0), (x0 + width, 0), (x0 + width - H * slant, H), (x0 - H * slant, H)], fill=color)
band(2060, 250, (232, 92, 28, 235))
band(2335, 44, (255, 176, 48, 255))
band(2425, 16, (255, 120, 40, 200))
for i, (x0, w, a) in enumerate([(1820, 5, 60), (1900, 3, 50), (2520, 5, 90), (2600, 3, 70), (2690, 8, 60)]):
    band(x0, w, (255, 150, 60, a))
glow = layer.filter(ImageFilter.GaussianBlur(28))
img = Image.alpha_composite(img, glow)
img = Image.alpha_composite(img, layer)

# Fade the slash out under the text column so the words stay readable.
fade = Image.new("RGBA", (W, H), (0, 0, 0, 0))
fd = ImageDraw.Draw(fade)
for x in range(0, 1750):
    a = int(200 * max(0.0, 1 - x / 1750) ** 1.4)
    fd.line([(x, 0), (x, H)], fill=(14, 15, 26, a))
img = Image.alpha_composite(img, fade)

# The port's icon on the right, over the slash.
icon = Image.open(ICON).convert("RGBA").resize((520, 520), Image.LANCZOS)
shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
shadow.paste((0, 0, 0, 160), (1972, 182, 1972 + 520, 182 + 520), icon.split()[3])
img = Image.alpha_composite(img, shadow.filter(ImageFilter.GaussianBlur(24)))
img.alpha_composite(icon, (1950, 160))

# Typography.
d = ImageDraw.Draw(img)
x = 150
d.text((x, 150), "NATIVE PC PORT", font=font(BOLD, 54), fill=(255, 150, 60, 255), spacing=0)
title = font(BLACK, 150)
d.text((x + 6, 238), "TEKKEN TAG", font=title, fill=(0, 0, 0, 140))
d.text((x, 230), "TEKKEN TAG", font=title, fill=(250, 250, 252, 255))
d.text((x + 6, 408), "TOURNAMENT 2", font=title, fill=(0, 0, 0, 140))
d.text((x, 400), "TOURNAMENT 2", font=title, fill=(250, 250, 252, 255))
d.rectangle([x, 600, x + 250, 610], fill=(232, 92, 28, 255))
d.text((x, 640), "Not an emulator  \u2022  60 fps up to 4K  \u2022  Online with friends over Steam",
       font=font(SEMI, 46), fill=(214, 216, 230, 255))

img.convert("RGB").save(OUT, optimize=True)
print("wrote", OUT, os.path.getsize(OUT), "bytes")

# Optional second output: the README's download button (no version in it, so it never goes stale).
if len(sys.argv) > 2:
    TITLE, SUB = "Download for Windows", "Free  •  Windows 10 & 11  •  Bring your own copy of the game"
    probe = ImageDraw.Draw(Image.new("RGBA", (1, 1)))
    textW = max(probe.textlength(TITLE, font=font(BLACK, 72)), probe.textlength(SUB, font=font(SEMI, 36)))
    BW, BH, R = int(230 + textW + 70), 200, 36
    btn = Image.new("RGBA", (BW, BH), (0, 0, 0, 0))
    grad = Image.new("RGBA", (BW, BH))
    gd = ImageDraw.Draw(grad)
    for y in range(BH):
        t = y / (BH - 1)
        gd.line([(0, y), (BW, y)], fill=(int(246 - 22 * t), int(112 - 30 * t), int(36 - 12 * t), 255))
    mask = Image.new("L", (BW, BH), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, BW - 1, BH - 1], radius=R, fill=255)
    btn.paste(grad, (0, 0), mask)
    bd = ImageDraw.Draw(btn)
    bd.rounded_rectangle([3, 3, BW - 4, BH - 4], radius=R - 3, outline=(255, 200, 140, 120), width=3)
    # Download arrow into a tray.
    cx, cy = 130, 100
    bd.rectangle([cx - 11, cy - 58, cx + 11, cy + 4], fill=(255, 255, 255, 255))
    bd.polygon([(cx - 40, cy - 2), (cx + 40, cy - 2), (cx, cy + 40)], fill=(255, 255, 255, 255))
    bd.rounded_rectangle([cx - 52, cy + 52, cx + 52, cy + 64], radius=6, fill=(255, 255, 255, 255))
    bd.text((230, 34), TITLE, font=font(BLACK, 72), fill=(255, 255, 255, 255))
    bd.text((232, 120), SUB, font=font(SEMI, 36), fill=(255, 236, 220, 255))
    btn.save(sys.argv[2], optimize=True)
    print("wrote", sys.argv[2], os.path.getsize(sys.argv[2]), "bytes")
