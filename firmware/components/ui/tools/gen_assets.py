#!/usr/bin/env python3
"""Generates the UI's own graphics as C sources (no external artwork):

  assets/ui_icons.c / ui_icons.h   anti-aliased A8 icons (recoloured at runtime)
  assets/ui_vinyl.c                180x180 RGB565 record, placeholder for a missing cover

Icons are drawn 8x oversized with Pillow and box-filtered down. Requires
Python 3 with Pillow and numpy:

  python3 firmware/components/ui/tools/gen_assets.py
"""
import math
import os

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "assets")
SS = 8  # supersampling factor


class Pen:
    """Drawing helper with coordinates normalised to 0..1."""

    def __init__(self, size):
        self.size = size
        self.n = size * SS
        self.img = Image.new("L", (self.n, self.n), 0)
        self.d = ImageDraw.Draw(self.img)

    def p(self, v):
        return v * self.n

    def pt(self, x, y):
        return (self.p(x), self.p(y))

    def circle(self, cx, cy, r, fill=255):
        self.d.ellipse([self.p(cx - r), self.p(cy - r), self.p(cx + r), self.p(cy + r)], fill=fill)

    def ring(self, cx, cy, r, w, fill=255):
        self.d.ellipse([self.p(cx - r), self.p(cy - r), self.p(cx + r), self.p(cy + r)],
                       outline=fill, width=max(1, int(round(self.p(w)))))

    def line(self, pts, w, fill=255, caps=True):
        xy = [self.pt(x, y) for x, y in pts]
        self.d.line(xy, fill=fill, width=int(round(self.p(w))), joint="curve")
        if caps:
            for x, y in (pts[0], pts[-1]):
                self.circle(x, y, w / 2, fill)

    def poly_round(self, pts, r, fill=255):
        """Filled polygon with rounded corners (outline stroked with round joins)."""
        xy = [self.pt(x, y) for x, y in pts]
        self.d.polygon(xy, fill=fill)
        self.d.line(xy + [xy[0]], fill=fill, width=int(round(self.p(2 * r))), joint="curve")
        for x, y in pts:
            self.circle(x, y, r, fill)

    def rrect(self, x0, y0, x1, y1, r, fill=255, outline_w=None):
        box = [self.p(x0), self.p(y0), self.p(x1), self.p(y1)]
        if outline_w is None:
            self.d.rounded_rectangle(box, radius=self.p(r), fill=fill)
        else:
            self.d.rounded_rectangle(box, radius=self.p(r), outline=fill, width=int(round(self.p(outline_w))))

    def arc(self, cx, cy, r, a0, a1, w, fill=255):
        """Arc with round caps; angles in degrees, 0 = 3 o'clock, clockwise."""
        box = [self.p(cx - r), self.p(cy - r), self.p(cx + r), self.p(cy + r)]
        self.d.arc(box, a0, a1, fill=fill, width=int(round(self.p(w))))
        rm = r - w / 2
        for a in (a0, a1):
            ar = math.radians(a)
            self.circle(cx + rm * math.cos(ar), cy + rm * math.sin(ar), w / 2, fill)

    def clear_line(self, pts, w):
        self.line(pts, w, fill=0)

    def result(self):
        small = self.img.reduce(SS)
        return np.asarray(small, dtype=np.uint8)


# ---------------------------------------------------------------- icons

def icon_play(s):
    p = Pen(s)
    p.poly_round([(0.34, 0.22), (0.34, 0.78), (0.80, 0.50)], 0.05)
    return p.result()


def icon_pause(s):
    p = Pen(s)
    p.rrect(0.26, 0.20, 0.42, 0.80, 0.05)
    p.rrect(0.58, 0.20, 0.74, 0.80, 0.05)
    return p.result()


def icon_next(s):
    p = Pen(s)
    p.poly_round([(0.22, 0.24), (0.22, 0.76), (0.62, 0.50)], 0.045)
    p.rrect(0.68, 0.22, 0.78, 0.78, 0.05)
    return p.result()


