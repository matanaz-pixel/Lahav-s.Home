import json

from .config import prompt
from .llm import LLM


def suggest_price(llm: LLM, item: dict, comparables: list[dict] | None = None, owner_floor: int | None = None) -> dict:
    payload = {"item": item, "comparables": comparables or [], "owner_floor": owner_floor}
    # TODO שלב 2: למלא comparables מחיפוש אמיתי ביד2/פייסבוק (Playwright)
    return llm.json_call(prompt("pricing"), json.dumps(payload, ensure_ascii=False))
