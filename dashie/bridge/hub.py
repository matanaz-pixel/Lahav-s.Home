"""Dashie Hub: תיאום בין שני מכשירים דרך ה-bridge.

'car'    = Core2 על הדשבורד (רמקול/A2DP, מסך גדול).
'pocket' = Plus2 בכיס (הקלטת הערות משמרת, רטט).

המכשירים לא מדברים ישירות. כל אחד שולח heartbeat (POST /hub/hello) שהוא גם poll, ומקבל
בתשובה פקודות. אין שום רדיו נוסף, וזה מה שמחזיק את ה-ESP32 יציב עם Wi-Fi + Bluetooth.

נקודות קצה (כולן מוגנות ב-BRIDGE_TOKEN, כמו /shift/*):
    POST /hub/hello   heartbeat + poll
    POST /hub/ack     אישור פקודה
    POST /hub/alert   התראת תאונה מהרכב (crash)
    GET  /hub/state   מצב לניפוי שגיאות (בלי מידע רגיש)
"""
import json
import os
import re
import secrets
import smtplib
import ssl
import threading
import time
from contextlib import contextmanager
from datetime import datetime, timezone
from email.message import EmailMessage
from pathlib import Path

from flask import Blueprint, jsonify

import shift

bp = Blueprint("hub", __name__)
LOCK = threading.RLock()

ROLES = ("car", "pocket")
ALERT_TYPES = ("crash",)
MAX_ALERTS_KEPT = 50
ACKED_KEEP_S = 3600
ALERT_EMAIL_MIN_GAP_S = 300


# ---------------------------------------------------------------- הגדרות (נקראות בכל בקשה, כדי שאפשר לשנות ב-env)


def _env_int(name: str, default: int) -> int:
    try:
        return int(os.environ.get(name, default))
    except (TypeError, ValueError):
        return default


def car_ssid() -> str:
    return (os.environ.get("HUB_CAR_SSID") or "").strip()


def cmd_ttl_s() -> int:
    return _env_int("HUB_CMD_TTL_S", 600)


def ack_retry_s() -> int:
    return _env_int("HUB_ACK_RETRY_S", 30)


def max_retries() -> int:
    return _env_int("HUB_MAX_RETRIES", 2)


def online_s() -> int:
    return _env_int("HUB_ONLINE_S", 90)


def debounce() -> int:
    return _env_int("HUB_DEBOUNCE", 2)


def poll_s(role: str) -> int:
    return _env_int("HUB_POLL_CAR_S", 10) if role == "car" else _env_int("HUB_POLL_POCKET_S", 20)


def now() -> float:
    """בנפרד כדי שאפשר יהיה להחליף בבדיקות."""
    return time.time()


# ---------------------------------------------------------------- מצב בדיסק


def state_path() -> Path:
    p = os.environ.get("HUB_STATE_FILE")
    return Path(p) if p else shift.shifts_dir().parent / "hub_state.json"


def _empty_state() -> dict:
    return {
        "devices": {},
        "commands": [],
        "car_read": {},
        "car_session": {"active": False, "id": 0, "offered": []},
        "alerts": {},
        "last_alert_email": 0,
    }


def _load(path: Path, t: float) -> dict:
    st = shift._read_json(path, None)
    if not isinstance(st, dict):
        st = _empty_state()
    base = _empty_state()
    for k, v in base.items():
        ok = isinstance(st.get(k), (int, float)) if isinstance(v, (int, float)) else isinstance(st.get(k), type(v))
        if k not in st or not ok:
            st[k] = v
    # TTL: פקודות שפג תוקפן או שאושרו לפני יותר משעה נמחקות
    keep = []
    for c in st["commands"]:
        if not isinstance(c, dict):
            continue
        if c.get("acked"):
            if t - c["acked"] > ACKED_KEEP_S:
                continue
        elif t >= c.get("expires", 0):
            continue
        keep.append(c)
    st["commands"] = keep
    return st


@contextmanager
def state():
    """טעינה, עדכון ושמירה אטומית תחת נעילה (thread + קובץ). חריגה לא שומרת."""
    with LOCK:
        path = state_path()
        path.parent.mkdir(parents=True, exist_ok=True)
        lockf = None
        try:
            import fcntl

            lockf = open(str(path) + ".lock", "a")
            fcntl.flock(lockf, fcntl.LOCK_EX)
        except Exception:  # noqa: BLE001  (Windows / אין fcntl: נשארים עם נעילת ה-thread)
            lockf = None
        try:
            st = _load(path, now())
            yield st
            shift._write_atomic(path, json.dumps(st, ensure_ascii=False, indent=1))
        finally:
            if lockf is not None:
                lockf.close()


