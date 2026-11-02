import json

import pytest

import hub
import shift
from conftest import make_wav

CAR_SSID = "PhoneHotspot"


class Clock:
    def __init__(self, t=1_000_000.0):
        self.t = t

    def __call__(self):
        return self.t

    def advance(self, s):
        self.t += s


@pytest.fixture
def clock(monkeypatch):
    c = Clock()
    monkeypatch.setattr(hub, "now", c)
    return c


@pytest.fixture
def hubenv(env, monkeypatch, clock):
    monkeypatch.setenv("HUB_CAR_SSID", CAR_SSID)
    for k in ("HUB_CRASH_EMAIL", "HUB_CMD_TTL_S", "HUB_ACK_RETRY_S", "HUB_MAX_RETRIES", "HUB_ONLINE_S", "HUB_DEBOUNCE"):
        monkeypatch.delenv(k, raising=False)
    return env


def hello(client, did, role, ssid=CAR_SSID, headers=None, **extra):
    body = {"device_id": did, "role": role, "ssid": ssid, "battery": 80, "notes_pending": 0, "shift_active": False, "fw": "t"}
    body.update(extra)
    return client.post("/hub/hello", json=body, headers=headers or {})


def both_in_car(client, clock, rounds=2):
    """שני המכשירים שולחים heartbeat על ה-SSID של הרכב (rounds פעמים כל אחד)."""
    last = None
    for _ in range(rounds):
        hello(client, "pocket1", "pocket")
        last = hello(client, "car1", "car")
        clock.advance(10)
    return last


def make_done_shift(shift_id="s1", ended="2025-06-12T23:40:00", audio=2, spoken="משמרת עמוסה"):
    meta = shift.new_meta(shift_id, "2025-06-12T16:00:00")
    meta.update(status="done", ended_at=ended, audio_urls_count=audio, spoken=spoken)
    shift.save_meta(shift_id, meta)
    for i in range(audio):
        shift._write_atomic(shift._sdir(shift_id) / f"spoken_{i}.wav", make_wav(100))
    return meta


# ---------------------------------------------------------------- hello


def test_hello_validation(client, hubenv):
    assert hello(client, "bad id!", "car").status_code == 400
    assert hello(client, "car1", "robot").status_code == 400
    assert client.post("/hub/hello", data="x", content_type="text/plain").status_code == 400


def test_hello_shape_and_peer(client, hubenv, clock):
    j = hello(client, "car1", "car", battery=100).get_json()
    assert j["status"] == "ok" and j["commands"] == [] and j["peer"] is None
    assert j["poll_s"] == 10 and j["in_car"] is False
    assert "T" in j["server_time"] and j["server_epoch"] == int(clock.t)

    clock.advance(30)
    j = hello(client, "pocket1", "pocket", battery=55, notes_pending=3, shift_active=True).get_json()
    assert j["poll_s"] == 20
    assert j["peer"]["role"] == "car" and j["peer"]["battery"] == 100 and j["peer"]["last_seen_s"] == 30

    clock.advance(5)
    peer = hello(client, "car1", "car").get_json()["peer"]
    assert peer["role"] == "pocket" and peer["battery"] == 55 and peer["notes_pending"] == 3
    assert peer["shift_active"] is True and peer["last_seen_s"] == 5


def test_state_persisted_and_corrupt_file_recovers(client, hubenv):
    hello(client, "car1", "car")
    p = hubenv / "hub_state.json"
    assert json.loads(p.read_text())["devices"]["car1"]["role"] == "car"
    p.write_text("{not json")
    j = hello(client, "pocket1", "pocket").get_json()
    assert j["status"] == "ok" and j["peer"] is None  # המצב הישן אבד, אבל השרת לא נפל


# ---------------------------------------------------------------- זיהוי באוטו + הקראה


def test_in_car_needs_debounce(client, hubenv, clock):
    make_done_shift()
    j = hello(client, "pocket1", "pocket").get_json()
    j = hello(client, "car1", "car").get_json()
    assert j["in_car"] is False and j["commands"] == []  # heartbeat אחד לכל מכשיר: עדיין לא
    clock.advance(10)
    hello(client, "pocket1", "pocket")
    j = hello(client, "car1", "car").get_json()
    assert j["in_car"] is True
    assert [c["type"] for c in j["commands"]] == ["read_summary"]


