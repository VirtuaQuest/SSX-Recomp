# Draws the keyboard and mouse versions of the game's two controller atlases (white, with the picture in alpha, tinted by
# the UI): button icons (256 x 256, one version for gameplay and one for menus) and the controller diagram (512 x 512), every
# piece at the same place as in the original, since the UI's quads stay the same. Writes ARRecomp/gpu/mkb_art.inl (alpha planes,
# run length encoded). Contains no game art.
# Usage: python make_mkb_art.py [preview folder]
import os, sys
from PIL import Image, ImageDraw, ImageFont

SS = 4  # Supersampling.
FONT = 'C:/Windows/Fonts/arialbd.ttf'


def canvas(size):
    return Image.new('L', (size * SS, size * SS), 0)


def font(px):
    return ImageFont.truetype(FONT, int(px * SS))


def text(d, box, s, fill=255, scale=0.8, pad=0):
    # Text fitted to the box, centered.
    x0, y0, x1, y1 = [v * SS for v in box]
    w, h = x1 - x0 - pad * 2 * SS, y1 - y0 - pad * 2 * SS
    px = (y1 - y0) / SS
    while True:
        f = font(px * scale)
        l, t, r, b = d.textbbox((0, 0), s, font=f)
        if (r - l <= w and b - t <= h) or px < 4:
            break
        px -= 0.5
    d.text(((x0 + x1 - (r - l)) / 2 - l, (y0 + y1 - (b - t)) / 2 - t), s, font=f, fill=fill)


def pill(d, box, s, radius=5, width=1.5):
    x0, y0, x1, y1 = [v * SS for v in box]
    d.rounded_rectangle((x0 + 1, y0 + 1, x1 - 1, y1 - 1), radius * SS, outline=255, width=int(width * SS))
    text(d, box, s, 255, 0.62, 3)


def stun_icons():
    # While stunned: the buttons, moved to the left, are W, A, S and D, and the stick, moved to the right, is the mouse with both
    # buttons to mash. Only what those prompts use.
    im = Image.new('L', (256 * SS, 256 * SS), 0)
    d = ImageDraw.Draw(im)
    text(d, (135, 66, 171, 96), 'W', 255, 1.0)
    text(d, (169, 6, 201, 36), 'A', 255, 1.0)
    text(d, (125, 6, 157, 36), 'S', 255, 1.0)
    text(d, (93, 66, 125, 96), 'D', 255, 1.0)
    # The mouse, in the stick's ring cell.
    x0, y0, x1, y1 = [v * SS for v in (73, 6, 107, 50)]
    cx = (x0 + x1) / 2
    split = y0 + (y1 - y0) * 0.46
    r = (x1 - x0) * 0.46
    mask = Image.new('L', im.size, 0)
    md = ImageDraw.Draw(mask)
    md.rounded_rectangle((x0, y0, x1, y1), r, fill=255)
    md.rectangle((x0, split, x1, y1), fill=0)
    im.paste(255, (0, 0), mask)
    d.rounded_rectangle((x0, y0, x1, y1), r, outline=255, width=int(2 * SS))
    d.line((x0, split, x1, split), fill=255, width=int(1.5 * SS))
    d.line((cx, y0, cx, split), fill=0, width=int(1.5 * SS))
    return im.resize((256, 256), Image.LANCZOS)


def mouse_icon(d, im, box, side, ring=False):
    # A mouse from above with the left or right button filled, or (side None) just the mouse, for moving it.
    x0, y0, x1, y1 = [v * SS for v in box]
    if ring:
        d.ellipse((x0 + 1, y0 + 1, x1 - 1, y1 - 1), outline=255, width=int(1.5 * SS))
        inset = (x1 - x0) * 0.22
        x0, y0, x1, y1 = x0 + inset, y0 + inset * 0.8, x1 - inset, y1 - inset * 0.8
    w, h = x1 - x0, y1 - y0
    mw = min(w, h * 0.72)
    cx = (x0 + x1) / 2
    bx0, bx1 = cx - mw / 2, cx + mw / 2
    line = max(int(1.2 * SS), int(mw * 0.09))
    split = y0 + h * 0.46
    r = mw * 0.46
    if side in ('left', 'right'):
        half = Image.new('L', im.size, 0)
        hd = ImageDraw.Draw(half)
        hd.rounded_rectangle((bx0, y0, bx1, y1), r, fill=255)
        hd.rectangle((bx0, split, bx1, y1), fill=0)
        if side == 'left':
            hd.rectangle((cx, y0, bx1, split), fill=0)
        else:
            hd.rectangle((bx0, y0, cx, split), fill=0)
        im.paste(255, (0, 0), half)
    d.rounded_rectangle((bx0, y0, bx1, y1), r, outline=255, width=line)
    d.line((bx0, split, bx1, split), fill=255, width=line)
    d.line((cx, y0, cx, split), fill=255, width=line)


