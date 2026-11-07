#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow>=11.0"]
# ///
"""Convert the face PNGs into LVGL images the firmware can draw from flash.

LVGL cannot decode PNG on this board: a decoder would cost flash and a full
480x480 temporary buffer costs more RAM than the C6 has. The images are
therefore pre-converted to LVGL 9's RGB565A8 layout, which LVGL can draw
straight from a const buffer, and cropped to the part that is actually visible
so the transparent margin never reaches the flash.
"""

from dataclasses import dataclass
from pathlib import Path

from PIL import Image

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
SOURCE_DIRECTORY = REPOSITORY_ROOT / "assets" / "faces"
BINARY_DIRECTORY = REPOSITORY_ROOT / "main" / "faces"
GENERATED_HEADER = REPOSITORY_ROOT / "main" / "faces_generated.h"
GENERATED_SOURCE = REPOSITORY_ROOT / "main" / "faces_generated.c"

# The faces the firmware uses. A face must exist here, in faces.h's enum and as
# a PNG of the same name; the generated table uses the enum constant derived
# from the name, so a missing or renamed one fails to compile.
FACE_NAMES = (
    "idle_default",
    "listening",
    "thinking",
    "speaking",
    "muted",
    "blink",
    "idle_happy_eyes",
    "idle_curious",
)

FRAME_SIZE = 480

# The glow around each face fades out very gradually, so the last few percent
# of alpha covers another 10 to 20 px per side of almost black pixels. Ignoring
# alpha below 8/255 (3% of full brightness) and keeping a small margin crops
# those invisible pixels without cutting into the glow that can be seen.
ALPHA_THRESHOLD = 8
GLOW_MARGIN = 6


@dataclass(frozen=True)
class FaceImage:
    """One converted face and where it belongs in the 480x480 frame."""

    name: str
    width: int
    height: int
    x: int
    y: int

    @property
    def data_size(self) -> int:
        # The RGB565 plane is two bytes per pixel and the alpha plane one.
        return self.width * self.height * 3


def crop_to_visible_pixels(image: Image.Image) -> tuple[Image.Image, int, int]:
    """Crop to the visible pixels plus a margin, and return the crop offset."""
    visible = image.getchannel("A").point(lambda value: 255 if value >= ALPHA_THRESHOLD else 0)
    bounding_box = visible.getbbox()
    if bounding_box is None:
        raise ValueError("the image is fully transparent, so there is nothing to draw")

    left, top, right, bottom = bounding_box
    # LVGL's RGB565A8 drawing code assumes the alpha plane's stride is half the
    # RGB565 plane's stride, which only holds when the width is even: the
    # alpha rows are then exactly as wide as the image. Rounding both crop
    # edges to even coordinates keeps every row aligned and the offsets even,
    # which also matches the display's even-coordinate flush rounding.
    left = max(0, left - GLOW_MARGIN) & ~1
    top = max(0, top - GLOW_MARGIN) & ~1
    right = min(FRAME_SIZE, right + GLOW_MARGIN)
    bottom = min(FRAME_SIZE, bottom + GLOW_MARGIN)
    right += (right - left) & 1
    bottom += (bottom - top) & 1

    return image.crop((left, top, right, bottom)), left, top


def rgb565_value(red: int, green: int, blue: int) -> int:
    """Pack an 8-bit colour into LVGL's native RGB565 bit layout."""
    return ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)


def encode_image(image: Image.Image) -> bytes:
    """Return the RGB565 plane followed by the A8 plane, as LVGL expects."""
    width, height = image.size
    raw_rgb = image.convert("RGB").tobytes()

    rgb565_plane = bytearray()
    for offset in range(0, len(raw_rgb), 3):
        value = rgb565_value(raw_rgb[offset], raw_rgb[offset + 1], raw_rgb[offset + 2])
        # LVGL stores lv_color_t in the CPU's little-endian order, and the
        # display port swaps bytes only when it flushes the draw buffer. The
        # image therefore has to stay in host order rather than pre-swapped.
        rgb565_plane.append(value & 0xFF)
        rgb565_plane.append(value >> 8)

    alpha_plane = image.getchannel("A").tobytes()
    encoded = bytes(rgb565_plane) + alpha_plane
    if len(encoded) != width * height * 3:
        raise AssertionError("encoded size does not match an even-width RGB565A8 image")
    return encoded


def decode_image(encoded: bytes, width: int, height: int) -> tuple[list[int], list[int]]:
    """Split an RGB565A8 buffer back into its two planes."""
    plane_size = width * height * 2
    rgb565_values = [
        encoded[offset] | (encoded[offset + 1] << 8) for offset in range(0, plane_size, 2)
    ]
    alpha_values = list(encoded[plane_size:])
    return rgb565_values, alpha_values