def test_read_summary_command_contents_and_signing(client, hubenv, clock, monkeypatch):
    monkeypatch.setenv("BRIDGE_TOKEN", "secret")
    make_done_shift(audio=2)
    auth = {"Authorization": "Bearer secret"}
    for _ in range(2):
        hello(client, "pocket1", "pocket", headers=auth)
        j = hello(client, "car1", "car", headers=auth).get_json()
        clock.advance(10)
    cmd = j["commands"][0] if j["commands"] else None
    assert cmd and cmd["shift_id"] == "s1" and cmd["spoken"] == "משמרת עמוסה"
    assert len(cmd["audio_urls"]) == 2 and cmd["attempt"] == 1
    assert all("?t=" in u for u in cmd["audio_urls"])
    # הקישור החתום עובד בלי כותרת
    path = cmd["audio_urls"][0].replace("http://localhost", "")
    assert client.get(path).status_code == 200


def test_different_ssid_resets_streak(client, hubenv, clock):
    make_done_shift()
    for _ in range(3):
        hello(client, "pocket1", "pocket", ssid="KitchenWifi")
        j = hello(client, "car1", "car").get_json()
        clock.advance(10)
    assert j["in_car"] is False and j["commands"] == []

    # רצף נשבר באמצע: SSID אחר מאפס את המונה
    hello(client, "pocket1", "pocket")
    hello(client, "pocket1", "pocket", ssid="KitchenWifi")
    clock.advance(10)
    hello(client, "pocket1", "pocket")
    j = hello(client, "car1", "car").get_json()
    assert j["in_car"] is False


def test_in_car_requires_both_alive(client, hubenv, clock):
    make_done_shift()
    both_in_car(client, clock)
    clock.advance(120)  # הכיס נעלם (מעל 90 שניות)
    j = hello(client, "car1", "car").get_json()
    assert j["in_car"] is False


def test_no_car_ssid_configured_disables_detection(client, hubenv, clock, monkeypatch):
    monkeypatch.delenv("HUB_CAR_SSID")
    make_done_shift()
    j = both_in_car(client, clock, rounds=3).get_json()
    assert j["in_car"] is False and j["commands"] == []


def test_no_ready_summary_no_command(client, hubenv, clock):
    j = both_in_car(client, clock).get_json()
    assert j["in_car"] is True and j["commands"] == []
    # סיכום בלי אודיו או לא גמור לא נחשב מוכן
    meta = shift.new_meta("open1")
    shift.save_meta("open1", meta)
    make_done_shift("noaudio", audio=0)
    clock.advance(10)
    hello(client, "pocket1", "pocket")
    assert hello(client, "car1", "car").get_json()["commands"] == []


def test_latest_shift_is_chosen(client, hubenv, clock):
    make_done_shift("old", ended="2025-06-10T23:00:00")
    make_done_shift("new", ended="2025-06-12T23:00:00")
    j = both_in_car(client, clock).get_json()
    assert j["commands"][0]["shift_id"] == "new"


def test_ack_and_not_reoffered(client, hubenv, clock):
    make_done_shift()
    j = both_in_car(client, clock).get_json()
    cid = j["commands"][0]["id"]
    r = client.post("/hub/ack", json={"device_id": "car1", "command_id": cid})
    assert r.get_json() == {"status": "ok", "already": False}
    assert client.post("/hub/ack", json={"device_id": "car1", "command_id": cid}).get_json()["already"] is True

    for _ in range(3):
        clock.advance(40)
        hello(client, "pocket1", "pocket")
        assert hello(client, "car1", "car").get_json()["commands"] == []

    # יצא מהרכב וחזר: הסיכום כבר הוקרא, אין הצעה חדשה
    clock.advance(500)
    hello(client, "pocket1", "pocket", ssid="Home")
    hello(client, "car1", "car", ssid="Home")
    clock.advance(10)
    j = both_in_car(client, clock).get_json()
    assert j["in_car"] is True and j["commands"] == []


