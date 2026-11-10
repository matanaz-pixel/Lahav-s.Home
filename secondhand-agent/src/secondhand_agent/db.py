import json
import sqlite3
from pathlib import Path

STATUSES = ("draft", "awaiting_approval", "published", "negotiating", "handoff", "sold", "archived")

SCHEMA = """
CREATE TABLE IF NOT EXISTS items(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  status TEXT NOT NULL DEFAULT 'draft',
  data TEXT NOT NULL,           -- JSON: פרטי פריט, תמונות, מודעה, תמחור
  price INTEGER, floor_price INTEGER,
  created_at TEXT DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS messages(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  item_id INTEGER NOT NULL REFERENCES items(id),
  channel TEXT, buyer TEXT, direction TEXT, text TEXT,
  created_at TEXT DEFAULT CURRENT_TIMESTAMP
);
"""


class DB:
    def __init__(self, path: Path):
        self.conn = sqlite3.connect(path, check_same_thread=False)
        self.conn.executescript(SCHEMA)

    def add_item(self, data: dict, price: int | None, floor: int | None) -> int:
        cur = self.conn.execute(
            "INSERT INTO items(status,data,price,floor_price) VALUES('awaiting_approval',?,?,?)",
            (json.dumps(data, ensure_ascii=False), price, floor),
        )
        self.conn.commit()
        return cur.lastrowid

    def set_status(self, item_id: int, status: str) -> None:
        if status not in STATUSES:
            raise ValueError(status)
        self.conn.execute("UPDATE items SET status=? WHERE id=?", (status, item_id))
        self.conn.commit()

    def get_item(self, item_id: int) -> dict | None:
        r = self.conn.execute("SELECT id,status,data,price,floor_price FROM items WHERE id=?", (item_id,)).fetchone()
        if not r:
            return None
        return {"id": r[0], "status": r[1], "data": json.loads(r[2]), "price": r[3], "floor_price": r[4]}
