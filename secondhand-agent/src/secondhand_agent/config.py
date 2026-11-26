import os
from dataclasses import dataclass
from pathlib import Path

from dotenv import load_dotenv

from .llm import DEFAULT_MODELS, PROVIDERS

PROMPTS_DIR = Path(__file__).resolve().parents[2] / "prompts"

_KEY_ENV = {"gemini": "GEMINI_API_KEY", "anthropic": "ANTHROPIC_API_KEY"}


@dataclass(frozen=True)
class Config:
    provider: str
    llm_key: str
    telegram_token: str
    owner_id: int
    model: str
    data_dir: Path


def load_config() -> Config:
    load_dotenv()
    provider = os.getenv("LLM_PROVIDER", "gemini").strip().lower()
    if provider not in PROVIDERS:
        raise SystemExit(f"LLM_PROVIDER לא תקין: {provider} (אפשרי: {', '.join(PROVIDERS)})")
    key_env = _KEY_ENV[provider]
    required = (key_env, "TELEGRAM_BOT_TOKEN", "OWNER_TELEGRAM_ID")
    missing = [k for k in required if not os.getenv(k)]
    if missing:
        raise SystemExit(f"חסרים משתני סביבה: {', '.join(missing)} (ראה .env.example)")
    data_dir = Path(os.getenv("DATA_DIR", "./data"))
    data_dir.mkdir(parents=True, exist_ok=True)
    return Config(
        provider=provider,
        llm_key=os.environ[key_env],
        telegram_token=os.environ["TELEGRAM_BOT_TOKEN"],
        owner_id=int(os.environ["OWNER_TELEGRAM_ID"]),
        model=os.getenv("LLM_MODEL") or DEFAULT_MODELS[provider],
        data_dir=data_dir,
    )


def prompt(name: str) -> str:
    return (PROMPTS_DIR / f"{name}.md").read_text(encoding="utf-8")
