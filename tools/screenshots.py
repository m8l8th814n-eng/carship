#!/usr/bin/env python3
"""Render every preset's prompt to a PNG.

    tools/screenshots.py [build/carship] [outdir]

Runs the prompt with each embedded preset in the repository itself, so git
modules have something to show, and paints the ANSI output cell by cell with a
Nerd Font. No terminal or display is involved, which keeps the images
reproducible.
"""
import os
import re
import subprocess
import sys
import tempfile
import unicodedata

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

FONT_DIR = "/usr/share/fonts/TTF"
FONTS = {
    (False, False): "JetBrainsMonoNerdFontMono-Regular.ttf",
    (True, False): "JetBrainsMonoNerdFontMono-Bold.ttf",
    (False, True): "JetBrainsMonoNerdFontMono-Italic.ttf",
    (True, True): "JetBrainsMonoNerdFontMono-BoldItalic.ttf",
}
FALLBACKS = [
    "/usr/share/fonts/TTF/SymbolsNerdFontMono-Regular.ttf",
    "/usr/share/fonts/noto/NotoSansSymbols2-Regular.ttf",
    "/usr/share/fonts/noto/NotoSansMath-Regular.ttf",
]
EMOJI = "/usr/share/fonts/noto/NotoColorEmoji.ttf"

SIZE = 28
COLS = 80
ROWS = 3
PAD_X, PAD_Y = 24, 20
BG = (30, 30, 46)
FG = (205, 214, 244)

# Catppuccin Mocha, so the 16 named colours sit well on BG.
ANSI16 = [
    (69, 71, 90), (243, 139, 168), (166, 227, 161), (249, 226, 175),
    (137, 180, 250), (245, 194, 231), (148, 226, 213), (186, 194, 222),
    (88, 91, 112), (243, 139, 168), (166, 227, 161), (249, 226, 175),
    (137, 180, 250), (245, 194, 231), (148, 226, 213), (166, 173, 200),
]


