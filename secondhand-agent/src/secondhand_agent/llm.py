import base64
import json
import re
from pathlib import Path

import anthropic


def _extract_json(text: str):
    m = re.search(r"```(?:json)?\s*(.*?)```", text, re.S)
    if m:
        text = m.group(1)
    start = min((i for i in (text.find("{"), text.find("[")) if i != -1), default=-1)
    if start == -1:
        raise ValueError("לא נמצא JSON בתשובת המודל")
    return json.loads(text[start:])


class LLM:
    def __init__(self, api_key: str, model: str):
        self.client = anthropic.Anthropic(api_key=api_key)
        self.model = model

    def json_call(self, system: str, user_text: str, images: list[Path] | None = None, max_tokens: int = 2000):
        content = []
        for i, path in enumerate(images or []):
            data = base64.standard_b64encode(path.read_bytes()).decode()
            content.append({"type": "text", "text": f"תמונה {i}:"})
            content.append({"type": "image", "source": {"type": "base64", "media_type": "image/jpeg", "data": data}})
        content.append({"type": "text", "text": user_text})
        resp = self.client.messages.create(
            model=self.model,
            max_tokens=max_tokens,
            system=system,
            messages=[{"role": "user", "content": content}],
        )
        return _extract_json("".join(b.text for b in resp.content if b.type == "text"))
