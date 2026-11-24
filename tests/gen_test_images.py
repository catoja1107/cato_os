#!/usr/bin/env python3
"""Writes test images and their expected pixels for tests/unit/test_image.c.

Usage: gen_test_images.py <output-directory>

Each case NAME.EXT has NAME.rgb beside it: width and height as little-endian
32-bit values, then RGB bytes with transparency blended onto the browser's page
color, exactly as the decoders do.
"""
import pathlib
import struct
import sys

from PIL import Image

BACKGROUND = (30, 30, 37)
WIDTH, HEIGHT = 97, 61  # Odd sizes exercise partial blocks and row padding.


def blend(red, green, blue, alpha):
    inverse = 255 - alpha
    return tuple((c * alpha + b * inverse + 127) // 255
                 for c, b in zip((red, green, blue), BACKGROUND))


def write_expected(path, width, height, pixels):
    data = bytearray(struct.pack("<II", width, height))
    for pixel in pixels:
        data += bytes(pixel)
    path.write_bytes(bytes(data))


def rgba_pixels(image):
    rgba = image.convert("RGBA")
    return [blend(*pixel) for pixel in rgba.getdata()]


def gradient():
    image = Image.new("RGB", (WIDTH, HEIGHT))
    image.putdata([((x * 255) // (WIDTH - 1), (y * 255) // (HEIGHT - 1), ((x ^ y) * 7) & 255)
                   for y in range(HEIGHT) for x in range(WIDTH)])
    return image


def main():
    out = pathlib.Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    base = gradient()

    # PNG in each color type.
    base.save(out / "rgb8.png")
    write_expected(out / "rgb8.rgb", WIDTH, HEIGHT, rgba_pixels(base))

    rgba = base.copy()
    rgba.putalpha(Image.linear_gradient("L").resize((WIDTH, HEIGHT)))
    rgba.save(out / "rgba8.png")
    write_expected(out / "rgba8.rgb", WIDTH, HEIGHT, rgba_pixels(rgba))

    gray = base.convert("L")
    gray.save(out / "gray8.png")
    write_expected(out / "gray8.rgb", WIDTH, HEIGHT, rgba_pixels(gray))

    gray_alpha = Image.merge("LA", (gray, Image.linear_gradient("L").resize((WIDTH, HEIGHT))))
    gray_alpha.save(out / "grayalpha8.png")
    write_expected(out / "grayalpha8.rgb", WIDTH, HEIGHT, rgba_pixels(gray_alpha))

    values = [((x * 65535) // (WIDTH - 1)) for y in range(HEIGHT) for x in range(WIDTH)]
    gray16 = Image.new("I;16", (WIDTH, HEIGHT))
    gray16.putdata(values)
    gray16.save(out / "gray16.png")
    write_expected(out / "gray16.rgb", WIDTH, HEIGHT, [(v >> 8,) * 3 for v in values])

    bits = base.convert("1")
    bits.save(out / "gray1.png")
    write_expected(out / "gray1.rgb", WIDTH, HEIGHT, rgba_pixels(bits))

    palette = base.quantize(colors=16)
    palette.save(out / "palette4.png", bits=4, transparency=3)
    reloaded = Image.open(out / "palette4.png")
    write_expected(out / "palette4.rgb", WIDTH, HEIGHT, rgba_pixels(reloaded))

    palette256 = base.quantize(colors=200)
    palette256.save(out / "palette8.png")
    write_expected(out / "palette8.rgb", WIDTH, HEIGHT, rgba_pixels(Image.open(out / "palette8.png")))

    # JPEG: compared against Pillow's decoder with a tolerance.
    base.save(out / "baseline444.jpg", quality=95, subsampling=0)
    write_expected(out / "baseline444.rgb", WIDTH, HEIGHT,
                   rgba_pixels(Image.open(out / "baseline444.jpg")))
    # Chroma subsampling is checked on a smooth image. Decoders upsample chroma
    # differently, so sharp color detail would measure that choice, not correctness.
    smooth = Image.new("RGB", (WIDTH, HEIGHT))
    smooth.putdata([((x * 255) // (WIDTH - 1), (y * 255) // (HEIGHT - 1),
                     ((x + y) * 255) // (WIDTH + HEIGHT - 2))
                    for y in range(HEIGHT) for x in range(WIDTH)])
    smooth.save(out / "baseline420.jpg", quality=90, subsampling=2)
    write_expected(out / "baseline420.rgb", WIDTH, HEIGHT,
                   rgba_pixels(Image.open(out / "baseline420.jpg")))
    gray.save(out / "gray.jpg", quality=92)
    write_expected(out / "gray.rgb", WIDTH, HEIGHT, rgba_pixels(Image.open(out / "gray.jpg")))
    base.save(out / "progressive.jpg", quality=90, progressive=True)

    # GIF, plain and interlaced, with a transparent color.
    gif = base.quantize(colors=64)
    gif.save(out / "plain.gif", interlace=False, transparency=5)
    write_expected(out / "plain.rgb", WIDTH, HEIGHT, rgba_pixels(Image.open(out / "plain.gif")))
    gif.save(out / "interlaced.gif", interlace=True)
    write_expected(out / "interlaced.rgb", WIDTH, HEIGHT,
                   rgba_pixels(Image.open(out / "interlaced.gif")))

    (out / "not-an-image.webp").write_bytes(b"RIFF\x10\x00\x00\x00WEBPVP8 ")
    return 0


if __name__ == "__main__":
    sys.exit(main())
