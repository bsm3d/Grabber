"""capture.ico: a dark rounded square with a red fairground claw (a grabber) hanging from its cable"""
from PIL import Image, ImageDraw

S = 256
K = 4                                                       # drawn 4 times bigger, then reduced: smooth edges
im = Image.new('RGBA', (S * K, S * K), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
RED = (229, 50, 45, 255)
WHITE = (236, 236, 236, 255)


def P(x, y):
    return (x * K, y * K)


def thick(points, w, col):
    """a thick line with round ends and round joints"""
    d.line([P(*p) for p in points], fill=col, width=w * K, joint='curve')
    for x, y in (points[0], points[-1]):
        d.ellipse((P(x - w / 2, y - w / 2), P(x + w / 2, y + w / 2)), fill=col)


def bezier(a, b, c, n=24):
    out = []
    for i in range(n + 1):
        t = i / n
        out.append(((1 - t) ** 2 * a[0] + 2 * (1 - t) * t * b[0] + t * t * c[0], (1 - t) ** 2 * a[1] + 2 * (1 - t) * t * b[1] + t * t * c[1]))
    return out


d.rounded_rectangle((P(8, 8), P(S - 8, S - 8)), radius=48 * K, fill=(27, 27, 27, 255), outline=(70, 70, 70, 255), width=6 * K)
d.rectangle((P(20, 22), P(S - 20, 32)), fill=(70, 70, 70, 255))                        # the rail the claw runs on
thick([(128, 28), (128, 92)], 8, WHITE)                                                 # the cable
thick([(112, 100), (88, 168)], 9, WHITE)                                                # left arm: white, thin, straight; behind the head
thick([(144, 100), (168, 168)], 9, WHITE)                                               # right arm
thick([(128, 100), (128, 160)], 9, WHITE)                                               # middle arm
thick([(88, 168), (82, 190), (98, 204)], 11, RED)                                       # the legs, red, with a small hook
thick([(168, 168), (174, 190), (158, 204)], 11, RED)
thick([(128, 160), (128, 196)], 11, RED)
d.rounded_rectangle((P(100, 88), P(156, 116)), radius=10 * K, fill=WHITE)               # the head, in front of the arms
im = im.resize((S, S), Image.LANCZOS)
im.save('capture.ico', sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
im.save('capture_preview.png')

# the pictures of the installer wizard: the icon on the dark grey of the program
big = Image.new('RGB', (164, 314), (72, 74, 77))
big.paste(im.resize((128, 128), Image.LANCZOS), (18, 90), im.resize((128, 128), Image.LANCZOS))
big.save('wizard.bmp')
small = Image.new('RGB', (55, 55), (72, 74, 77))
small.paste(im.resize((51, 51), Image.LANCZOS), (2, 2), im.resize((51, 51), Image.LANCZOS))
small.save('wizard_small.bmp')
