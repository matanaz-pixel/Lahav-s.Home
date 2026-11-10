import json

from .config import prompt
from .llm import LLM


def write_listing(llm: LLM, item: dict, price: int, notes: str = "") -> dict:
    payload = {"item": item, "price": price, "owner_notes": notes}
    return llm.json_call(prompt("listing_writer"), json.dumps(payload, ensure_ascii=False))