def icon_prev(s):
    return np.ascontiguousarray(icon_next(s)[:, ::-1])


def icon_wifi(s, off=False):
    p = Pen(s)
    cx, cy = 0.5, 0.80
    w = 0.10
    for r in (0.30, 0.52, 0.74):
        p.arc(cx, cy, r, 225, 315, w)
    p.circle(cx, cy - 0.03, 0.075)
    if off:
        p.clear_line([(0.14, 0.12), (0.88, 0.86)], 0.20)
        p.line([(0.16, 0.14), (0.86, 0.84)], 0.085)
    return p.result()


def icon_nfc(s):
    """Contactless symbol: four arcs opening to the right."""
    p = Pen(s)
    cx, cy = 0.10, 0.50
    w = 0.085
    for r in (0.24, 0.42, 0.60, 0.78):
        p.arc(cx, cy, r, -38, 38, w)
    return p.result()


def icon_card(s):
    """NFC tag / card with contactless waves."""
    p = Pen(s)
    p.rrect(0.10, 0.20, 0.90, 0.80, 0.12, outline_w=0.065)
    cx, cy = 0.30, 0.50
    for r in (0.14, 0.26, 0.38):
        p.arc(cx, cy, r, -40, 40, 0.06)
    return p.result()


def icon_shuffle(s):
    p = Pen(s)
    w = 0.08
    p.line([(0.12, 0.72), (0.30, 0.72), (0.60, 0.30), (0.80, 0.30)], w)
    p.clear_line([(0.40, 0.44), (0.50, 0.58)], 0.20)
    p.line([(0.12, 0.30), (0.30, 0.30), (0.42, 0.47)], w)
    p.line([(0.48, 0.55), (0.60, 0.72), (0.80, 0.72)], w)
    for y in (0.30, 0.72):
        p.line([(0.70, y - 0.12), (0.84, y), (0.70, y + 0.12)], w)
    return p.result()


def icon_info(s):
    p = Pen(s)
    p.ring(0.5, 0.5, 0.44, 0.07)
    p.circle(0.5, 0.30, 0.065)
    p.rrect(0.455, 0.42, 0.545, 0.74, 0.045)
    return p.result()


def icon_sun(s, rays=0.13, core=0.19):
    p = Pen(s)
    p.circle(0.5, 0.5, core)
    for i in range(8):
        a = math.radians(i * 45)
        r0, r1 = core + 0.11, core + 0.11 + rays
        p.line([(0.5 + r0 * math.cos(a), 0.5 + r0 * math.sin(a)),
                (0.5 + r1 * math.cos(a), 0.5 + r1 * math.sin(a))], 0.085)
    return p.result()


def icon_close(s):
    p = Pen(s)
    p.line([(0.24, 0.24), (0.76, 0.76)], 0.10)
    p.line([(0.76, 0.24), (0.24, 0.76)], 0.10)
    return p.result()


def icon_back(s):
    p = Pen(s)
    p.line([(0.62, 0.18), (0.32, 0.50), (0.62, 0.82)], 0.12)
    return p.result()


def icon_check(s):
    p = Pen(s)
    p.line([(0.22, 0.52), (0.42, 0.72), (0.78, 0.32)], 0.09)
    return p.result()


def icon_warn(s):
    p = Pen(s)
    pts = [(0.5, 0.14), (0.88, 0.82), (0.12, 0.82)]
    p.line(pts + [pts[0]], 0.075)
    p.rrect(0.462, 0.38, 0.538, 0.62, 0.035)
    p.circle(0.5, 0.71, 0.045)
    return p.result()


def icon_update(s):
    p = Pen(s)
    p.line([(0.5, 0.14), (0.5, 0.60)], 0.085)
    p.line([(0.30, 0.42), (0.5, 0.62), (0.70, 0.42)], 0.085)
    p.line([(0.16, 0.64), (0.16, 0.84), (0.84, 0.84), (0.84, 0.64)], 0.085)
    return p.result()