# ---------------------------------------------------------------- אבטחה


@bp.before_request
def _guard():
    if not shift._auth_ok():
        return jsonify(status="error", error="unauthorized"), 401
    return None


# ---------------------------------------------------------------- פקודות


def new_command(st: dict, device_id: str, ctype: str, payload: dict, t: float) -> dict:
    c = {
        "id": secrets.token_hex(6),
        "device_id": device_id,
        "type": ctype,
        "created": t,
        "expires": t + cmd_ttl_s(),
        "deliveries": 0,
        "last_delivery": 0,
        "acked": None,
    }
    c.update(payload)
    st["commands"].append(c)
    return c


def live_command(st: dict, device_id: str, ctype: str, t: float, **match):
    """פקודה חיה (לא אושרה, לא פגה) מאותו סוג ליעד, אם יש."""
    for c in st["commands"]:
        if c["device_id"] == device_id and c["type"] == ctype and not c.get("acked") and t < c["expires"]:
            if all(c.get(k) == v for k, v in match.items()):
                return c
    return None


def due_commands(st: dict, device_id: str, t: float) -> list:
    """פקודות שצריך למסור עכשיו. אין ack => מסירה חוזרת אחרי ack_retry_s, עד max_retries פעמים."""
    out = []
    for c in st["commands"]:
        if c["device_id"] != device_id or c.get("acked") or t >= c["expires"]:
            continue
        if c["deliveries"] >= 1 + max_retries():
            continue
        if c["deliveries"] > 0 and t - c["last_delivery"] < ack_retry_s():
            continue
        c["deliveries"] += 1
        c["last_delivery"] = t
        out.append(c)
    return out


def render_command(c: dict, t: float) -> dict:
    """הצורה שהמכשיר רואה (בלי שדות פנימיים). כתובות האודיו נחתמות בזמן המסירה."""
    out = {
        "id": c["id"],
        "type": c["type"],
        "attempt": c["deliveries"],
        "expires_in_s": max(0, int(c["expires"] - t)),
    }
    if c["type"] == "read_summary":
        out["shift_id"] = c["shift_id"]
        out["audio_urls"] = shift.audio_urls(c["shift_id"], int(c.get("audio_count", 0)))
        out["spoken"] = c.get("spoken", "")
    elif c["type"] == "vibrate":
        out["pattern"] = c.get("pattern", "crash")
        if c.get("g") is not None:
            out["g"] = c["g"]
    elif c["type"] == "find_me":
        out["duration_s"] = c.get("duration_s", 30)
    return out


# ---------------------------------------------------------------- זיהוי 'באוטו'


def _qualifying(st: dict, role: str, t: float) -> list:
    """מכשירים מהתפקיד שחיים ומחוברים ל-SSID של הרכב כבר debounce() heartbeats רצופים."""
    if not car_ssid():
        return []
    return [
        did
        for did, d in st["devices"].items()
        if d.get("role") == role and t - d.get("last_seen", 0) <= online_s() and d.get("car_streak", 0) >= debounce()
    ]


def eval_in_car(st: dict, t: float) -> bool:
    return bool(_qualifying(st, "car", t)) and bool(_qualifying(st, "pocket", t))


def update_session(st: dict, in_car: bool):
    s = st["car_session"]
    if in_car and not s["active"]:
        s["active"] = True
        s["id"] += 1
        s["offered"] = []
    elif not in_car and s["active"]:
        s["active"] = False


def latest_ready_shift():
    """הסיכום המוכן האחרון (status=done, עם אודיו להקראה). מחזיר (shift_id, meta) או None."""
    root = shift.shifts_dir()
    best = None
    try:
        dirs = [p for p in root.iterdir() if p.is_dir()]
    except OSError:
        return None
    for p in dirs:
        if not shift._valid_id(p.name):
            continue
        meta = shift.load_meta(p.name)
        if meta.get("status") != "done" or int(meta.get("audio_urls_count") or 0) <= 0:
            continue
        key = (meta.get("ended_at") or "", p.stat().st_mtime)
        if best is None or key > best[0]:
            best = (key, p.name, meta)
    return (best[1], best[2]) if best else None


