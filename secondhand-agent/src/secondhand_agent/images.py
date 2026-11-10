"""שיפור תמונות שמרני: כיוון, חיתוך לשוליים, תאורה וחדות. לא יוצר תוכן חדש."""
from pathlib import Path

from PIL import Image, ImageEnhance, ImageOps


def enhance(src: Path, dst: Path, max_side: int = 1600) -> Path:
    img = ImageOps.exif_transpose(Image.open(src)).convert("RGB")
    img = ImageOps.autocontrast(img, cutoff=1)
    img = ImageEnhance.Brightness(img).enhance(1.05)
    img = ImageEnhance.Color(img).enhance(1.08)
    img = ImageEnhance.Sharpness(img).enhance(1.25)
    img.thumbnail((max_side, max_side))
    dst.parent.mkdir(parents=True, exist_ok=True)
    img.save(dst, "JPEG", quality=88)
    return dst