def icon_phone(s):
    p = Pen(s)
    p.rrect(0.28, 0.10, 0.72, 0.90, 0.10, outline_w=0.065)
    p.rrect(0.42, 0.76, 0.58, 0.81, 0.025)
    return p.result()


def icon_gear(s):
    """Settings cog: 8 rounded teeth, ring, open centre."""
    p = Pen(s)
    cx = cy = 0.5
    for i in range(8):
        a = math.radians(i * 45 + 22.5)
        ca, sa = math.cos(a), math.sin(a)
        # tooth: short thick stroke pointing outwards
        p.line([(cx + 0.30 * ca, cy + 0.30 * sa), (cx + 0.40 * ca, cy + 0.40 * sa)], 0.15)
    p.circle(cx, cy, 0.33)
    p.circle(cx, cy, 0.13, fill=0)
    return p.result()


ICONS = [
    ("play_32", icon_play, 32), ("play_48", icon_play, 48),
    ("pause_32", icon_pause, 32), ("pause_48", icon_pause, 48),
    ("next_32", icon_next, 32), ("prev_32", icon_prev, 32),
    ("next_48", icon_next, 48), ("prev_48", icon_prev, 48),
    ("wifi_20", icon_wifi, 20), ("wifi_off_20", lambda s: icon_wifi(s, True), 20),
    ("wifi_36", icon_wifi, 36),
    ("nfc_20", icon_nfc, 20), ("nfc_56", icon_nfc, 56),
    ("card_36", icon_card, 36), ("card_56", icon_card, 56),
    ("shuffle_36", icon_shuffle, 36), ("info_36", icon_info, 36),
    ("sun_20", lambda s: icon_sun(s, 0.08, 0.15), 20), ("sun_28", icon_sun, 28),
    ("close_28", icon_close, 28), ("back_24", icon_back, 24),
    ("check_56", icon_check, 56), ("warn_56", icon_warn, 56),
    ("update_56", icon_update, 56), ("phone_24", icon_phone, 24),
    ("gear_28", icon_gear, 28), ("shuffle_20", icon_shuffle, 20),
]


# ---------------------------------------------------------------- vinyl

def vinyl(size=180):
    ss = 4
    n = size * ss
    c = (n - 1) / 2.0
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    dx, dy = (x - c) / ss, (y - c) / ss
    r = np.hypot(dx, dy)
    th = np.arctan2(dy, dx)

    R_OUT, R_LABEL, R_HOLE = size / 2 - 1.5, 30.0, 3.2
    rgb = np.zeros((n, n, 3))

    # disc body: deep charcoal with fine grooves and a few track gaps
    body = np.full_like(r, 0.060)
    grooves = 0.010 * np.sin(2 * math.pi * r / 2.2)
    body += np.where((r > 36) & (r < R_OUT - 3), grooves, 0)
    for gap in (47.0, 58.5, 70.0, 79.0):
        body -= 0.022 * np.exp(-((r - gap) / 0.7) ** 2)
    body += np.where((r > 33) & (r < 36), -0.02, 0)          # glossy run-out
    # anisotropic "bow tie" reflection, warm tint
    ang = np.angle(np.exp(2j * (th - math.radians(-35)))) / 2
    sheen = np.exp(-(ang / 0.20) ** 2) * (0.20 * np.clip((r - 33) / 50, 0, 1))
    ang2 = np.angle(np.exp(2j * (th - math.radians(55)))) / 2
    sheen += np.exp(-(ang2 / 0.10) ** 2) * (0.05 * np.clip((r - 33) / 50, 0, 1))
    warm = np.array([1.0, 0.86, 0.62])
    disc = body[..., None] * np.array([1.0, 0.98, 0.95]) + sheen[..., None] * warm
    rim = np.exp(-((r - (R_OUT - 0.8)) / 0.8) ** 2) * 0.10
    disc += rim[..., None]

    # brass label
    brass = np.array([0xC9, 0xA4, 0x5C]) / 255.0
    shade = 0.80 + 0.28 * (1 - r / R_LABEL) + 0.10 * np.cos(th - math.radians(-120))
    label = brass * shade[..., None]
    label *= (1 - 0.30 * np.exp(-((r - (R_LABEL - 1.0)) / 0.9) ** 2))[..., None]
    label *= (1 - 0.18 * np.exp(-((r - 19.0) / 0.5) ** 2))[..., None]
    label *= (1 - 0.35 * np.exp(-((r - (R_HOLE + 1.2)) / 0.8) ** 2))[..., None]

    rgb = np.where((r <= R_LABEL)[..., None], label, disc)
    rgb = np.where((r <= R_HOLE)[..., None], 0.0, rgb)
    rgb = np.where((r > R_OUT)[..., None], 0.0, rgb)
    rgb = np.clip(rgb, 0, 1)
    small = rgb.reshape(size, ss, size, ss, 3).mean(axis=(1, 3))
    return small


