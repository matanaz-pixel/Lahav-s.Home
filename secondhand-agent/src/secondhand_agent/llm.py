"""שכבת ספק LLM מופשטת.

כל שאר הקוד מדבר רק עם `LLM.json_call`. הספק נבחר בהגדרה (`LLM_PROVIDER`),
כך שאפשר לעבור בין Gemini (שכבה חינמית) ל-Claude בלי לגעת בקוד.
"""
import base64
import json
import re
from abc import ABC, abstractmethod
from pathlib import Path

PROVIDERS = ("gemini", "anthropic")
DEFAULT_MODELS = {
    "gemini": "gemini-2.5-flash",
    "anthropic": "claude-sonnet-5-5",
}


def _extract_json(text: str):
    m = re.search(r"```(?:json)?\s*(.*?)```", text, re.S)
    if m:
        text = m.group(1)
    start = min((i for i in (text.find("{"), text.find("[")) if i != -1), default=-1)
    if start == -1:
        raise ValueError("לא נמצא JSON בתשובת המודל")
    return json.loads(text[start:])


class LLM(ABC):
    """ממשק משותף לכל הספקים."""

    @abstractmethod
    def json_call(self, system: str, user_text: str, images: list[Path] | None = None, max_tokens: int = 2000):
        """שולח פרומפט (עם תמונות אופציונליות) ומחזיר JSON מפוענח."""


class GeminiLLM(LLM):
    def __init__(self, api_key: str, model: str):
        from google import genai

        self.client = genai.Client(api_key=api_key)
        self.model = model

    def json_call(self, system, user_text, images=None, max_tokens=2000):
        from google.genai import types

        parts = []
        for i, path in enumerate(images or []):
            parts.append(f"תמונה {i}:")
            parts.append(types.Part.from_bytes(data=path.read_bytes(), mime_type="image/jpeg"))
        parts.append(user_text)
        resp = self.client.models.generate_content(
            model=self.model,
            contents=parts,
            config=types.GenerateContentConfig(
                system_instruction=system,
                response_mime_type="application/json",
                # מודלי Gemini 2.5 "חושבים" וצורכים טוקנים מהתקציב; משאירים מרווח לתשובה עצמה
                max_output_tokens=max_tokens * 4,
            ),
        )
        return _extract_json(resp.text or "")


class AnthropicLLM(LLM):
    def __init__(self, api_key: str, model: str):
        import anthropic

        self.client = anthropic.Anthropic(api_key=api_key)
        self.model = model

    def json_call(self, system, user_text, images=None, max_tokens=2000):
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


def create_llm(provider: str, api_key: str, model: str) -> LLM:
    if provider == "gemini":
        return GeminiLLM(api_key, model)
    if provider == "anthropic":
        return AnthropicLLM(api_key, model)
    raise ValueError(f"ספק לא מוכר: {provider} (אפשרי: {', '.join(PROVIDERS)})")
