#!/usr/bin/env python3
"""The Progress Log's Minecraft book: its pictures, fonts and sounds, taken from your own Minecraft install.

    mcassets.py [--jar client.jar] [--assets PrismLauncher/assets] [--out cstrike/custom/cstv] [--preview DIR]

Writes into --out (default: the CS:S install's cstrike/custom/cstv):
  resource/cstv/progbook.bin            the sprites, raw RGBA at their own size (the module upscales them), the
                                        font's glyph sheets (font/ascii.png, font/nonlatin_european.png) among them
  resource/cstv/progfont.txt            where each character is on them: "<hex code point> <sheet> <x> <y> <advance>"
                                        (sheet 0 ascii, 1 nonlatin, -1 a space; glyphs 8 px high, ascent 7)
  sound/progbook/flip1-3.wav, click.wav the book's page turn and the button click

The module draws text with the glyph pictures itself, like the game: VGUI on Linux never picks up a font file
added at run time (the TrueType version this made first came out as the system's sans).

These are Mojang's files out of your copy of the game: they stay in the game folder (never in this repo).
Without them the window draws a plain book in VGUI's font.

progbook.bin: "PBK1", u32 count, then count x (char name[32], u16 w, u16 h, u32 offset into the pixels),
then the pixels (RGBA, row by row). Little-endian.
"""
import argparse
import colorsys
import glob
import io
import json
import os
import struct
import subprocess
import sys
import zipfile

from PIL import Image

HOME = os.path.expanduser('~')
PRISM = os.path.join(HOME, '.local/share/PrismLauncher')
GAME = os.path.join(HOME, '.local/share/Steam/steamapps/common/Counter-Strike Source/cstrike/custom/cstv')


def newest_jar():
    jars = glob.glob(os.path.join(PRISM, 'libraries/com/mojang/minecraft/*/minecraft-*-client.jar'))
    if not jars:
        sys.exit('no Minecraft client jar under ' + PRISM + ' (pass --jar)')

    def key(p):   # versions like 1.21.11, 26.2: compare as numbers
        v = os.path.basename(os.path.dirname(p))
        return [int(x) if x.isdigit() else 0 for x in v.replace('-', '.').split('.')]
    return max(jars, key=key)


class Jar:
    def __init__(self, path):
        self.z = zipfile.ZipFile(path)

    def png(self, name):
        return Image.open(io.BytesIO(self.z.read('assets/minecraft/textures/' + name + '.png'))).convert('RGBA')

    def json(self, name):
        return json.loads(self.z.read('assets/minecraft/' + name))


# ---------------------------------------------------------------- sprites
SPRITES = [
    # name in progbook.bin      texture in the jar
    ('page_forward', 'gui/sprites/widget/page_forward'),
    ('page_forward_hl', 'gui/sprites/widget/page_forward_highlighted'),
    ('page_backward', 'gui/sprites/widget/page_backward'),
    ('page_backward_hl', 'gui/sprites/widget/page_backward_highlighted'),
    ('button', 'gui/sprites/widget/button'),
    ('button_hl', 'gui/sprites/widget/button_highlighted'),
    ('button_off', 'gui/sprites/widget/button_disabled'),
    ('scroller', 'gui/sprites/widget/scroller'),
    ('scroller_bg', 'gui/sprites/widget/scroller_background'),
    ('tooltip_bg', 'gui/sprites/tooltip/background'),
    ('tooltip_frame', 'gui/sprites/tooltip/frame'),
    ('popup', 'gui/sprites/popup/background'),
    ('xp_bg', 'gui/sprites/hud/experience_bar_background'),
    ('xp_green', 'gui/sprites/hud/experience_bar_progress'),
    ('tab', 'gui/sprites/recipe_book/tab'),
    ('tab_on', 'gui/sprites/recipe_book/tab_selected'),
    ('icon_book', 'item/writable_book'),
    ('icon_compass', 'item/compass_16'),
]


