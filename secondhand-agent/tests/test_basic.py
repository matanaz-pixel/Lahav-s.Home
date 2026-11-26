from PIL import Image

from secondhand_agent.db import DB
from secondhand_agent.images import enhance
from secondhand_agent.llm import _extract_json


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
