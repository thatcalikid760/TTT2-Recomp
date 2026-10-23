# Draws the port's own icon (original art: a "T2" monogram on a dark tile with an orange
# diagonal slash) and saves a multi-size .ico. No game art is used.
import sys
from PIL import Image, ImageDraw, ImageFont

out = sys.argv[1]
S = 256
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

# Rounded dark tile with a vertical gradient.
tile = Image.new("RGBA", (S, S), (0, 0, 0, 0))
grad = Image.new("RGBA", (S, S))
gd = ImageDraw.Draw(grad)
for y in range(S):
    t = y / (S - 1)
    gd.line([(0, y), (S, y)], fill=(int(28 + 10 * t), int(30 + 4 * t), int(42 - 12 * t), 255))
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle([8, 8, S - 9, S - 9], radius=44, fill=255)
tile.paste(grad, (0, 0), mask)
img = Image.alpha_composite(img, tile)

# Orange/red diagonal slash behind the letters, clipped to the tile.
slash = Image.new("RGBA", (S, S), (0, 0, 0, 0))
sd = ImageDraw.Draw(slash)
sd.polygon([(150, 8), (226, 8), (106, 248), (30, 248)], fill=(232, 92, 28, 255))
sd.polygon([(196, 8), (214, 8), (94, 248), (76, 248)], fill=(255, 176, 48, 255))
clipped = Image.new("RGBA", (S, S), (0, 0, 0, 0))
clipped.paste(slash, (0, 0), mask)
img = Image.alpha_composite(img, clipped)

# "T2" monogram.
font = None
for name in ("C:/Windows/Fonts/seguibl.ttf", "C:/Windows/Fonts/arialbd.ttf", "C:/Windows/Fonts/segoeuib.ttf"):
    try:
        font = ImageFont.truetype(name, 150)
        break
    except OSError:
        pass
txt = Image.new("RGBA", (S, S), (0, 0, 0, 0))
td = ImageDraw.Draw(txt)
box = td.textbbox((0, 0), "T2", font=font)
w, h = box[2] - box[0], box[3] - box[1]
x, y = (S - w) // 2 - box[0], (S - h) // 2 - box[1]
td.text((x + 6, y + 8), "T2", font=font, fill=(0, 0, 0, 150))
td.text((x, y), "T2", font=font, fill=(250, 250, 250, 255))
img = Image.alpha_composite(img, txt)

# Thin light border.
ImageDraw.Draw(img).rounded_rectangle([8, 8, S - 9, S - 9], radius=44, outline=(255, 255, 255, 60), width=3)

img.save(out, format="ICO", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
if len(sys.argv) > 2:
    img.save(sys.argv[2])
print("wrote", out)
