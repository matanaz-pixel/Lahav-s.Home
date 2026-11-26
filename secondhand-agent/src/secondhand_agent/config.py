import os
from dataclasses import dataclass
from pathlib import Path

from dotenv import load_dotenv

PROMPTS_DIR = Path(__file__).resolve().parents[2] / "prompts"


@dataclass(frozen=True)
class Config:
    anthropic_key: str
    telegram_token: str
    owner_id: int
    model: str
    data_dir: Path


def load_config() -> Config:
    load_dotenv()
    missing = [k for k in ("ANTHROPIC_API_KEY", "TELEGRAM_BOT_TOKEN", "OWNER_TELEGRAM_ID") if not os.getenv(k)]
    if missing:
        raise SystemExit(f"חסרים משתני סביבה: {', '.join(missing)} (ראה .env.example)")
    data_dir = Path(os.getenv("DATA_DIR", "./data"))
    data_dir.mkdir(parents=True, exist_ok=True)
    return Config(
        anthropic_key=os.environ["ANTHROPIC_API_KEY"],
        telegram_token=os.environ["TELEGRAM_BOT_TOKEN"],
        owner_id=int(os.environ["OWNER_TELEGRAM_ID"]),
        model=os.getenv("CLAUDE_MODEL", "claude-sonnet-5-5"),
        data_dir=data_dir,
    )


def prompt(name: str) -> str:
    return (PROMPTS_DIR / f"{name}.md").read_text(encoding="utf-8")