def queue_read_summary(st: dict, device_id: str, shift_id: str, meta: dict, t: float):
    existing = live_command(st, device_id, "read_summary", t, shift_id=shift_id)
    if existing:
        return existing
    return new_command(
        st,
        device_id,
        "read_summary",
        {"shift_id": shift_id, "audio_count": int(meta.get("audio_urls_count") or 0), "spoken": meta.get("spoken", "")},
        t,
    )


def maybe_offer_summary(st: dict, t: float):
    """בכניסה לרכב: פקודת read_summary אחת לכל סיכום שטרם הוקרא, פעם אחת בכל 'ישיבה' ברכב."""
    sess = st["car_session"]
    ready = latest_ready_shift()
    if not ready:
        return
    shift_id, meta = ready
    if shift_id in st["car_read"] or shift_id in sess["offered"]:
        return
    for did in _qualifying(st, "car", t):
        queue_read_summary(st, did, shift_id, meta, t)
    sess["offered"].append(shift_id)


# ---------------------------------------------------------------- hello / ack / alert


def _clean_id(v) -> str:
    return v if isinstance(v, str) and shift._valid_id(v) else ""


def _int_or_none(v, lo=0, hi=100):
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return None
    return max(lo, min(hi, int(v)))


def peer_of(st: dict, me: dict, my_id: str, t: float):
    other = "pocket" if me["role"] == "car" else "car"
    cands = [(d.get("last_seen", 0), did, d) for did, d in st["devices"].items() if d.get("role") == other]
    if not cands:
        return None
    _, did, d = max(cands)
    return {
        "device_id": did,
        "role": d["role"],
        "battery": d.get("battery"),
        "notes_pending": d.get("notes_pending"),
        "shift_active": d.get("shift_active"),
        "last_seen_s": max(0, int(t - d.get("last_seen", t))),
    }


@bp.post("/hub/hello")
def hello():
    body = shift._json_body()
    did = _clean_id(body.get("device_id"))
    role = body.get("role")
    if not did:
        return shift._err(400, "bad_device_id")
    if role not in ROLES:
        return shift._err(400, "bad_role")
    ssid = body.get("ssid")
    ssid = ssid.strip()[:32] if isinstance(ssid, str) else ""
    fw = body.get("fw")
    fw = fw[:32] if isinstance(fw, str) else ""
    t = now()
    with state() as st:
        dev = st["devices"].get(did) or {}
        prev = dev.get("last_seen", 0)
        streak = dev.get("car_streak", 0)
        if car_ssid() and ssid == car_ssid():
            streak = streak + 1 if prev and t - prev <= online_s() else 1
        else:
            streak = 0
        dev.update(
            role=role,
            ssid=ssid,
            battery=_int_or_none(body.get("battery")),
            notes_pending=_int_or_none(body.get("notes_pending"), 0, 100000),
            shift_active=bool(body.get("shift_active")) if "shift_active" in body else None,
            fw=fw,
            last_seen=t,
            car_streak=streak,
        )
        st["devices"][did] = dev
        in_car = eval_in_car(st, t)
        update_session(st, in_car)
        if in_car:
            maybe_offer_summary(st, t)
        due = due_commands(st, did, t)
        commands = [render_command(c, t) for c in due]
        peer = peer_of(st, dev, did, t)
    return jsonify(
        status="ok",
        server_time=datetime.fromtimestamp(t, timezone.utc).isoformat(timespec="seconds"),
        server_epoch=int(t),
        poll_s=poll_s(role),
        in_car=in_car,
        commands=commands,
        peer=peer,
    )


@bp.post("/hub/ack")
def ack():
    body = shift._json_body()
    did = _clean_id(body.get("device_id"))
    cid = body.get("command_id")
    if not did:
        return shift._err(400, "bad_device_id")
    if not isinstance(cid, str) or not cid:
        return shift._err(400, "bad_command_id")
    t = now()
    with state() as st:
        for c in st["commands"]:
            if c["id"] == cid and c["device_id"] == did:
                if c.get("acked"):
                    return jsonify(status="ok", already=True)
                c["acked"] = t
                if c["type"] == "read_summary":
                    st["car_read"][c["shift_id"]] = t
                return jsonify(status="ok", already=False)
    # פג תוקף או לא קיים: המכשיר צריך להתייחס לזה כסיום ולא לנסות שוב
    return shift._err(404, "unknown_command")


