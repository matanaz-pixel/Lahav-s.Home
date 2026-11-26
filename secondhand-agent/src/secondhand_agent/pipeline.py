from pathlib import Path

from . import images, pricing, listing, vision
from .db import DB
from .llm import LLM


def process_photos(llm: LLM, db: DB, photos: list[Path], out_dir: Path, notes: str = "") -> list[int]:
    """תמונות → פריטים → תמונות משופרות → מחיר → מודעה → נשמר ממתין לאישור."""
    detected = vision.detect_items(llm, photos)
    ids = []
    for n, item in enumerate(detected):
        idxs = [i for i in item.get("image_indexes", []) if 0 <= i < len(photos)]
        enhanced = [str(images.enhance(photos[i], out_dir / f"item{n}_{i}.jpg")) for i in idxs]
        price_info = pricing.suggest_price(llm, item)
        price = int(price_info["recommended_price"])
        ad = listing.write_listing(llm, item, price, notes)
        data = {"item": item, "images": enhanced, "pricing": price_info, "listing": ad}
        ids.append(db.add_item(data, price, int(price_info["floor_price"])))
    return ids