def circled(d, box, s):
    x0, y0, x1, y1 = [v * SS for v in box]
    d.ellipse((x0 + 1, y0 + 1, x1 - 1, y1 - 1), outline=255, width=int(1.5 * SS))
    text(d, (box[0] + 4, box[1] + 4, box[2] - 4, box[3] - 4), s, 255, 0.95)


def icons(menu, viewer=False):
    # What each of the controller's icons stands for, in gameplay or in menus.
    im = canvas(256)
    d = ImageDraw.Draw(im)
    # Rings and a disc the originals have too.
    for box in ((65, 3, 115, 53), (145, 177, 171, 203)):
        x0, y0, x1, y1 = [v * SS for v in box]
        d.ellipse((x0 + 2, y0 + 2, x1 - 2, y1 - 2), outline=255, width=int(1.5 * SS))
    x0, y0, x1, y1 = [v * SS for v in (1, 203, 27, 229)]
    d.ellipse((x0, y0, x1, y1), fill=255)

    a_box, x_box, b_box, y_box = (127, 9, 155, 33), (171, 7, 199, 35), (97, 67, 121, 95), (139, 67, 167, 95)
    a_ring, x_ring, b_ring, y_ring = (207, 174, 229, 196), (119, 182, 141, 204), (179, 172, 201, 194), (91, 180, 113, 202)
    if menu:
        # Left click confirms, right click goes back, R is the default settings button.
        mouse_icon(d, im, a_box, 'left')
        mouse_icon(d, im, a_ring, 'left', True)
        mouse_icon(d, im, b_box, 'right')
        mouse_icon(d, im, b_ring, 'right', True)
        text(d, y_box, 'R', 255, 1.0)
        circled(d, y_ring, 'R')
        text(d, x_box, 'X', 255, 1.0)
        circled(d, x_ring, 'X')
    else:
        # Space jumps, E grabs, the mouse buttons attack.
        text(d, a_box, 'SPC', 255, 0.85)
        circled(d, a_ring, 'SP')
        text(d, b_box, 'E', 255, 1.1)
        circled(d, b_ring, 'E')
        mouse_icon(d, im, x_box, 'left')
        mouse_icon(d, im, x_ring, 'left', True)
        mouse_icon(d, im, y_box, 'right')
        mouse_icon(d, im, y_ring, 'right', True)

    # Shoulder buttons and triggers: lock-on, guard (either of two), the killer weapon.
    pill(d, (3, 65, 41, 87), 'CTRL')
    pill(d, (47, 65, 85, 87), 'TAB')
    pill(d, (179, 65, 212, 87), 'C')
    pill(d, (3, 99, 36, 121), 'SHIFT')
    # Start, back, stick clicks.
    pill(d, (47, 96, 79, 124), 'ENT' if menu else 'ESC')
    pill(d, (129, 142, 158, 170), 'F')
    pill(d, (129, 108, 158, 132), 'Q')
    pill(d, (3, 134, 32, 158), 'Q')
    # D-pad: taunts (up and down), weapon slots (left and right). In menus they move the cursor.
    pill(d, (184, 100, 203, 121), 'T/G' if viewer else 'W/S' if menu else 'T/V', 4, 1.2)
    pill(d, (228, 109, 247, 129), 'S' if menu else 'V', 4, 1.2)
    pill(d, (46, 136, 65, 156), 'W' if menu else 'T', 4, 1.2)
    pill(d, (91, 111, 120, 130), 'A/D' if menu else '1/2', 4, 1.2)
    pill(d, (179, 137, 203, 156), 'A' if menu else '1', 4, 1.2)
    pill(d, (96, 145, 120, 164), 'D' if menu else '2', 4, 1.2)
    # The whole D-pad: between the HUD's two weapon slots.
    pill(d, (4, 171, 30, 193), 'WASD' if menu else '1 2', 4, 1.2)
    # Sticks: move, and the camera.
    x0, y0, x1, y1 = [v * SS for v in (223, 137, 249, 163)]
    d.rounded_rectangle((x0 + 1, y0 + 1, x1 - 1, y1 - 1), 5 * SS, outline=255, width=int(1.2 * SS))
    text(d, (225, 139, 247, 150), 'W' if viewer else 'WA', 255, 1.0)
    text(d, (225, 150, 247, 161), 'S' if viewer else 'SD', 255, 1.0)
    mouse_icon(d, im, (39, 171, 65, 197), None, True)
    return im.resize((256, 256), Image.LANCZOS)


