#!/usr/bin/env python3
"""Regenerate the NAI PaintBrush icon assets from one square source PNG.

    python scripts/pb-make-icon.py "C:/path/to/emblem.png"

Writes:
  share/icons/nai-paintbrush.ico        (16,24,32,48,64,128,256; BMP entries
                                         for small sizes, PNG for 256)
  share/icons/256x256/nai-paintbrush.png
  share/icons/16x16/nai-paintbrush.png

The Windows resource compiler embeds the .ico into nai-paintbrush.exe
(bin/vlc_win32_rc.rc.in) and the Qt resource file embeds the 256/16 PNGs
(modules/gui/qt/assets.qrc -> :/logo/nai-paintbrush256.png / 16).

Source requirements: square RGBA PNG. A non-square image is padded to the
larger side with transparency rather than cropped, so the emblem is never cut.
"""
import os
import sys

from PIL import Image

ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SRC = os.path.join(HERE, "share", "icons", "nai-paintbrush.png")
OUT_ICON = os.path.join(HERE, "share", "icons", "nai-paintbrush.ico")
OUT_256 = os.path.join(HERE, "share", "icons", "256x256", "nai-paintbrush.png")
OUT_16 = os.path.join(HERE, "share", "icons", "16x16", "nai-paintbrush.png")
OUT_MSIX = os.path.join(HERE, "packaging", "msix", "Assets")


def main() -> int:
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SRC
    if not os.path.isfile(src):
        print("source not found: %s" % src, file=sys.stderr)
        return 2

    im = Image.open(src).convert("RGBA")
    w, h = im.size
    if w != h:
        side = max(w, h)
        canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        canvas.paste(im, ((side - w) // 2, (side - h) // 2))
        im = canvas
        print("padded %dx%d -> %dx%d" % (w, h, side, side))

    # store a master copy for future regenerations
    im.save(os.path.join(HERE, "share", "icons", "nai-paintbrush.png"))

    os.makedirs(os.path.dirname(OUT_256), exist_ok=True)
    os.makedirs(os.path.dirname(OUT_16), exist_ok=True)

    im.save(OUT_ICON, format="ICO", sizes=[(s, s) for s in ICO_SIZES])
    im.resize((256, 256), Image.LANCZOS).save(OUT_256)
    im.resize((16, 16), Image.LANCZOS).save(OUT_16)

    # MSIX visual assets (AppxManifest.xml -> Assets\...)
    os.makedirs(OUT_MSIX, exist_ok=True)
    msix = {
        "Square44x44Logo.png": (44, 44),
        "Square150x150Logo.png": (150, 150),
        "StoreLogo.png": (50, 50),
    }
    for name, size in msix.items():
        im.resize(size, Image.LANCZOS).save(os.path.join(OUT_MSIX, name))
    wide = Image.new("RGBA", (310, 150), (0, 0, 0, 0))
    wide.paste(im.resize((150, 150), Image.LANCZOS), (80, 0))
    wide.save(os.path.join(OUT_MSIX, "Wide310x150Logo.png"))

    for path in (OUT_ICON, OUT_256, OUT_16,
                 *[os.path.join(OUT_MSIX, name) for name in msix],
                 os.path.join(OUT_MSIX, "Wide310x150Logo.png")):
        print("%-60s %8d bytes" % (os.path.relpath(path, HERE), os.path.getsize(path)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
