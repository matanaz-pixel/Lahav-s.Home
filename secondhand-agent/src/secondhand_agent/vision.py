from pathlib import Path

from .config import prompt
from .llm import LLM


def detect_items(llm: LLM, images: list[Path]) -> list[dict]:
    result = llm.json_call(
        prompt("item_detection"),
        f"יש {len(images)} תמונות. החזר מערך JSON של פריטים.",
        images=images,
        max_tokens=3000,
    )
    return result if isinstance(result, list) else result.get("items", [])