# The keys the default bindings use in gameplay. (Enter, Backspace, R and the arrow keys only work in menus.)
DEFAULT_BOUND = {'ESC', '1', '2', 'TAB', 'W', 'E', 'T', 'A', 'S', 'D', 'F', 'Q', 'SHIFT', 'C', 'V', 'CTRL', 'SPACE'}
BOUND = DEFAULT_BOUND
KEY_BOXES = {}  # Where each key was last drawn, for the keyboard page.

# Windows' virtual key codes of the keys drawn.
VK = {'ESC': 0x1B, '`': 0xC0, '-': 0xBD, '=': 0xBB, 'BKSP': 0x08, 'TAB': 0x09, '[': 0xDB, ']': 0xDD, '\\': 0xDC, 'CAPS': 0x14, ';': 0xBA, "'": 0xDE, 'ENTER': 0x0D,
      'SHIFT': 0xA0, ',': 0xBC, '.': 0xBE, '/': 0xBF, 'RSHIFT': 0xA1, 'CTRL': 0xA2, 'WIN': 0x5B, 'ALT': 0xA4, 'SPACE': 0x20, 'RALT': 0xA5, 'MENU': 0x5D, 'RCTRL': 0xA3,
      '^': 0x26, 'v': 0x28, '<': 0x25, '>': 0x27, 'LEFT CLICK': 0x01, 'RIGHT CLICK': 0x02}
for i in range(1, 13):
    VK['F%d' % i] = 0x6F + i
for c in '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ':
    VK[c] = ord(c)


def key(d, box, label):
    x0, y0, x1, y1 = [v * SS for v in box]
    r = 3 * SS
    KEY_BOXES[label] = box
    if label in BOUND:
        # Filled, with the letter punched out, like the buttons on the controller art.
        d.rounded_rectangle((x0, y0, x1, y1), r, fill=170)
        d.rounded_rectangle((x0, y0, x1, y1), r, outline=255, width=int(1.2 * SS))
        text(d, box, label, 0, 0.6, 2)
    else:
        d.rounded_rectangle((x0, y0, x1, y1), r, outline=90, width=int(0.8 * SS))
        text(d, box, label, 90, 0.5, 2)


def row(d, y, height, x, keys, unit, gap):
    for label, width in keys:
        w = unit * width
        if label:
            key(d, (x, y, x + w - gap, y + height), label)
        x += w