def hue_shift(im, hue_of_x):
    """The XP bar in another colour: each pixel's hue turned by hue_of_x(x) turns (its shading kept)."""
    out = im.copy()
    px = out.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            h, sat, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
            r2, g2, b2 = colorsys.hsv_to_rgb((h + hue_of_x(x)) % 1, sat, v)
            px[x, y] = (round(r2 * 255), round(g2 * 255), round(b2 * 255), a)
    return out


XP_HUE = 95 / 360   # the XP bar's green
RAINBOW_FRAMES = 12


def xp_bars(jar):
    """The XP bar's progress picture green (as it is), blue, red, and a rainbow: RAINBOW_FRAMES of them one
    under the other, the colours moving along a step each frame."""
    xp = jar.png('gui/sprites/hud/experience_bar_progress')
    out = [('xp_blue', hue_shift(xp, lambda x: 215 / 360 - XP_HUE)), ('xp_red', hue_shift(xp, lambda x: 1 - XP_HUE))]
    rainbow = Image.new('RGBA', (xp.width, xp.height * RAINBOW_FRAMES))
    for f in range(RAINBOW_FRAMES):
        rainbow.paste(hue_shift(xp, lambda x: x / xp.width - f / RAINBOW_FRAMES - XP_HUE), (0, f * xp.height))
    out.append(('xp_rainbow', rainbow))
    return out