def test_offered_once_per_session_but_again_next_session_if_unread(client, hubenv, clock):
    make_done_shift()
    j = both_in_car(client, clock).get_json()
    assert len(j["commands"]) == 1
    # פקודה שלא אושרה לא נוצרת מחדש באותה ישיבה אחרי שפג תוקפה
    clock.advance(700)
    hello(client, "pocket1", "pocket")
    j = hello(client, "car1", "car").get_json()
    assert j["commands"] == []
    # ישיבה חדשה ברכב: מציעים שוב, כי הסיכום לא הוקרא
    hello(client, "pocket1", "pocket", ssid="Home")
    hello(client, "car1", "car", ssid="Home")
    clock.advance(10)
    j = both_in_car(client, clock).get_json()
    assert [c["type"] for c in j["commands"]] == ["read_summary"]


def test_ack_wrong_device_or_unknown(client, hubenv, clock):
    make_done_shift()
    cid = both_in_car(client, clock).get_json()["commands"][0]["id"]
    assert client.post("/hub/ack", json={"device_id": "pocket1", "command_id": cid}).status_code == 404
    assert client.post("/hub/ack", json={"device_id": "car1", "command_id": "nope"}).status_code == 404
    assert client.post("/hub/ack", json={"device_id": "car1"}).status_code == 400
    assert client.post("/hub/ack", json={"command_id": cid}).status_code == 400


# ---------------------------------------------------------------- ack וניסיון חוזר


def test_redelivery_until_max_retries(client, hubenv, clock):
    make_done_shift()
    j = both_in_car(client, clock, rounds=2).get_json()  # מסירה ראשונה בסוף הסיבוב השני
    assert j["commands"][0]["attempt"] == 1
    cid = j["commands"][0]["id"]

    def poll():
        hello(client, "pocket1", "pocket")
        return hello(client, "car1", "car").get_json()["commands"]

    clock.advance(10)
    assert poll() == []  # עוד לא עברו 30 שניות
    clock.advance(25)
    c2 = poll()
    assert len(c2) == 1 and c2[0]["id"] == cid and c2[0]["attempt"] == 2
    clock.advance(31)
    c3 = poll()
    assert len(c3) == 1 and c3[0]["attempt"] == 3
    clock.advance(31)
    assert poll() == []  # מיצינו ניסיון ראשון + 2 חוזרים
    clock.advance(31)
    assert poll() == []


def test_ack_stops_redelivery(client, hubenv, clock):
    make_done_shift()
    cid = both_in_car(client, clock).get_json()["commands"][0]["id"]
    clock.advance(31)
    hello(client, "pocket1", "pocket")
    assert hello(client, "car1", "car").get_json()["commands"][0]["attempt"] == 2
    client.post("/hub/ack", json={"device_id": "car1", "command_id": cid})
    clock.advance(31)
    hello(client, "pocket1", "pocket")
    assert hello(client, "car1", "car").get_json()["commands"] == []


def test_ttl_expires_commands(client, hubenv, clock):
    make_done_shift()
    j = both_in_car(client, clock).get_json()
    assert j["commands"][0]["expires_in_s"] == 600
    clock.advance(601)
    hello(client, "pocket1", "pocket")
    assert hello(client, "car1", "car").get_json()["commands"] == []
    st = json.loads((hubenv / "hub_state.json").read_text())
    assert [c for c in st["commands"] if c["type"] == "read_summary" and not c["acked"]] == []


def test_ttl_configurable(client, hubenv, clock, monkeypatch):
    monkeypatch.setenv("HUB_CMD_TTL_S", "60")
    make_done_shift()
    j = both_in_car(client, clock).get_json()
    assert j["commands"][0]["expires_in_s"] == 60


# ---------------------------------------------------------------- התראת תאונה


def test_crash_alert_queues_vibrate_for_pocket(client, hubenv, clock):
    hello(client, "car1", "car")
    hello(client, "pocket1", "pocket")
    r = client.post("/hub/alert", json={"device_id": "car1", "type": "crash", "g": 5.26, "ts": "2025-06-12T20:00:00"})
    j = r.get_json()
    assert j["status"] == "ok" and j["queued_to"] == ["pocket1"] and j["email"] is None
    cmds = hello(client, "pocket1", "pocket").get_json()["commands"]
    assert len(cmds) == 1 and cmds[0]["type"] == "vibrate" and cmds[0]["pattern"] == "crash" and cmds[0]["g"] == 5.3
    # מכשיר הרכב לא מקבל את הרטט
    assert hello(client, "car1", "car").get_json()["commands"] == []


