"""Generate 32x32 1-bpp Hangul bitmaps from Windows Malgun Gothic."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

FONT = Path(r"C:\Windows\Fonts\malgun.ttf")
OUTPUT = Path(__file__).parents[1] / "components/ycb_lcd/src/font/malgun_gothic_32.h"
FIRST, LAST, SIZE = 0xAC00, 0xD7A3, 32

def glyph(font, ch):
    image = Image.new("L", (SIZE, SIZE), 0)
    draw = ImageDraw.Draw(image)
    left, top, right, bottom = draw.textbbox((0, 0), ch, font=font)
    draw.text(((SIZE-(right-left))//2-left, (SIZE-(bottom-top))//2-top), ch, font=font, fill=255)
    result = []
    for y in range(SIZE):
        for block in range(4):
            value = 0
            for bit in range(8):
                if image.getpixel((block*8+bit, y)) >= 112:
                    value |= 0x80 >> bit
            result.append(value)
    return result

def main():
    font = ImageFont.truetype(str(FONT), 30)
    with OUTPUT.open("w", encoding="ascii", newline="\n") as out:
        out.write("#pragma once\n#include <stdint.h>\n")
        out.write("/* Generated from Malgun Gothic by tools/generate_malgun32_font.py */\n")
        out.write("#define MALGUN32_FIRST 0xAC00u\n#define MALGUN32_LAST 0xD7A3u\n")
        out.write("static const uint8_t malgun32_font[11172][128] = {\n")
        for cp in range(FIRST, LAST+1):
            values = ",".join(f"0x{x:02X}" for x in glyph(font, chr(cp)))
            out.write(f"{{{values}}},/*U+{cp:04X}*/\n")
        out.write("};\n")
    print(f"generated {LAST-FIRST+1} glyphs into {OUTPUT}")

if __name__ == "__main__":
    main()