def filled_map(jar):
    """The filled map icon: the map, and its markings tinted the colour the game gives them."""
    base = jar.png('item/filled_map')
    marks = jar.png('item/filled_map_markings')
    tint = (0x46, 0x40, 0x2E)
    mp = marks.load()
    for y in range(marks.height):
        for x in range(marks.width):
            r, g, b, a = mp[x, y]
            mp[x, y] = (r * tint[0] // 255, g * tint[1] // 255, b * tint[2] // 255, a)
    base.alpha_composite(marks)
    return base


def build_sprites(jar):
    out = []
    book = jar.png('gui/book')
    bb = book.getbbox()
    out.append(('book', book.crop(bb)))
    print('book: %dx%d at %s in gui/book.png' % (bb[2] - bb[0], bb[3] - bb[1], bb[:2]))
    for name, path in SPRITES:
        out.append((name, jar.png(path)))
    out += xp_bars(jar)
    out.append(('icon_map', filled_map(jar)))
    out.append(('font_ascii', jar.png('font/ascii')))
    out.append(('font_nonlatin', jar.png('font/nonlatin_european')))
    return out, bb


def write_bin(path, sprites):
    head = bytearray(b'PBK1' + struct.pack('<I', len(sprites)))
    blob = bytearray()
    for name, im in sprites:
        assert len(name) < 32
        head += name.encode().ljust(32, b'\0') + struct.pack('<HHI', im.width, im.height, len(blob))
        blob += im.tobytes()
    with open(path, 'wb') as f:
        f.write(head + blob)


# ---------------------------------------------------------------- the font
SHEETS = {'font/ascii.png': 0, 'font/nonlatin_european.png': 1}


def font_table(jar):
    """[(code point, sheet, x, y, advance)] from the font's bitmap providers (the first with a char wins, as in the
    game; advance = the glyph's width + 1) and its spaces (sheet -1)."""
    out = {}
    spaces = {}
    for p in jar.json('font/include/default.json')['providers'] + jar.json('font/include/space.json')['providers']:
        if p['type'] == 'space':
            spaces.update(p['advances'])
            continue
        f = p.get('file', '').split(':', 1)[-1]
        if p['type'] != 'bitmap' or f not in SHEETS:
            continue
        im = jar.png(f.rsplit('.png', 1)[0])
        rows = p['chars']
        cw, ch = im.width // len(rows[0]), im.height // len(rows)
        px = im.load()
        for r, row in enumerate(rows):
            for c, chr_ in enumerate(row):
                if chr_ in ('\0', ' ') or ord(chr_) in out:
                    continue
                cols = [x for x in range(cw) if any(px[c * cw + x, r * ch + y][3] for y in range(ch))]
                if cols:
                    out[ord(chr_)] = (SHEETS[f], c * cw, r * ch, max(cols) + 2)
    for chr_, adv in spaces.items():
        out.setdefault(ord(chr_), (-1, 0, 0, adv))
    return sorted((cp,) + v for cp, v in out.items())


# ---------------------------------------------------------------- sounds
SOUNDS = [('flip1', 'minecraft/sounds/item/book/open_flip1.ogg'), ('flip2', 'minecraft/sounds/item/book/open_flip2.ogg'),
          ('flip3', 'minecraft/sounds/item/book/open_flip3.ogg'), ('click', 'minecraft/sounds/random/click.ogg')]


def build_sounds(assets, outdir):
    objects = {}
    for idx in sorted(glob.glob(os.path.join(assets, 'indexes/*.json')), key=os.path.getmtime):
        objects.update(json.load(open(idx)).get('objects', {}))
    os.makedirs(outdir, exist_ok=True)
    for name, key in SOUNDS:
        if key not in objects:
            print('no sound', key)
            continue
        h = objects[key]['hash']
        src = os.path.join(assets, 'objects', h[:2], h)
        if not os.path.exists(src):
            print('not downloaded:', key)
            continue
        dst = os.path.join(outdir, name + '.wav')
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', src, '-ac', '1', '-ar', '44100', '-sample_fmt', 's16', dst], check=True)
        print(dst)


# ---------------------------------------------------------------- checking
def preview(sprites, d):
    os.makedirs(d, exist_ok=True)
    s = dict(sprites)
    book = s['book']
    spread = Image.new('RGBA', (book.width * 2, book.height))
    spread.paste(book.transpose(Image.FLIP_LEFT_RIGHT), (0, 0))
    spread.paste(book, (book.width, 0))
    spread.resize((spread.width * 5, spread.height * 5), Image.NEAREST).save(os.path.join(d, 'spread.png'))
    w = sum(im.width for _, im in sprites[1:]) + 4 * len(sprites)
    sheet = Image.new('RGBA', (w, max(im.height for _, im in sprites[1:])), (60, 60, 60, 255))
    x = 0
    for _, im in sprites[1:]:
        sheet.alpha_composite(im, (x, 0))
        x += im.width + 4
    sheet.resize((sheet.width * 3, sheet.height * 3), Image.NEAREST).save(os.path.join(d, 'sprites.png'))
    print('previews in', d)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--jar')
    ap.add_argument('--assets', default=os.path.join(PRISM, 'assets'))
    ap.add_argument('--out', default=GAME)
    ap.add_argument('--preview')
    a = ap.parse_args()
    jar_path = a.jar or newest_jar()
    print('jar:', jar_path)
    jar = Jar(jar_path)
    res = os.path.join(a.out, 'resource/cstv')
    os.makedirs(res, exist_ok=True)
    sprites, _ = build_sprites(jar)
    write_bin(os.path.join(res, 'progbook.bin'), sprites)
    print(os.path.join(res, 'progbook.bin') + ': %d sprites' % len(sprites))
    table = font_table(jar)
    with open(os.path.join(res, 'progfont.txt'), 'w') as f:
        for cp, sheet, x, y, adv in table:
            f.write('%x %d %d %d %d\n' % (cp, sheet, x, y, adv))
    print(os.path.join(res, 'progfont.txt') + ': %d characters' % len(table))
    for old in ('progmc.ttf', 'progmc-bold.ttf'):   # (the TrueType tried first: VGUI never used it)
        if os.path.exists(os.path.join(res, old)):
            os.remove(os.path.join(res, old))
    build_sounds(a.assets, os.path.join(a.out, 'sound/progbook'))
    if a.preview:
        preview(sprites, a.preview)


if __name__ == '__main__':
    main()