def verify_image(image: Image.Image, encoded: bytes) -> None:
    """Check that the buffer decodes back to the pixels it was made from.

    The layout is the part that a mistake here would only reveal on the board,
    where a swapped plane or a wrong stride looks like a corrupted face, so the
    generator checks its own output instead.
    """
    width, height = image.size
    raw_rgb = image.convert("RGB").tobytes()
    expected_rgb = [
        rgb565_value(raw_rgb[offset], raw_rgb[offset + 1], raw_rgb[offset + 2])
        for offset in range(0, len(raw_rgb), 3)
    ]
    expected_alpha = list(image.getchannel("A").tobytes())

    rgb565_values, alpha_values = decode_image(encoded, width, height)
    if rgb565_values != expected_rgb:
        raise AssertionError(f"{width}x{height} image: the RGB565 plane does not round-trip")
    if alpha_values != expected_alpha:
        raise AssertionError(f"{width}x{height} image: the alpha plane does not round-trip")


def enum_constant(name: str) -> str:
    """Map a file name to the face_id_t enumerator the firmware uses."""
    return f"FACE_{name.upper()}"


def descriptor_name(name: str) -> str:
    return f"face_{name}_image"


def generate_header() -> str:
    return """\
// Generated by assets/faces/generate_faces.py from assets/faces/*.png. Do not
// edit; run ./assets/faces/generate_faces.py to regenerate.
#pragma once

#include "faces.h"

// The images live in flash and are const, so the module hands out pointers
// into this table rather than copies.
extern const face_asset_t face_generated_assets[FACE_COUNT];
"""


def generate_source(faces: list[FaceImage]) -> str:
    lines = [
        "// Generated by assets/faces/generate_faces.py from assets/faces/*.png. Do not",
        "// edit; run ./assets/faces/generate_faces.py to regenerate.",
        '#include "faces_generated.h"',
        "",
        "// The linker creates these symbols from the .bin files in EMBED_FILES in",
        "// main/CMakeLists.txt, naming them after the file.",
    ]
    for face in faces:
        lines.append(f"extern const uint8_t {face.name}_bin[];")

    for face in faces:
        lines.extend(
            [
                "",
                f"// {face.name}.bin is {face.width}x{face.height} at ({face.x}, {face.y}) in the",
                f"// 480x480 frame: {face.data_size} bytes of RGB565 followed by A8.",
                f"static const lv_image_dsc_t {descriptor_name(face.name)} = {{",
                "    .header = {",
                "        .magic = LV_IMAGE_HEADER_MAGIC,",
                "        .cf = LV_COLOR_FORMAT_RGB565A8,",
                "        .flags = 0,",
                f"        .w = {face.width},",
                f"        .h = {face.height},",
                f"        .stride = {face.width * 2},",
                "        .reserved_2 = 0,",
                "    },",
                f"    .data_size = {face.data_size},",
                f"    .data = {face.name}_bin,",
                "};",
            ]
        )

    lines.extend(["", "const face_asset_t face_generated_assets[FACE_COUNT] = {"])
    for face in faces:
        lines.append(
            f"    [{enum_constant(face.name)}] = {{ .image = &{descriptor_name(face.name)}, "
            f".x = {face.x}, .y = {face.y} }},"
        )
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def convert_face(name: str) -> FaceImage:
    source_path = SOURCE_DIRECTORY / f"{name}.png"
    image = Image.open(source_path).convert("RGBA")
    if image.size != (FRAME_SIZE, FRAME_SIZE):
        raise ValueError(f"{source_path.name} is {image.size}, expected {FRAME_SIZE}x{FRAME_SIZE}")

    cropped, x, y = crop_to_visible_pixels(image)
    encoded = encode_image(cropped)
    verify_image(cropped, encoded)

    BINARY_DIRECTORY.mkdir(parents=True, exist_ok=True)
    (BINARY_DIRECTORY / f"{name}.bin").write_bytes(encoded)

    width, height = cropped.size
    return FaceImage(name=name, width=width, height=height, x=x, y=y)


def main() -> None:
    faces = [convert_face(name) for name in FACE_NAMES]
    GENERATED_HEADER.write_text(generate_header())
    GENERATED_SOURCE.write_text(generate_source(faces))

    total_bytes = 0
    print(f"{'face':<18} {'crop':>11} {'offset':>11} {'bytes':>8}")
    for face in faces:
        total_bytes += face.data_size
        print(
            f"{face.name:<18} {face.width:>5}x{face.height:<5} "
            f"{face.x:>5},{face.y:<5} {face.data_size:>8}"
        )
    print(f"{'total':<18} {'':>11} {'':>11} {total_bytes:>8}")


if __name__ == "__main__":
    main()