def send_alert_email(subject: str, text: str):
    """מייל קצר אל SHIFT_EMAIL_TO בלבד. זורק חריגה בכשל."""
    host = os.environ.get("SMTP_HOST")
    to = os.environ.get("SHIFT_EMAIL_TO")
    if not (host and to):
        raise RuntimeError("not_configured")
    port = int(os.environ.get("SMTP_PORT", "587"))
    user = os.environ.get("SMTP_USER")
    pw = os.environ.get("SMTP_PASS")
    msg = EmailMessage()
    msg["Subject"] = subject
    msg["From"] = os.environ.get("SMTP_FROM") or user or to
    msg["To"] = to
    msg.set_content(text)
    ctx = ssl.create_default_context()
    if port == 465:
        with smtplib.SMTP_SSL(host, port, context=ctx, timeout=30) as s:
            if user:
                s.login(user, pw or "")
            s.send_message(msg)
    else:
        with smtplib.SMTP(host, port, timeout=30) as s:
            s.starttls(context=ctx)
            if user:
                s.login(user, pw or "")
            s.send_message(msg)


def crash_email_text(g, ts) -> tuple:
    g_part = f" (בערך {g:g}g)" if isinstance(g, (int, float)) else ""
    when = f" בשעה {ts}" if ts else ""
    subject = "התראה אוטומטית מ-Dashie: זוהתה פגיעה חזקה"
    text = (
        f"Dashie ברכב זיהה פגיעה חזקה{g_part}{when}.\n"
        "זו התראה אוטומטית ועלולה להיות שגויה, למשל בגלל מהמורה או נפילה של המכשיר.\n"
        "אם הכול בסדר, אין צורך לעשות דבר. אם לא, אנא צור קשר.\n"
    )
    return subject, text


@bp.post("/hub/alert")
def alert():
    body = shift._json_body()
    did = _clean_id(body.get("device_id"))
    atype = body.get("type")
    if not did:
        return shift._err(400, "bad_device_id")
    if atype not in ALERT_TYPES:
        return shift._err(400, "bad_type")
    g = body.get("g")
    g = round(float(g), 1) if isinstance(g, (int, float)) and not isinstance(g, bool) and 0 <= g <= 100 else None
    ts = body.get("ts")
    ts = str(ts)[:40] if isinstance(ts, (str, int, float)) and not isinstance(ts, bool) else None
    t = now()
    send_mail = False
    with state() as st:
        key = f"{did}:{atype}:{ts}" if ts is not None else None
        if key and key in st["alerts"]:
            return jsonify(status="duplicate")
        queued = []
        for pid, d in st["devices"].items():
            if d.get("role") == "pocket":
                new_command(st, pid, "vibrate", {"pattern": "crash", "g": g}, t)
                queued.append(pid)
        if key:
            st["alerts"][key] = t
            while len(st["alerts"]) > MAX_ALERTS_KEPT:
                st["alerts"].pop(next(iter(st["alerts"])))
        if os.environ.get("HUB_CRASH_EMAIL") == "1" and t - st.get("last_alert_email", 0) >= ALERT_EMAIL_MIN_GAP_S:
            st["last_alert_email"] = t
            send_mail = True
    email = None  # None = כבוי או מוגבל קצב
    if send_mail:
        try:
            subject, text = crash_email_text(g, ts)
            send_alert_email(subject, text)
            email = True
        except Exception:  # noqa: BLE001  (כשל מייל לא מפיל את ההתראה לכיס)
            email = False
    return jsonify(status="ok", queued_to=queued, email=email)


@bp.get("/hub/state")
def get_state():
    t = now()
    with state() as st:
        devices = {
            did: {
                "role": d.get("role"),
                "ssid": d.get("ssid"),
                "battery": d.get("battery"),
                "notes_pending": d.get("notes_pending"),
                "shift_active": d.get("shift_active"),
                "fw": d.get("fw"),
                "last_seen_s": int(t - d.get("last_seen", t)),
                "car_streak": d.get("car_streak", 0),
            }
            for did, d in st["devices"].items()
        }
        pending = [{"id": c["id"], "type": c["type"], "device_id": c["device_id"], "deliveries": c["deliveries"]}
                   for c in st["commands"] if not c.get("acked")]
        return jsonify(
            devices=devices,
            in_car=eval_in_car(st, t),
            car_session=dict(st["car_session"]),
            pending_commands=pending,
            car_ssid_configured=bool(car_ssid()),
        )


