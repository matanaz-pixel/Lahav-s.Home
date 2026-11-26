from PIL import Image

from secondhand_agent.db import DB
from secondhand_agent.images import enhance
import pytest

from secondhand_agent.config import load_config
from secondhand_agent.llm import GeminiLLM, _extract_json, create_llm


def test_extract_json_from_fenced():
    assert _extract_json('הנה:\n```json\n{"a": 1}\n```') == {"a": 1}


def test_db_flow(tmp_path):
    db = DB(tmp_path / "t.db")
    i = db.add_item({"x": 1}, 100, 80)
    assert db.get_item(i)["status"] == "awaiting_approval"
    db.set_status(i, "published")
    assert db.get_item(i)["status"] == "published"


def test_enhance(tmp_path):
    src = tmp_path / "a.png"
    Image.new("RGB", (3000, 2000), (120, 130, 140)).save(src)
    out = enhance(src, tmp_path / "o.jpg")
    assert max(Image.open(out).size) <= 1600


def test_create_llm_unknown_provider():
    with pytest.raises(ValueError):
        create_llm("nope", "k", "m")


def test_create_llm_gemini_with_images(tmp_path, monkeypatch):
    img = tmp_path / "a.jpg"
    Image.new("RGB", (10, 10)).save(img)
    llm = create_llm("gemini", "fake-key", "gemini-2.5-flash")
    assert isinstance(llm, GeminiLLM)

    seen = {}

    class FakeResp:
        text = '```json\n{"ok": true}\n```'

    def fake_generate(model, contents, config):
        seen.update(model=model, contents=contents, config=config)
        return FakeResp()

    monkeypatch.setattr(llm.client.models, "generate_content", fake_generate)
    assert llm.json_call("מערכת", "שאלה", images=[img]) == {"ok": True}
    assert seen["model"] == "gemini-2.5-flash"
    assert len(seen["contents"]) == 3  # כותרת תמונה, התמונה, הטקסט
    assert seen["config"].system_instruction == "מערכת"


def test_config_defaults_to_gemini(tmp_path, monkeypatch):
    for k in ("LLM_PROVIDER", "LLM_MODEL", "ANTHROPIC_API_KEY"):
        monkeypatch.delenv(k, raising=False)
    monkeypatch.setenv("GEMINI_API_KEY", "g")
    monkeypatch.setenv("TELEGRAM_BOT_TOKEN", "t")
    monkeypatch.setenv("OWNER_TELEGRAM_ID", "1")
    monkeypatch.setenv("DATA_DIR", str(tmp_path))
    monkeypatch.chdir(tmp_path)
    cfg = load_config()
    assert (cfg.provider, cfg.llm_key, cfg.model) == ("gemini", "g", "gemini-2.5-flash")


def test_config_switch_to_anthropic(tmp_path, monkeypatch):
    monkeypatch.setenv("LLM_PROVIDER", "anthropic")
    monkeypatch.setenv("ANTHROPIC_API_KEY", "a")
    monkeypatch.delenv("LLM_MODEL", raising=False)
    monkeypatch.setenv("TELEGRAM_BOT_TOKEN", "t")
    monkeypatch.setenv("OWNER_TELEGRAM_ID", "1")
    monkeypatch.setenv("DATA_DIR", str(tmp_path))
    monkeypatch.chdir(tmp_path)
    cfg = load_config()
    assert (cfg.provider, cfg.llm_key, cfg.model) == ("anthropic", "a", "claude-sonnet-5-5")