def test_crash_alert_duplicate_and_validation(client, hubenv):
    hello(client, "pocket1", "pocket")
    body = {"device_id": "car1", "type": "crash", "g": 4.2, "ts": 12345}
    assert client.post("/hub/alert", json=body).get_json()["status"] == "ok"
    assert client.post("/hub/alert", json=body).get_json()["status"] == "duplicate"
    assert client.post("/hub/alert", json={**body, "type": "fire"}).status_code == 400
    assert client.post("/hub/alert", json={"type": "crash"}).status_code == 400


def test_crash_alert_without_pocket_device(client, hubenv):
    j = client.post("/hub/alert", json={"device_id": "car1", "type": "crash"}).get_json()
    assert j["status"] == "ok" and j["queued_to"] == []


def test_crash_alert_retried_if_no_ack(client, hubenv, clock):
    hello(client, "pocket1", "pocket")
    client.post("/hub/alert", json={"device_id": "car1", "type": "crash", "ts": 1})
    a = hello(client, "pocket1", "pocket").get_json()["commands"]
    clock.advance(31)
    b = hello(client, "pocket1", "pocket").get_json()["commands"]
    assert a[0]["id"] == b[0]["id"] and b[0]["attempt"] == 2


def test_crash_email_gated_and_throttled(client, hubenv, clock, monkeypatch):
    sent = []
    monkeypatch.setattr(hub, "send_alert_email", lambda subject, text: sent.append((subject, text)))
    body = lambda ts: {"device_id": "car1", "type": "crash", "g": 6, "ts": ts}  # noqa: E731

    assert client.post("/hub/alert", json=body(1)).get_json()["email"] is None  # כבוי כברירת מחדל
    assert sent == []

    monkeypatch.setenv("HUB_CRASH_EMAIL", "1")
    assert client.post("/hub/alert", json=body(2)).get_json()["email"] is True
    assert len(sent) == 1
    subject, text = sent[0]
    assert "אוטומטית" in subject and "שגויה" in text and "6g" in text

    clock.advance(60)
    assert client.post("/hub/alert", json=body(3)).get_json()["email"] is None  # הגבלת קצב
    assert len(sent) == 1
    clock.advance(300)
    assert client.post("/hub/alert", json=body(4)).get_json()["email"] is True


def test_crash_email_failure_does_not_break_alert(client, hubenv, monkeypatch):
    def boom(subject, text):
        raise RuntimeError("smtp down")

    monkeypatch.setattr(hub, "send_alert_email", boom)
    monkeypatch.setenv("HUB_CRASH_EMAIL", "1")
    hello(client, "pocket1", "pocket")
    j = client.post("/hub/alert", json={"device_id": "car1", "type": "crash", "ts": 9}).get_json()
    assert j["status"] == "ok" and j["email"] is False and j["queued_to"] == ["pocket1"]
    assert hello(client, "pocket1", "pocket").get_json()["commands"][0]["type"] == "vibrate"


def test_send_alert_email_not_configured(hubenv):
    with pytest.raises(RuntimeError):
        hub.send_alert_email("s", "t")


# ---------------------------------------------------------------- intents קוליים


@pytest.mark.parametrize(
    "text,expected",
    [
        ("איפה הכיס שלי?", "find_me"),
        ("היי, איפה המכשיר שלי", "find_me"),
        ("תמצא לי את הכיס", "find_me"),
        ("Where's my pocket device?", "find_me"),
        ("find my pocket", "find_me"),
        ("תקריא את הסיכום", "read_summary"),
        ("תקריא לי את הסיכום, בבקשה", "read_summary"),
        ("Read me the shift summary", "read_summary"),
        ("מה השעה", None),
        ("איפה המכשיר הקרוב לתחנה", None),
        ("מה היה בסיכום של אתמול", None),
        ("", None),
    ],
)
def test_detect_intent(text, expected):
    assert hub.detect_intent(text) == expected