def diagram():
    im = canvas(512)
    d = ImageDraw.Draw(im)
    # Where the controller's triggers are: a title, and how to read the picture.
    text(d, (39, 14, 323, 54), 'KEYBOARD + MOUSE', 255, 0.62)
    text(d, (39, 58, 323, 84), 'Highlighted keys do something', 200, 0.62)

    # Where the controller's body is: the whole keyboard, then the mouse below it.
    x0 = 2
    unit = 357 / 15.0
    gap = 1.5
    height = 21
    y = 110
    step = height + 2
    row(d, y, height - 3, x0, [('ESC', 1), ('', 0.5)] + [('F%d' % i, 1) for i in range(1, 5)] + [('', 0.25)] + [('F%d' % i, 1) for i in range(5, 9)] + [('', 0.25)] +
        [('F%d' % i, 1) for i in range(9, 13)], unit * 0.98, gap)
    y += step + 1
    row(d, y, height, x0, [('`', 1)] + [(c, 1) for c in '1234567890'] + [('-', 1), ('=', 1), ('BKSP', 2)], unit, gap)
    y += step
    row(d, y, height, x0, [('TAB', 1.5)] + [(c, 1) for c in 'QWERTYUIOP'] + [('[', 1), (']', 1), ('\\', 1.5)], unit, gap)
    y += step
    row(d, y, height, x0, [('CAPS', 1.75)] + [(c, 1) for c in 'ASDFGHJKL'] + [(';', 1), ("'", 1), ('ENTER', 2.25)], unit, gap)
    y += step
    row(d, y, height, x0, [('SHIFT', 2.25)] + [(c, 1) for c in 'ZXCVBNM'] + [(',', 1), ('.', 1), ('/', 1), ('RSHIFT', 2.75)], unit, gap)
    y += step
    row(d, y, height, x0, [('CTRL', 1.5), ('WIN', 1.25), ('ALT', 1.25), ('SPACE', 5.5), ('RALT', 1.25), ('FN', 1.25), ('MENU', 1.25), ('RCTRL', 1.5)], unit, gap)
    y += step + 4

    # Mouse.
    bx0, by0, bx1, by1 = [v * SS for v in (112, y, 200, 364)]
    r = 34 * SS
    cx = (bx0 + bx1) / 2
    split = by0 + (by1 - by0) * 0.46
    wheel_y = int(y + 8)
    left_lit, right_lit = 'LEFT CLICK' in BOUND or BOUND is DEFAULT_BOUND, 'RIGHT CLICK' in BOUND or BOUND is DEFAULT_BOUND
    mid = (cx / SS)
    split_y = split / SS
    KEY_BOXES['LEFT CLICK'] = (112, y, mid, split_y)
    KEY_BOXES['RIGHT CLICK'] = (mid, y, 200, split_y)
    for lit, x_a, x_b in ((left_lit, bx0, cx), (right_lit, cx, bx1)):
        if lit:
            mask = Image.new('L', im.size, 0)
            md = ImageDraw.Draw(mask)
            md.rounded_rectangle((bx0, by0, bx1, by1), r, fill=255)
            md.rectangle((bx0, split, bx1, by1), fill=0)
            md.rectangle((bx0, by0, x_a, split), fill=0)
            md.rectangle((x_b, by0, bx1, split), fill=0)
            im.paste(170, (0, 0), mask)
    d.rounded_rectangle((bx0, by0, bx1, by1), r, outline=255, width=2 * SS)
    d.line((bx0, split, bx1, split), fill=255, width=int(1.5 * SS))
    d.line((cx, by0, cx, split), fill=255, width=int(1.5 * SS))
    d.rounded_rectangle((cx - 5 * SS, wheel_y * SS, cx + 5 * SS, (wheel_y + 22) * SS), 5 * SS, fill=255)
    text(d, (116, y + 14, 146, y + 40), 'L', 0 if left_lit else 110, 1.0)
    text(d, (166, y + 14, 196, y + 40), 'R', 0 if right_lit else 110, 1.0)
    text(d, (120, y + 56, 192, y + 84), 'CAMERA', 255, 0.62)

    # The arrow keys, which work menus, and a note for the wheel.
    ax, ay, size = 250, y + 22, 26
    key(d, (ax + size + 2, ay, ax + size * 2 + 2, ay + size), '^')
    key(d, (ax, ay + size + 2, ax + size, ay + size * 2 + 2), '<')
    key(d, (ax + size + 2, ay + size + 2, ax + size * 2 + 2, ay + size * 2 + 2), 'v')
    key(d, (ax + size * 2 + 4, ay + size + 2, ax + size * 3 + 4, ay + size * 2 + 2), '>')
    text(d, (6, y + 16, 106, y + 42), 'WHEEL', 255, 0.62)
    text(d, (6, y + 42, 106, y + 66), 'weapon slots', 200, 0.62)
    return im.resize((512, 512), Image.LANCZOS)