# ---------------------------------------------------------------- intents קוליים ל-/voice


_HEBREW = re.compile(r"[֐-׿]")
_NIQQUD = re.compile(r"[֑-ׇ]")
_PUNCT = re.compile(r"[^\w\s]", re.UNICODE)

# התאמה מכוונת מצומצמת (עדיף לפספס מאשר להפעיל פקודה בטעות).
# hook לשדרוג: להחליף/להוסיף את detect_intent במסווג LLM שמחזיר את אותו מבנה {"intent": ...}.
FIND_PATTERNS = [
    re.compile(r"\bאיפה\s+ה?כיס\b"),
    re.compile(r"\bאיפה\s+המכשיר\s+שלי\b"),
    re.compile(r"\b(תמצא|מצא|חפש|תחפש)\s+(לי\s+)?(את\s+)?(ה?כיס|המכשיר\s+שלי)\b"),
    re.compile(r"\bwhere(?:s| is)\s+my\s+(pocket|device)\b"),
    re.compile(r"\b(find|ping|ring|beep)\s+(my\s+)?pocket(\s+device)?\b"),
]
READ_PATTERNS = [
    re.compile(r"\b(תקריא|תקרא|הקרא|השמע)\s+(לי\s+)?(את\s+)?ה?סיכום\b"),
    re.compile(r"\bread\s+(me\s+)?(the\s+|my\s+)?(shift\s+)?summary\b"),
]


def normalize(text: str) -> str:
    t = _NIQQUD.sub("", text or "").lower().replace("'", "").replace("\u2019", "")
    t = _PUNCT.sub(" ", t)
    return re.sub(r"\s+", " ", t).strip()


def detect_intent(text: str):
    """מחזיר 'find_me' / 'read_summary' / None. נקודת החלפה לשדרוג ל-LLM."""
    t = normalize(text)
    if not t:
        return None
    if any(p.search(t) for p in FIND_PATTERNS):
        return "find_me"
    if any(p.search(t) for p in READ_PATTERNS):
        return "read_summary"
    return None


def _reply(text: str, he: str, en: str) -> str:
    return he if _HEBREW.search(text) else en


def handle_voice_intent(text: str, device_id=None):
    """נקרא מ-/voice. מחזיר {"reply","action"} כשזוהתה פקודת Hub ויצרנו אותה, אחרת None.
    כש-BRIDGE_TOKEN מוגדר, ללא Authorization תקף לא מתבצעת שום פעולה והבקשה ממשיכה כרגיל."""
    intent = detect_intent(text)
    if not intent or not shift._auth_ok():
        return None
    t = now()
    did = _clean_id(device_id)
    with state() as st:
        if intent == "find_me":
            targets = [pid for pid, d in st["devices"].items() if d.get("role") == "pocket"]
            if not targets:
                return {"reply": _reply(text, "לא מצאתי מכשיר כיס רשום.", "No pocket device is registered."),
                        "action": "face:confused"}
            stale = True
            for pid in targets:
                if not live_command(st, pid, "find_me", t):
                    new_command(st, pid, "find_me", {"duration_s": 30}, t)
                if t - st["devices"][pid].get("last_seen", 0) <= online_s():
                    stale = False
            if stale:
                return {"reply": _reply(text, "שלחתי, אבל הכיס לא התחבר לאחרונה. הוא יצפצף כשיתחבר.",
                                        "Sent, but the pocket device hasn't connected lately. It will beep when it does."),
                        "action": "hub:find_me"}
            return {"reply": _reply(text, "מצפצף על הכיס שלך.", "Pinging your pocket device."), "action": "hub:find_me"}
        # read_summary
        ready = latest_ready_shift()
        if not ready:
            return {"reply": _reply(text, "אין סיכום מוכן.", "There is no summary ready."), "action": "face:confused"}
        shift_id, meta = ready
        car_ids = [did] if did and st["devices"].get(did, {}).get("role") == "car" else [
            cid for cid, d in st["devices"].items() if d.get("role") == "car"]
        if not car_ids:
            return {"reply": _reply(text, "לא מצאתי מכשיר רכב רשום.", "No car device is registered."),
                    "action": "face:confused"}
        for cid in car_ids:
            queue_read_summary(st, cid, shift_id, meta, t)
        return {"reply": _reply(text, "מקריא את הסיכום.", "Reading the summary."), "action": "hub:read_summary"}