@pytest.fixture
def voice(client, hubenv, monkeypatch):
    import server

    state = {"text": "", "llm_calls": 0}
    monkeypatch.setattr(server, "transcribe", lambda wav: state["text"])

    def fake_llm(t):
        state["llm_calls"] += 1
        return "LLM"

    monkeypatch.setattr(server, "llm", fake_llm)
    monkeypatch.setattr(server, "tts_wav", lambda t: b"")

    def say(text, headers=None):
        state["text"] = text
        return client.post("/voice", data=make_wav(1600), headers=headers or {}).get_json()

    return say, state


def test_voice_find_me_queues_for_pocket(client, voice, clock):
    say, state = voice
    hello(client, "pocket1", "pocket")
    j = say("איפה הכיס שלי")
    assert j["reply"] == "מצפצף על הכיס שלך." and j["action"] == "hub:find_me"
    assert state["llm_calls"] == 0
    cmds = hello(client, "pocket1", "pocket").get_json()["commands"]
    assert [c["type"] for c in cmds] == ["find_me"] and cmds[0]["duration_s"] == 30
    # אין כפילות בפקודה חיה
    say("איפה הכיס שלי")
    st = json.loads(hub.state_path().read_text())
    assert len([c for c in st["commands"] if c["type"] == "find_me"]) == 1


def test_voice_find_me_english_and_stale_and_none(client, voice, clock):
    say, _ = voice
    assert say("where is my pocket")["reply"] == "No pocket device is registered."
    hello(client, "pocket1", "pocket")
    clock.advance(500)
    assert "hasn't connected" in say("find my pocket")["reply"]
    assert "לא התחבר" in say("איפה הכיס שלי")["reply"]


def test_voice_read_summary(client, voice, clock):
    say, _ = voice
    assert say("תקריא את הסיכום")["reply"] == "אין סיכום מוכן."
    make_done_shift()
    assert say("תקריא את הסיכום")["reply"] == "לא מצאתי מכשיר רכב רשום."
    hello(client, "car1", "car", ssid="Home")
    j = say("תקריא את הסיכום", headers={"X-Device-Id": "car1"})
    assert j["reply"] == "מקריא את הסיכום." and j["action"] == "hub:read_summary"
    cmds = hello(client, "car1", "car", ssid="Home").get_json()["commands"]
    assert cmds[0]["type"] == "read_summary" and cmds[0]["shift_id"] == "s1"
    assert say("read the summary")["reply"] == "Reading the summary."


def test_voice_regular_sentences_untouched(client, voice):
    say, state = voice
    j = say("מה השעה")
    assert j["reply"] == "LLM" and j["action"] == "face:happy" and state["llm_calls"] == 1


def test_voice_intents_ignored_without_token(client, voice, monkeypatch, clock):
    say, state = voice
    hello(client, "pocket1", "pocket")
    monkeypatch.setenv("BRIDGE_TOKEN", "secret")
    j = say("איפה הכיס שלי")  # בלי Authorization
    assert j["reply"] == "LLM" and state["llm_calls"] == 1
    j = say("איפה הכיס שלי", headers={"Authorization": "Bearer secret"})
    assert j["action"] == "hub:find_me"


# ---------------------------------------------------------------- טוקן


def test_token_required_everywhere(client, hubenv, monkeypatch):
    monkeypatch.setenv("BRIDGE_TOKEN", "secret")
    ok = {"Authorization": "Bearer secret"}
    bad = {"Authorization": "Bearer nope"}
    body = {"device_id": "car1", "role": "car"}
    assert client.post("/hub/hello", json=body).status_code == 401
    assert client.post("/hub/hello", json=body, headers=bad).status_code == 401
    assert client.post("/hub/hello", json=body, headers=ok).status_code == 200
    assert client.post("/hub/ack", json={"device_id": "car1", "command_id": "x"}).status_code == 401
    assert client.post("/hub/alert", json={"device_id": "car1", "type": "crash"}).status_code == 401
    assert client.get("/hub/state").status_code == 401
    assert client.get("/hub/state", headers=ok).status_code == 200


def test_state_endpoint(client, hubenv, clock):
    both_in_car(client, clock)
    j = client.get("/hub/state").get_json()
    assert j["in_car"] is True and j["car_ssid_configured"] is True
    assert set(j["devices"]) == {"car1", "pocket1"}
    assert j["car_session"]["active"] is True


def test_existing_endpoints_unaffected(client, hubenv):
    assert client.get("/health").get_json() == {"ok": True}