# ---- Overlay: what the renderer draws on top of the game for the rows added to Options, and the two controls pages ----

BAHN = 'C:/Windows/Fonts/bahnschrift.ttf'
OVERLAY_TEXTS = ['MOUSE AIMING CAMERA', 'CONTROLLER CONTROLS', 'KEYBOARD AND MOUSE CONTROLS', 'CONTROLLER AIMING CAMERA',
                 'N', 'Y', '1', '2', '3', '4', '5', '6', '7', '8', '9', '10', 'UP/DOWN', 'LEFT/RIGHT', 'SPEED']


def bahn(px, name='SemiBold Condensed'):
    f = ImageFont.truetype(BAHN, int(px * SS))
    f.set_variation_by_name(name)
    return f


def overlay_atlas():
    # Strings, 1 x 1 pixels of white for solid rectangles first. Returns the picture and each string's rectangle.
    w, h = 1024, 256
    im = Image.new('L', (w * SS, h * SS), 0)
    d = ImageDraw.Draw(im)
    d.rectangle((0, 0, 8 * SS, 8 * SS), fill=255)
    f = bahn(24)
    x, y = 12, 0
    rects = []
    for s in OVERLAY_TEXTS:
        l, t, r, b = d.textbbox((0, 0), s, font=f)
        tw = (r - l) // SS + 3
        if x + tw > w:
            x, y = 0, y + 30
        d.text((x * SS - l, (y + 2) * SS - t), s, font=f, fill=255)
        rects.append((x, y, tw, (b - t) // SS + 5))
        x += tw + 6
    return im.resize((w, h), Image.LANCZOS), rects


LEFT_PAD = [('Move', 'Left stick'), ('Use Killer Weapon', 'LT'), ('Strong Killer Weapon Attack', 'LT + Y'), ('Weak Killer Weapon Attack', 'LT + X'), ('Lock-On', 'LB'), ('Taunt', 'D-pad up or down'),
            ('Use Item', 'D-pad left or right'), ('Unleash Rampage', 'LS + RS'), ('Start Mission', 'Back at a marker'), ('Pause', 'Start')]
RIGHT_PAD = [('Normal Attack', 'X'), ('Heavy Attack', 'Y'), ('Jump', 'A'), ('Grab', 'B'), ('Guard', 'RB or RT'), ('Evade', 'RB or RT + A'), ('360 Attack', 'X + A'), ('Move Camera', 'Right stick'),
             ('Break Out Of A Stun', 'Mash buttons + stick')]


def pad_picture(d, cx, cy, s):
    # A plain controller, drawn for this page.
    def box(x0, y0, x1, y1):
        return tuple(int(v * SS) for v in (cx + x0 * s, cy + y0 * s, cx + x1 * s, cy + y1 * s))
    line = int(3 * SS)
    d.rounded_rectangle(box(-180, -70, 180, 60), int(55 * s * SS), outline=255, width=line)
    d.ellipse(box(-190, -40, -80, 140), outline=255, width=line)
    d.ellipse(box(80, -40, 190, 140), outline=255, width=line)
    d.rounded_rectangle(box(-165, -100, -80, -72), int(10 * s * SS), outline=255, width=line)
    d.rounded_rectangle(box(80, -100, 165, -72), int(10 * s * SS), outline=255, width=line)
    d.ellipse(box(-135, -45, -85, 5), outline=255, width=line)
    d.ellipse(box(35, 10, 85, 60), outline=255, width=line)
    d.rectangle(box(-72, 18, -48, 66), outline=255, width=line)
    d.rectangle(box(-84, 30, -36, 54), outline=255, width=line)
    for x, y, c in ((120, -35, 'Y'), (95, -10, 'X'), (145, -10, 'B'), (120, 15, 'A')):
        d.ellipse(box(x - 16, y - 16, x + 16, y + 16), outline=255, width=line)
        l, t, r, b = d.textbbox((0, 0), c, font=bahn(22 * s, 'Bold Condensed'))
        d.text((int((cx + x * s) * SS) - (r - l) // 2 - l, int((cy + y * s) * SS) - (b - t) // 2 - t), c, font=bahn(22 * s, 'Bold Condensed'), fill=255)
    for x, c in ((-38, 'BACK'), (38, 'START')):
        d.rounded_rectangle(box(x - 22, -40, x + 22, -22), int(6 * s * SS), outline=255, width=int(2 * SS))
    d.ellipse(box(-20, -60, 20, -20), outline=255, width=line)


def controls_page(title, left, right):
    # The controller's, until the title's own diagram has been seen.
    im = Image.new('L', (1280 * SS, 720 * SS), 0)
    d = ImageDraw.Draw(im)
    page_title(d, title)
    text(d, (240, 170, 1040, 215), 'The diagram appears here after the first loading screen', 200, 0.6)
    y0 = 300
    label, value = bahn(21), bahn(21, 'Bold Condensed')
    rows = max(len(left), len(right))
    step = (668 - y0) / ((rows + 1) // 2)
    for x, items in ((70, left), (660, right)):
        half = (len(items) + 1) // 2
        for i, (name, key_name) in enumerate(items):
            column = 0 if i < half else 1
            yy = y0 + (i if i < half else i - half) * step
            xx = x + column * 290
            d.text((xx * SS, int(yy * SS)), name, font=label, fill=190)
            d.text((xx * SS, int((yy + 18) * SS)), key_name, font=value, fill=255)
    text(d, (340, 686, 940, 712), 'Left click, right click or ESC to go back', 170, 0.7)
    return im.resize((1280, 720), Image.LANCZOS)


def page_title(d, title):
    f = bahn(40, 'Bold Condensed')
    l, t, r, b = d.textbbox((0, 0), title, font=f)
    d.text((640 * SS - (r - l) // 2 - l, 34 * SS - t), title, font=f, fill=255)
    d.line((90 * SS, 88 * SS, 1190 * SS, 88 * SS), fill=255, width=2 * SS)


PAGE_SCALE = 1.28
PAGE_X = 640 - 362 * PAGE_SCALE / 2
PAGE_Y = 96


def keyboard_pages():
    # The keyboard page is two pictures: everything with no key lit, which is what the renderer draws first, and every key lit, from
    # which it draws those in use. Bindings and their names are drawn when shown, with page_font().
    global BOUND
    labels = None
    pictures = []
    for bound in (set(), None):
        BOUND = bound if bound is not None else set(VK)
        im = Image.new('L', (1280 * SS, 720 * SS), 0)
        d = ImageDraw.Draw(im)
        art = diagram().crop((0, 108, 362, 368)).resize((int(362 * PAGE_SCALE) * SS, int(260 * PAGE_SCALE) * SS), Image.LANCZOS)
        im.paste(255, (int(PAGE_X * SS), PAGE_Y * SS), art)
        if bound is not None:
            page_title(d, 'CONTROLS: KEYBOARD + MOUSE')
            text(d, (140, 686, 1140, 712), 'Click or press ENTER to change a control        T: back to the defaults        ESC or right click: leave', 170, 0.7)
        pictures.append(im.resize((1280, 720), Image.LANCZOS))
    BOUND = DEFAULT_BOUND
    boxes = []
    for label, box in KEY_BOXES.items():
        if label in VK:
            x0, y0, x1, y1 = box
            boxes.append((VK[label], PAGE_X + x0 * PAGE_SCALE, PAGE_Y + (y0 - 108) * PAGE_SCALE, PAGE_X + x1 * PAGE_SCALE, PAGE_Y + (y1 - 108) * PAGE_SCALE))
    diagram()  # KEY_BOXES as the default again; nothing depends on it.
    return pictures[0], pictures[1], boxes


FONT_FIRST, FONT_LAST = 32, 126


def page_font():
    # The two weights of the keyboard page's text, every printable character in its own cell: a picture, and for each its left, top,
    # width, height and how far the next one goes on, in sixteenths.
    w, h = 1024, 128
    im = Image.new('L', (w * SS, h * SS), 0)
    d = ImageDraw.Draw(im)
    cells = []
    x, y = 0, 0
    height = 28
    for f in (bahn(21), bahn(21, 'Bold Condensed')):
        for code in range(FONT_FIRST, FONT_LAST + 1):
            c = chr(code)
            advance = f.getlength(c) / SS
            width = int(advance) + 4
            if x + width > w:
                x, y = 0, y + height
            assert y + height <= h
            d.text(((x + 2) * SS, (y + 22) * SS), c, font=f, fill=255, anchor='ls')
            cells.append((x, y, width, height, int(advance * 16)))
            x += width
    return im.resize((w, h), Image.LANCZOS), cells


def encode(image):
    # Pairs of (run length - 1, value).
    data = image.tobytes()
    out = bytearray()
    i = 0
    while i < len(data):
        j = i
        while j + 1 < len(data) and data[j + 1] == data[i] and j - i < 255:
            j += 1
        out += bytes((j - i, data[i]))
        i = j + 1
    return out


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
    atlas, rects = overlay_atlas()
    plain, lit, key_boxes = keyboard_pages()
    font_picture, font_cells = page_font()
    pieces = [('g_mkbStunIcons', stun_icons(), 256), ('g_mkbIcons', icons(False), 256), ('g_mkbMenuIcons', icons(True), 256), ('g_mkbViewerIcons', icons(True, True), 256), ('g_mkbDiagram', diagram(), 512),
              ('g_overlayAtlas', atlas, 1024), ('g_controlsKeyboard', plain, 1280),
              ('g_controlsController', controls_page('CONTROLS: CONTROLLER', LEFT_PAD, RIGHT_PAD), 1280),
              ('g_controlsKeysLit', lit, 1280), ('g_pageFont', font_picture, 1024)]
    with open(os.path.join(root, 'ARRecomp', 'gpu', 'mkb_art.inl'), 'w', newline='\n') as f:
        f.write('// Generated by tools/mkb_art/make_mkb_art.py: alpha planes as pairs of (run length - 1, value).\n')
        for name, image, size in pieces:
            data = encode(image)
            f.write('static const uint8_t %s[] = {\n' % name)
            for k in range(0, len(data), 32):
                f.write(','.join(str(v) for v in data[k:k + 32]) + ',\n')
            f.write('};\n')
        f.write('static constexpr uint16_t g_overlayRects[][4] = {\n')
        for x, y, w, h in rects:
            f.write('{%d,%d,%d,%d},\n' % (x, y, w, h))
        f.write('};\n')
    # For the input code, which lays out the keyboard page.
    with open(os.path.join(root, 'ARRecomp', 'gpu', 'mkb_art_tables.inl'), 'w', newline='\n') as f:
        f.write('// Generated by tools/mkb_art/make_mkb_art.py: the keyboard page\'s keys (virtual key, left, top, right, bottom) and font (x, y, width, height, advance in sixteenths).\n')
        f.write('static constexpr int16_t g_pageKeys[][5] = {\n')
        for vk, x0, y0, x1, y1 in key_boxes:
            f.write('{%d,%d,%d,%d,%d},\n' % (vk, x0, y0, x1 + 0.999, y1 + 0.999))
        f.write('};\n')
        f.write('static constexpr int16_t g_pageFontCells[2][%d][5] = {\n' % (FONT_LAST - FONT_FIRST + 1))
        count = FONT_LAST - FONT_FIRST + 1
        for weight in range(2):
            f.write('{\n')
            for cell in font_cells[weight * count:(weight + 1) * count]:
                f.write('{%d,%d,%d,%d,%d},\n' % cell)
            f.write('},\n')
        f.write('};\n')
    if len(sys.argv) > 1:
        for name, image, size in pieces:
            bg = Image.new('RGB', image.size, (20, 45, 20))
            bg.paste((170, 255, 90), (0, 0), image)
            scale = 3 if size == 256 else 2 if size == 512 else 1
            bg.resize((image.width * scale, image.height * scale), Image.LANCZOS).save(os.path.join(sys.argv[1], name + '_view.png'))


main()
