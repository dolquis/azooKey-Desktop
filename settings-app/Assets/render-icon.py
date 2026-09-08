"""Author the original Windows settings icon; no fonts or legacy assets are used.

Run from the repository root:
uv run --no-project --with pillow==12.1.1 python settings-app/Assets/render-icon.py
"""

from pathlib import Path

from PIL import Image, ImageDraw


def render_icon() -> Image.Image:
    scale = 4
    image = Image.new("RGBA", (256 * scale, 256 * scale))
    draw = ImageDraw.Draw(image)

    def box(bounds: tuple[int, int, int, int]) -> tuple[int, ...]:
        return tuple(value * scale for value in bounds)

    blue = "#2877DE"
    draw.rounded_rectangle(box((16, 12, 240, 244)), radius=48 * scale, fill="#16417C")
    draw.rounded_rectangle(box((16, 12, 240, 222)), radius=48 * scale, fill=blue)
    # A single-storey lowercase a, drawn as geometry to avoid font licensing.
    draw.ellipse(box((68, 68, 172, 176)), fill="white")
    draw.ellipse(box((92, 92, 148, 152)), fill=blue)
    draw.rounded_rectangle(box((150, 68, 176, 176)), radius=6 * scale, fill="white")
    return image.resize((256, 256), Image.Resampling.LANCZOS)


if __name__ == "__main__":
    render_icon().save(
        Path(__file__).with_name("azookey-settings.ico"),
        sizes=[(size, size) for size in (16, 20, 24, 32, 40, 48, 64, 128, 256)],
    )