def xterm256(n):
    if n < 16:
        return ANSI16[n]
    if n < 232:
        n -= 16
        steps = [0, 95, 135, 175, 215, 255]
        return steps[n // 36], steps[n // 6 % 6], steps[n % 6]
    v = 8 + (n - 232) * 10
    return v, v, v


def parse(text):
    """Yield (char, fg, bg, bold, italic, underline) per character."""
    fg = bg = None
    bold = italic = under = False
    for m in re.finditer(r"\x1b\[([0-9;]*)m|([^\x1b])", text, re.S):
        if m.group(2) is not None:
            yield m.group(2), fg, bg, bold, italic, under
            continue
        codes = [int(c) if c else 0 for c in m.group(1).split(";")]
        i = 0
        while i < len(codes):
            c = codes[i]
            if c == 0:
                fg = bg = None
                bold = italic = under = False
            elif c == 1:
                bold = True
            elif c == 3:
                italic = True
            elif c == 4:
                under = True
            elif c == 22:
                bold = False
            elif c == 23:
                italic = False
            elif c == 24:
                under = False
            elif c in (38, 48):
                if codes[i + 1] == 2:
                    col = tuple(codes[i + 2:i + 5])
                    i += 4
                else:
                    col = xterm256(codes[i + 2])
                    i += 2
                if c == 38:
                    fg = col
                else:
                    bg = col
            elif c == 39:
                fg = None
            elif c == 49:
                bg = None
            elif 30 <= c <= 37:
                fg = ANSI16[c - 30]
            elif 40 <= c <= 47:
                bg = ANSI16[c - 40]
            elif 90 <= c <= 97:
                fg = ANSI16[c - 90 + 8]
            elif 100 <= c <= 107:
                bg = ANSI16[c - 100 + 8]
            i += 1


def width(ch):
    if unicodedata.combining(ch) or ch in "‍︎️":
        return 0
    return 2 if unicodedata.east_asian_width(ch) in "WF" else 1


class Fonts:
    def __init__(self):
        self.faces = {k: ImageFont.truetype(os.path.join(FONT_DIR, v), SIZE)
                      for k, v in FONTS.items()}
        self.cmap = TTFont(os.path.join(FONT_DIR, FONTS[(False, False)])).getBestCmap()
        self.fallbacks = [(ImageFont.truetype(p, SIZE), TTFont(p).getBestCmap())
                          for p in FALLBACKS if os.path.exists(p)]
        self.emoji = ImageFont.truetype(EMOJI, 109)
        self.emoji_cmap = TTFont(EMOJI).getBestCmap()
        box = self.faces[(False, False)].getbbox("M")
        self.cw = round(self.faces[(False, False)].getlength("M"))
        self.ch = round(SIZE * 1.3)
        self.baseline = (self.ch - (box[3] - box[1])) // 2 - box[1]

    def pick(self, ch, bold, italic):
        if ord(ch) in self.cmap:
            return self.faces[(bold, italic)]
        for face, cmap in self.fallbacks:
            if ord(ch) in cmap:
                return face
        if ord(ch) in self.emoji_cmap:
            return self.emoji
        return self.faces[(bold, italic)]


def layout(text):
    """Split rendered output into rows of cells."""
    rows = [[]]
    for ch, fg, bg, bold, italic, under in parse(text):
        if ch == "\n":
            rows.append([])
        elif ch != "\r" and width(ch):
            rows[-1].append((ch, fg, bg, bold, italic, under))
    return rows


def render(left, right, fonts, path):
    rows = layout(left)
    rrow = layout(right)[0] if right else []
    rw = sum(width(c[0]) for c in rrow)
    last = sum(width(c[0]) for c in rows[-1])
    # Every image gets the same canvas, so they line up in a README.
    img = Image.new("RGB", (PAD_X * 2 + COLS * fonts.cw,
                            PAD_Y * 2 + ROWS * fonts.ch), BG)
    draw = ImageDraw.Draw(img)

    def cells(row, x0, y):
        x = x0
        for ch, fg, bg, bold, italic, under in row:
            w = width(ch)
            px, py = PAD_X + x * fonts.cw, PAD_Y + y * fonts.ch
            if bg:
                draw.rectangle([px, py, px + w * fonts.cw - 1, py + fonts.ch - 1], fill=bg)
            face = fonts.pick(ch, bold, italic)
            if face is fonts.emoji:
                glyph = Image.new("RGBA", (136, 128))
                ImageDraw.Draw(glyph).text((0, 0), ch, font=face, embedded_color=True)
                glyph = glyph.crop(glyph.getbbox() or (0, 0, 1, 1))
                s = min(w * fonts.cw / glyph.width, fonts.ch * 0.85 / glyph.height)
                glyph = glyph.resize((max(1, round(glyph.width * s)),
                                      max(1, round(glyph.height * s))), Image.LANCZOS)
                img.paste(glyph, (px + (w * fonts.cw - glyph.width) // 2,
                                  py + (fonts.ch - glyph.height) // 2), glyph)
            else:
                draw.text((px, py + fonts.baseline), ch, font=face, fill=fg or FG)
            if under:
                draw.line([px, py + fonts.ch - 3, px + w * fonts.cw, py + fonts.ch - 3],
                          fill=fg or FG, width=2)
            x += w
        return x

    end = 0
    for y, row in enumerate(rows):
        end = cells(row, 0, y)
    if rrow:
        # A right prompt that does not fit is pushed right and clipped.
        cells(rrow, max(COLS - rw, last + 2), len(rows) - 1)
    # A block cursor where input would start.
    px, py = PAD_X + end * fonts.cw, PAD_Y + (len(rows) - 1) * fonts.ch
    draw.rectangle([px, py + 2, px + fonts.cw - 2, py + fonts.ch - 3], fill=FG)
    img.save(path, optimize=True)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    binary = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "build/carship"))
    outdir = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "screenshots"))
    os.makedirs(outdir, exist_ok=True)
    fonts = Fonts()

    names = subprocess.run([binary, "preset", "--list"], capture_output=True,
                           text=True, check=True).stdout.split()
    with tempfile.TemporaryDirectory() as tmp:
        cfg = os.path.join(tmp, "preset.toml")
        env = dict(os.environ, CARSHIP_CONFIG=cfg, PWD=root, COLUMNS=str(COLS))
        env.pop("CARSHIP_SHELL", None)
        for name in names:
            with open(cfg, "w") as f:
                f.write(subprocess.run([binary, "preset", name], capture_output=True,
                                       text=True, check=True).stdout)
            args = [binary, "prompt", "--shell", "none", "--terminal-width", str(COLS)]
            left = subprocess.run(args, cwd=root, env=env, capture_output=True, text=True).stdout
            right = subprocess.run(args + ["--right"], cwd=root, env=env,
                                   capture_output=True, text=True).stdout
            # The newline add_newline puts above the prompt is just blank space here.
            left = left.lstrip("\n")
            render(left, right.strip("\n"), fonts, os.path.join(outdir, name + ".png"))
            print(name)


if __name__ == "__main__":
    main()