def rgb565(img):
    q = np.clip(np.round(img * 255), 0, 255).astype(np.uint16)
    r, g, b = q[..., 0], q[..., 1], q[..., 2]
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


# ---------------------------------------------------------------- C output

HEADER = "/* Generated by tools/gen_assets.py - do not edit. */\n"


def c_bytes(data, indent="    ", per_line=16):
    lines = []
    for i in range(0, len(data), per_line):
        lines.append(indent + ", ".join("0x%02x" % b for b in data[i:i + per_line]) + ",")
    return "\n".join(lines)


def main():
    os.makedirs(OUT, exist_ok=True)
    src = [HEADER, '#include "ui_icons.h"\n']
    hdr = [HEADER, "#pragma once\n", '#include "lvgl.h"\n']
    total = 0
    for name, fn, size in ICONS:
        a = fn(size)
        assert a.shape == (size, size)
        data = a.astype(np.uint8).tobytes()
        total += len(data)
        src.append("static const uint8_t ui_icon_%s_map[] = {\n%s\n};\n" % (name, c_bytes(data)))
        src.append(
            "const lv_image_dsc_t ui_icon_%s = {\n"
            "    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .flags = 0,\n"
            "               .w = %d, .h = %d, .stride = %d},\n"
            "    .data_size = sizeof(ui_icon_%s_map),\n"
            "    .data = ui_icon_%s_map,\n};\n" % (name, size, size, size, name, name))
        hdr.append("extern const lv_image_dsc_t ui_icon_%s;\n" % name)
        Image.fromarray(a).save(os.path.join(HERE, "..", "simulator", "icons_preview_%s.png" % name)) \
            if os.environ.get("UI_ICON_PREVIEW") else None

    v = rgb565(vinyl(180)).astype("<u2").tobytes()
    hdr.append("\n/* 180 x 180 RGB565 record (placeholder cover), black outside the disc */\n")
    hdr.append("extern const lv_image_dsc_t ui_img_vinyl_180;\n")
    vsrc = [HEADER, '#include "ui_icons.h"\n',
            "static const uint8_t ui_img_vinyl_180_map[] = {\n%s\n};\n" % c_bytes(v),
            "const lv_image_dsc_t ui_img_vinyl_180 = {\n"
            "    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .flags = 0,\n"
            "               .w = 180, .h = 180, .stride = 360},\n"
            "    .data_size = sizeof(ui_img_vinyl_180_map),\n"
            "    .data = ui_img_vinyl_180_map,\n};\n"]

    with open(os.path.join(OUT, "ui_icons.c"), "w") as f:
        f.write("\n".join(src))
    with open(os.path.join(OUT, "ui_icons.h"), "w") as f:
        f.write("".join(hdr))
    with open(os.path.join(OUT, "ui_vinyl.c"), "w") as f:
        f.write("\n".join(vsrc))
    print("icons: %d bytes, vinyl: %d bytes" % (total, len(v)))


if __name__ == "__main__":
    main()
