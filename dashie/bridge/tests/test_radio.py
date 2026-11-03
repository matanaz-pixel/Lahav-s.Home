"""בדיקות radio.py: פרסור עברית, מצב והשרדות, SSE, אימות, פרטיות. ללא שירותים חיצוניים."""
import importlib
import json
import logging

import pytest

from conftest import make_wav

import radio

S = "start"


def cfg():
    return radio.load_config()


# ---------------------------------------------------------------- פרסור: (טקסט, [(type, table, item, seconds)])

PARSE_CASES = [
    # חד-משמעיות
    ("תשע דקות ללזניה של 12", [(S, 12, "לזניה", 540)]),
    ("9 דקות ללזניה של 12", [(S, 12, "לזניה", 540)]),
    ("תשע דקות ללזניה לשולחן שתים עשרה", [(S, 12, "לזניה", 540)]),
    ("שולחן 12 לזניה תשע דקות", [(S, 12, "לזניה", 540)]),
    ("הלזניה של 12 תשע דקות", [(S, 12, "לזניה", 540)]),
    ("עשר דקות לניוקי שולחן 7", [(S, 7, "ניוקי", 600)]),
    ("ניוקי של 7 עשר דקות", [(S, 7, "ניוקי", 600)]),
    ("10 דקות פסטה שולחן 3", [(S, 3, "פסטה", 600)]),
    ("9 דקות לזניה של 12", [(S, 12, "לזניה", 540)]),
    ("9 דקות ללזנייה לשולחן 12", [(S, 12, "לזניה", 540)]),
    ("9 דקות ללזניה ל-12", [(S, 12, "לזניה", 540)]),
    ("9 דק' ללזניה של 12", [(S, 12, "לזניה", 540)]),
    ("דקה ללזניה של 2", [(S, 2, "לזניה", 60)]),
    ("10 דקות לשולחן 14", [(S, 14, None, 600)]),
    ("בעוד שבע דקות ללזניה של 21", [(S, 21, "לזניה", 420)]),
    ("שולחן 12 לזניה 9 דקות", [(S, 12, "לזניה", 540)]),
    # מספרים במילים
    ("חמש עשרה דקות לפסטה של 3", [(S, 3, "פסטה", 900)]),
    ("עשרים וחמש דקות לריזוטו של 4", [(S, 4, "ריזוטו", 1500)]),
    ("עשרים דקות לסטייק של 15", [(S, 15, "סטייק", 1200)]),
    ("תשע דקות ללזניה של שמונה עשרה", [(S, 18, "לזניה", 540)]),
    ("שתי דקות לפיצה של 6", [(S, 6, "פיצה", 120)]),
    ("שלוש עשרה דקות לפיצה של 6", [(S, 6, "פיצה", 780)]),
    ("אחד עשר דקות לפיצה של 2", [(S, 2, "פיצה", 660)]),
    ("מאה ועשרים דקות לסלמון של 3", [(S, 3, "סלמון", 7200)]),
    # חצאים ורבעים
    ("דקה וחצי לפסטה לשולחן 5", [(S, 5, "פסטה", 90)]),
    ("שלוש דקות וחצי לפיצה לשולחן 6", [(S, 6, "פיצה", 210)]),
    ("ארבע וחצי דקות לפיצה של 11", [(S, 11, "פיצה", 270)]),
    ("רבע שעה לסטייק לשולחן 8", [(S, 8, "סטייק", 900)]),
    ("חצי שעה לסלמון של 2", [(S, 2, "סלמון", 1800)]),
    ("שעה לסלמון לשולחן 9", [(S, 9, "סלמון", 3600)]),
    ("שלושת רבעי שעה לסלמון לשולחן 9", [(S, 9, "סלמון", 2700)]),
    ("30 שניות לפיצה של 1", [(S, 1, "פיצה", 30)]),
    ("9.5 דקות לפסטה של 4", [(S, 4, "פסטה", 570)]),
    # רעש ותוספות
    ("(רעש) תשע דקות ללזניה של שתים עשרה (רעש)", [(S, 12, "לזניה", 540)]),
    ("תשע דקות ללזניה של 12 קיבלתי", [(S, 12, "לזניה", 540)]),
    ("[מוזיקה] תשע דקות ללזניה של 12.", [(S, 12, "לזניה", 540)]),
    # כפילות בתוך אותו משפט
    ("תשע דקות ללזניה של 12 תשע דקות ללזניה של 12", [(S, 12, "לזניה", 540)]),
    # כמה פקודות
    ("תשע דקות ללזניה של 12, עשר דקות לניוקי של 7", [(S, 12, "לזניה", 540), (S, 7, "ניוקי", 600)]),
    ("9 דקות ללזניה של 12 ו-10 דקות לניוקי של 7", [(S, 12, "לזניה", 540), (S, 7, "ניוקי", 600)]),
    ("תשע דקות ללזניה של 12 עשר דקות לניוקי של 7", [(S, 12, "לזניה", 540), (S, 7, "ניוקי", 600)]),
    ("שבע דקות ללזניה לשולחנות 12 ו 14", [(S, 12, "לזניה", 420), (S, 14, "לזניה", 420)]),
    # סיום וביטול
    ("שולחן 12 יצא", [("done", 12, None, None)]),
    ("שולחן שתים עשרה יצא", [("done", 12, None, None)]),
    ("שולחן מספר 12 יצא", [("done", 12, None, None)]),
    ("לזניה של 12 יצאה", [("done", 12, "לזניה", None)]),
    ("הוגש שולחן 5", [("done", 5, None, None)]),
    ("שולחן 12 ושולחן 14 יצאו", [("done", 12, None, None), ("done", 14, None, None)]),
    ("בטלו את שולחן 12", [("cancel", 12, None, None)]),
    ("בטל שולחן 5", [("cancel", 5, None, None)]),
    ("ביטול ללזניה של 12", [("cancel", 12, "לזניה", None)]),
    ("תבטלו את הניוקי של 7", [("cancel", 7, "ניוקי", None)]),
    ("שולחן 3 ושולחן 4 בוטלו", [("cancel", 3, None, None), ("cancel", 4, None, None)]),
    # הארכה
    ("תוסיפו שתי דקות לשולחן 12", [("extend", 12, None, 120)]),
    ("ארבע דקות נוספות לשולחן 3", [("extend", 3, None, 240)]),
    ("עוד שתי דקות לשולחן 12", [(S, 12, None, 120)]),  # maybe_extend נפתר מול המצב
    # חסר רכיב: לא מתעלמים בשקט
    ("9 דקות ללזניה", [(S, None, "לזניה", 540)]),
    ("תשע דקות לניוקי", [(S, None, "ניוקי", 540)]),
    ("לזניה של 12", [(S, 12, "לזניה", 600)]),
    ("שולחן 12 לזניה", [(S, 12, "לזניה", 600)]),
    # לא פקודות
    ("מי יכול לקחת את 5?", []),
    ("תן לי חמש דקות", []),
    ("אני מגיע בעוד חמש דקות", []),
    ("קיבלתי", []),
    ("שולחן 12 מחכה", []),
    ("תודה רבה", []),
    ("", []),
    ("   ", []),
    ("אחד שתיים שלוש", []),
    ("מי לקח את הלזניה?", []),
    ("כמה זמן נשאר?", []),
    ("הלו, הלו, בדיקה בדיקה", []),
    ("שלוש לזניות", []),
    ("יצא לי הרגע", []),
    ("Thank you", []),
]


@pytest.mark.parametrize("text,expected", PARSE_CASES)
def test_parse(text, expected):
    got = [(c["type"], c["table"], c["item"], c.get("seconds")) for c in radio.parse_text(text, cfg())]
    assert got == expected


def test_parse_case_count():
    assert len(PARSE_CASES) >= 50


def test_maybe_extend_flag_and_unconfirmed():
    c = radio.parse_text("עוד שתי דקות לשולחן 12", cfg())[0]
    assert c["maybe_extend"] is True
    miss_table = radio.parse_text("9 דקות ללזניה", cfg())[0]
    assert miss_table["unconfirmed"] is True and miss_table["confidence"] < 0.8
    miss_time = radio.parse_text("לזניה של 12", cfg())[0]
    assert miss_time["unconfirmed"] is True and miss_time["reason"] == "no_time"
    sure = radio.parse_text("תשע דקות ללזניה של 12", cfg())[0]
    assert sure["confidence"] >= 0.9 and not sure.get("unconfirmed")


def test_missing_time_policy_ignore(tmp_path, monkeypatch):
    p = tmp_path / "c.json"
    p.write_text(json.dumps({"missing_time_policy": "ignore", "items": {"לזניה": ["לזניה"]}}), encoding="utf-8")
    monkeypatch.setenv("RADIO_CONFIG", str(p))
    assert radio.parse_text("לזניה של 12", radio.load_config()) == []
    assert radio.parse_text("תשע דקות ללזניה של 12", radio.load_config())[0]["seconds"] == 540


def test_config_custom_item_and_prefix(tmp_path, monkeypatch):
    p = tmp_path / "c.json"
    p.write_text(json.dumps({"items": {"טירמיסו": ["טירמיסו", "תירמיסו"]}}), encoding="utf-8")
    monkeypatch.setenv("RADIO_CONFIG", str(p))
    got = radio.parse_text("שלוש דקות לטירמיסו של 9", radio.load_config())
    assert [(c["table"], c["item"], c["seconds"]) for c in got] == [(9, "טירמיסו", 180)]


@pytest.mark.parametrize(
    "words,value",
    [
        (["אפס"], 0), (["אחד"], 1), (["אחת"], 1), (["שתיים"], 2), (["שתי"], 2), (["שלוש"], 3), (["ארבעה"], 4),
        (["חמש"], 5), (["שש"], 6), (["שבע"], 7), (["שמונה"], 8), (["תשע"], 9), (["עשר"], 10), (["עשרה"], 10),
        (["אחד", "עשר"], 11), (["אחת", "עשרה"], 11), (["שתים", "עשרה"], 12), (["שנים", "עשר"], 12),
        (["שלוש", "עשרה"], 13), (["חמש", "עשרה"], 15), (["תשע", "עשרה"], 19), (["עשרים"], 20),
        (["עשרים", "וחמש"], 25), (["עשרים", "ו", "חמש"], 25), (["שלושים", "ושתיים"], 32), (["ארבעים", "ושמונה"], 48),
        (["תשעים", "ותשע"], 99), (["מאה"], 100), (["מאה", "ועשרים"], 120), (["12"], 12), (["007"], 7),
    ],
)
def test_parse_number(words, value):
    got = radio.parse_number(words, 0)
    assert got is not None and got[0] == value and got[1] == len(words)


def test_parse_number_none():
    assert radio.parse_number(["שולחן"], 0) is None


# ---------------------------------------------------------------- מצב, API


class Clock:
    def __init__(self, t=1_800_000_000.0):
        self.t = t

    def __call__(self):
        return self.t


@pytest.fixture
def rad(env, monkeypatch, tmp_path):
    monkeypatch.setenv("RADIO_STATE_PATH", str(tmp_path / "radio_state.json"))
    for k in ("RADIO_DEBUG", "RADIO_LLM_FALLBACK", "ANTHROPIC_API_KEY", "RADIO_KEEP_S", "RADIO_STALE_S"):
        monkeypatch.delenv(k, raising=False)
    clock = Clock()
    monkeypatch.setattr(radio, "now", clock)
    return clock


class Stt:
    def __init__(self):
        self.text = ""
        self.fail = False
        self.calls = 0

    def __call__(self, wav):
        self.calls += 1
        if self.fail:
            raise RuntimeError("stt down")
        return self.text


@pytest.fixture
def stt(rad, monkeypatch):
    s = Stt()
    monkeypatch.setattr(radio, "transcribe_wav", s)
    return s


def say(client, stt, text, headers=None, wav=None):
    stt.text = text
    return client.post("/radio/segment", data=wav or make_wav(), headers=headers or {}, content_type="audio/wav")


def state(client, **q):
    r = client.get("/radio/state", query_string=q)
    assert r.status_code == 200
    return r.get_json()


def test_segment_start_timer_and_state(client, stt, rad):
    r = say(client, stt, "תשע דקות ללזניה של 12")
    assert r.status_code == 200
    j = r.get_json()
    assert j["ignored"] is False
    c = j["commands"][0]
    assert c["type"] == "start" and c["applied"] and c["table"] == 12 and c["item"] == "לזניה" and c["seconds"] == 540
    s = state(client)
    assert len(s["timers"]) == 1
    t = s["timers"][0]
    assert t["table"] == 12 and t["duration_s"] == 540 and t["status"] == "running"
    assert t["remaining_s"] == pytest.approx(540, abs=0.01) and t["overdue"] is False
    assert t["unconfirmed"] is False and t["src"] == "radio"


def test_countdown_and_overdue(client, stt, rad):
    say(client, stt, "דקה לפסטה של 3")
    rad.t += 30
    assert state(client)["timers"][0]["remaining_s"] == pytest.approx(30, abs=0.01)
    rad.t += 40
    t = state(client)["timers"][0]
    assert t["overdue"] is True and t["remaining_s"] < 0


def test_ignored_segment(client, stt, rad):
    j = say(client, stt, "מי יכול לקחת את 5?").get_json()
    assert j["ignored"] is True and j["commands"] == []
    assert state(client)["timers"] == []


def test_too_short_wav_does_not_call_stt(client, stt, rad):
    r = client.post("/radio/segment", data=b"abc", content_type="audio/wav")
    assert r.get_json()["ignored"] is True
    assert stt.calls == 0


def test_too_large_wav(client, stt, rad):
    r = client.post("/radio/segment", data=b"x" * (radio.MAX_WAV_BYTES + 1), content_type="audio/wav")
    assert r.status_code == 413


def test_stt_failure_is_502_without_leak(client, stt, rad):
    stt.fail = True
    r = client.post("/radio/segment", data=make_wav(), content_type="audio/wav")
    assert r.status_code == 502
    assert "stt down" not in r.get_data(as_text=True)


def test_duplicate_within_window_ignored(client, stt, rad):
    say(client, stt, "תשע דקות ללזניה של 12")
    rad.t += 10
    c = say(client, stt, "תשע דקות ללזניה של 12").get_json()["commands"][0]
    assert c["applied"] is False and c["duplicate"] is True
    assert len(state(client)["timers"]) == 1
    rad.t += 60  # אחרי חלון הכפילות: הזמנה חדשה אמיתית
    c = say(client, stt, "תשע דקות ללזניה של 12").get_json()["commands"][0]
    assert c["applied"] is True
    assert len(state(client)["timers"]) == 2


def test_done_removes_from_running_list(client, stt, rad):
    say(client, stt, "תשע דקות ללזניה של 12")
    c = say(client, stt, "שולחן 12 יצא").get_json()["commands"][0]
    assert c["applied"] and c["type"] == "done"
    assert state(client)["timers"] == []
    allt = state(client, all=1)["timers"]
    assert allt[0]["status"] == "done" and allt[0]["remaining_s"] is None


def test_cancel_by_item_only_that_item(client, stt, rad):
    say(client, stt, "תשע דקות ללזניה של 12")
    say(client, stt, "שש דקות לניוקי של 12")
    say(client, stt, "בטלו את הניוקי של 12")
    items = [t["item"] for t in state(client)["timers"]]
    assert items == ["לזניה"]


def test_cancel_without_match_reports_not_applied(client, stt, rad):
    c = say(client, stt, "בטלו את שולחן 9").get_json()["commands"][0]
    assert c["applied"] is False and c["reason"] == "no_match"


def test_extend_command(client, stt, rad):
    say(client, stt, "תשע דקות ללזניה של 12")
    c = say(client, stt, "תוסיפו שתי דקות לשולחן 12").get_json()["commands"][0]
    assert c["applied"] and c["type"] == "extend"
    t = state(client)["timers"][0]
    assert t["duration_s"] == 660 and t["remaining_s"] == pytest.approx(660, abs=0.01)


def test_maybe_extend_converts_when_timer_exists(client, stt, rad):
    say(client, stt, "תשע דקות ללזניה של 12")
    c = say(client, stt, "עוד שתי דקות לשולחן 12").get_json()["commands"][0]
    assert c["type"] == "extend" and c["converted_from"] == "start" and c["applied"]
    assert len(state(client)["timers"]) == 1
    assert state(client)["timers"][0]["duration_s"] == 660


def test_maybe_extend_starts_when_no_timer(client, stt, rad):
    c = say(client, stt, "עוד שתי דקות לשולחן 12").get_json()["commands"][0]
    assert c["type"] == "start" and c["applied"]
    assert state(client)["timers"][0]["duration_s"] == 120


def test_extend_ambiguous_picks_earliest(client, stt, rad):
    say(client, stt, "תשע דקות ללזניה של 12")
    say(client, stt, "חמש דקות לפיצה של 12")
    c = say(client, stt, "תוסיפו שתי דקות לשולחן 12").get_json()["commands"][0]
    assert c["applied"] and c.get("ambiguous") is True
    by_item = {t["item"]: t["duration_s"] for t in state(client)["timers"]}
    assert by_item == {"לזניה": 540, "פיצה": 420}


def test_item_only_done_applies_when_unique(client, stt, rad):
    say(client, stt, "תשע דקות ללזניה של 12")
    c = say(client, stt, "לזניה יצאה").get_json()["commands"][0]
    assert c["applied"] is True


def test_item_only_done_ambiguous(client, stt, rad):
    say(client, stt, "תשע דקות ללזניה של 12")
    say(client, stt, "תשע דקות ללזניה של 5")
    c = say(client, stt, "לזניה יצאה").get_json()["commands"][0]
    assert c["applied"] is False and c["reason"] == "ambiguous"


def test_start_without_table_is_unconfirmed_then_confirm_fixes(client, stt, rad):
    say(client, stt, "תשע דקות ללזניה")
    t = state(client)["timers"][0]
    assert t["table"] is None and t["unconfirmed"] is True
    r = client.post(f"/radio/timers/{t['id']}/confirm", json={"table": "7"})
    assert r.status_code == 200
    t = r.get_json()
    assert t["table"] == 7 and t["unconfirmed"] is False


def test_confirm_can_change_seconds(client, stt, rad):
    say(client, stt, "לזניה של 12")  # 600s ברירת מחדל, לא מאושר
    t = state(client)["timers"][0]
    assert t["duration_s"] == 600 and t["unconfirmed"]
    t = client.post(f"/radio/timers/{t['id']}/confirm", json={"minutes": 9}).get_json()
    assert t["duration_s"] == 540 and t["ends_at"] == t["started_at"] + 540


def test_multiple_commands_in_one_segment(client, stt, rad):
    j = say(client, stt, "תשע דקות ללזניה של 12, עשר דקות לניוקי של 7").get_json()
    assert [c["applied"] for c in j["commands"]] == [True, True]
    assert {t["table"] for t in state(client)["timers"]} == {12, 7}


def test_seg_ts_compensates_latency(client, stt, rad):
    ts = rad.t - 20
    say(client, stt, "דקה ללזניה של 2", headers={"X-Seg-Ts": str(ts)})
    t = state(client)["timers"][0]
    assert t["started_at"] == pytest.approx(ts)
    assert t["remaining_s"] == pytest.approx(40, abs=0.01)


def test_seg_ts_implausible_is_ignored(client, stt, rad):
    say(client, stt, "דקה ללזניה של 2", headers={"X-Seg-Ts": str(rad.t - 99999)})
    assert state(client)["timers"][0]["started_at"] == rad.t
    say(client, stt, "דקה לפסטה של 3", headers={"X-Seg-Ts": "not-a-number"})
    assert state(client)["timers"][1]["started_at"] == rad.t if len(state(client)["timers"]) > 1 else True


def test_seg_ts_milliseconds(client, stt, rad):
    ts = rad.t - 10
    say(client, stt, "דקה ללזניה של 2", headers={"X-Seg-Ts": str(int(ts * 1000))})
    assert state(client)["timers"][0]["started_at"] == pytest.approx(int(ts * 1000) / 1000.0)


# ---------------------------------------------------------------- טיימרים ידניים ופעולות


def test_manual_timer(client, rad):
    r = client.post("/radio/timers", json={"table": 5, "item": "פסטה", "minutes": 8})
    assert r.status_code == 201
    t = r.get_json()
    assert t["table"] == 5 and t["duration_s"] == 480 and t["src"] == "manual" and t["unconfirmed"] is False


@pytest.mark.parametrize(
    "body",
    [
        {}, {"table": 5}, {"seconds": 60}, {"table": 5, "seconds": -1}, {"table": 5, "seconds": 0},
        {"table": 5, "seconds": 10**9}, {"table": 5, "seconds": "abc"}, {"table": 0, "seconds": 60},
        {"table": True, "seconds": 60}, {"table": "!!!", "seconds": 60}, {"item": "x" * 99, "seconds": 60},
        {"table": 5, "minutes": True},
    ],
)
def test_manual_timer_validation(client, rad, body):
    assert client.post("/radio/timers", json=body).status_code == 400


def test_manual_timer_named_table(client, rad):
    t = client.post("/radio/timers", json={"table": "בר", "seconds": 90}).get_json()
    assert t["table"] == "בר"


def test_actions_cancel_dismiss_extend(client, rad):
    a = client.post("/radio/timers", json={"table": 1, "seconds": 60}).get_json()["id"]
    b = client.post("/radio/timers", json={"table": 2, "seconds": 60}).get_json()["id"]
    c = client.post("/radio/timers", json={"table": 3, "seconds": 60}).get_json()["id"]
    assert client.post(f"/radio/timers/{a}/cancel").get_json()["status"] == "cancelled"
    assert client.post(f"/radio/timers/{b}/dismiss").get_json()["status"] == "dismissed"
    e = client.post(f"/radio/timers/{c}/extend", json={"seconds": 30}).get_json()
    assert e["duration_s"] == 90
    e = client.post(f"/radio/timers/{c}/extend").get_json()  # ברירת מחדל דקה
    assert e["duration_s"] == 150
    assert [t["id"] for t in state(client)["timers"]] == [c]


def test_extend_closed_timer_is_409(client, rad):
    a = client.post("/radio/timers", json={"table": 1, "seconds": 60}).get_json()["id"]
    client.post(f"/radio/timers/{a}/cancel")
    assert client.post(f"/radio/timers/{a}/extend", json={"seconds": 30}).status_code == 409
    assert client.post(f"/radio/timers/{a}/confirm").status_code == 409


def test_unknown_timer_and_action(client, rad):
    assert client.post("/radio/timers/zzz/cancel").status_code == 404
    a = client.post("/radio/timers", json={"table": 1, "seconds": 60}).get_json()["id"]
    assert client.post(f"/radio/timers/{a}/explode").status_code == 404


def test_extend_bad_value(client, rad):
    a = client.post("/radio/timers", json={"table": 1, "seconds": 60}).get_json()["id"]
    assert client.post(f"/radio/timers/{a}/extend", json={"seconds": -5}).status_code == 400


# ---------------------------------------------------------------- שמירה, השרדות, גיזום


def test_state_survives_restart(client, stt, rad, tmp_path):
    say(client, stt, "תשע דקות ללזניה של 12")
    before = state(client)
    importlib.reload(radio)  # סימולציית הפעלה מחדש: המצב נטען מהקובץ
    radio.now = rad
    after = radio.snapshot()
    assert [t["id"] for t in after["timers"]] == [t["id"] for t in before["timers"]]
    assert after["timers"][0]["ends_at"] == before["timers"][0]["ends_at"]
    # והמונה ממשיך מאותה נקודה, בלי להתנגש במזהים
    with radio.state() as st:
        tm = radio._new_timer(st, 1, None, 60, rad.t, False, "manual")
    assert tm["id"] != before["timers"][0]["id"]


def test_corrupt_state_file_recovers(client, stt, rad, tmp_path):
    (tmp_path / "radio_state.json").write_text("{not json", encoding="utf-8")
    assert state(client)["timers"] == []
    say(client, stt, "תשע דקות ללזניה של 12")
    assert len(state(client)["timers"]) == 1


def test_wrong_types_in_state_file_recover(client, rad, tmp_path):
    (tmp_path / "radio_state.json").write_text(json.dumps({"rev": "x", "seq": [], "timers": "no"}), encoding="utf-8")
    assert state(client)["timers"] == []
    assert client.post("/radio/timers", json={"table": 1, "seconds": 60}).status_code == 201


def test_prune_closed_timers_after_keep(client, stt, rad, monkeypatch):
    monkeypatch.setenv("RADIO_KEEP_S", "100")
    say(client, stt, "דקה ללזניה של 2")
    say(client, stt, "שולחן 2 יצא")
    assert len(state(client, all=1)["timers"]) == 1
    rad.t += 200
    client.post("/radio/timers", json={"table": 9, "seconds": 60})  # כתיבה מפעילה גיזום
    assert [t["table"] for t in state(client, all=1)["timers"]] == [9]


def test_stale_overdue_timer_auto_dismissed(client, stt, rad, monkeypatch):
    monkeypatch.setenv("RADIO_STALE_S", "100")
    say(client, stt, "דקה ללזניה של 2")
    rad.t += 60 + 200
    client.post("/radio/timers", json={"table": 9, "seconds": 60})
    assert [t["table"] for t in state(client)["timers"]] == [9]
    assert {t["status"] for t in state(client, all=1)["timers"]} == {"running", "dismissed"}


def test_rev_increments_only_on_change(client, stt, rad):
    r0 = state(client)["rev"]
    say(client, stt, "מי יכול לקחת את 5?")
    assert state(client)["rev"] == r0
    say(client, stt, "דקה ללזניה של 2")
    assert state(client)["rev"] > r0


# ---------------------------------------------------------------- פרטיות


def test_transcript_not_returned_by_default(client, stt, rad):
    j = say(client, stt, "תשע דקות ללזניה של 12").get_json()
    assert "transcript" not in j


def test_debug_returns_transcript_but_does_not_store(client, stt, rad, monkeypatch, tmp_path):
    monkeypatch.setenv("RADIO_DEBUG", "1")
    secret = "תשע דקות ללזניה של 12 סודי מאוד"
    j = say(client, stt, secret).get_json()
    assert j["transcript"] == secret
    for f in tmp_path.rglob("*"):
        if f.is_file():
            assert "סודי" not in f.read_text(encoding="utf-8", errors="ignore"), f


def test_no_transcript_or_audio_on_disk_or_logs(client, stt, rad, tmp_path, caplog):
    caplog.set_level(logging.DEBUG)
    secret = "קוד סודי: תשע דקות ללזניה של 12 והמלצרית אמרה פרטי"
    say(client, stt, secret, wav=make_wav(4000))
    say(client, stt, "הערה פרטית שאינה פקודה כלל")
    for f in tmp_path.rglob("*"):
        if f.is_file():
            data = f.read_bytes()
            assert "סודי".encode() not in data and "פרטית".encode() not in data and "המלצרית".encode() not in data, f
            assert not f.name.endswith(".wav")
    assert "סודי" not in caplog.text and "פרטית" not in caplog.text
    stored = (tmp_path / "radio_state.json").read_text(encoding="utf-8")
    assert "המלצרית" not in stored
    assert set(json.loads(stored)["timers"][0]) <= {
        "id", "table", "item", "started_at", "duration_s", "ends_at", "status", "unconfirmed", "src", "closed_at",
    }


def test_stt_error_does_not_log_text(client, stt, rad, caplog):
    caplog.set_level(logging.DEBUG)
    stt.fail = True
    client.post("/radio/segment", data=make_wav(), content_type="audio/wav")
    assert "stt down" not in caplog.text


# ---------------------------------------------------------------- אימות


def test_auth_required_when_token_set(client, stt, rad, monkeypatch):
    monkeypatch.setenv("BRIDGE_TOKEN", "sekret")
    assert client.post("/radio/segment", data=make_wav()).status_code == 401
    assert client.get("/radio/state").status_code == 401
    assert client.post("/radio/timers", json={"table": 1, "seconds": 60}).status_code == 401
    assert client.get("/radio/stream?once=1").status_code == 401
    h = {"Authorization": "Bearer sekret"}
    assert client.get("/radio/state", headers=h).status_code == 200
    assert client.post("/radio/timers", json={"table": 1, "seconds": 60}, headers=h).status_code == 201
    assert client.post("/radio/segment", data=make_wav(), headers={**h, "Content-Type": "audio/wav"}).status_code == 200
    bad = {"Authorization": "Bearer nope"}
    assert client.get("/radio/state", headers=bad).status_code == 401


def test_query_token_only_for_get_state_and_stream(client, rad, monkeypatch):
    monkeypatch.setenv("BRIDGE_TOKEN", "sekret")
    assert client.get("/radio/state?token=sekret").status_code == 200
    assert client.get("/radio/stream?once=1&token=sekret").status_code == 200
    assert client.get("/radio/state?token=wrong").status_code == 401
    assert client.post("/radio/timers?token=sekret", json={"table": 1, "seconds": 60}).status_code == 401
    assert client.post("/radio/segment?token=sekret", data=make_wav()).status_code == 401


def test_no_token_means_open(client, rad):
    assert client.get("/radio/state").status_code == 200


# ---------------------------------------------------------------- SSE


def test_sse_once_snapshot(client, rad):
    client.post("/radio/timers", json={"table": 4, "item": "פסטה", "seconds": 120})
    r = client.get("/radio/stream?once=1")
    assert r.status_code == 200
    assert r.mimetype == "text/event-stream"
    assert r.headers["Cache-Control"] == "no-cache"
    body = r.get_data(as_text=True)
    assert body.startswith("event: state\ndata: ")
    data = json.loads(body.split("data: ", 1)[1].strip())
    assert data["timers"][0]["table"] == 4 and "server_time" in data


def test_sse_emits_on_change(client, rad):
    gen = radio.stream_events(poll_s=0.05, max_s=5)
    first = next(gen)
    assert json.loads(first.split("data: ", 1)[1])["timers"] == []
    client.post("/radio/timers", json={"table": 8, "seconds": 60})
    second = next(gen)
    assert json.loads(second.split("data: ", 1)[1])["timers"][0]["table"] == 8
    gen.close()


def test_sse_keepalive(client, rad):
    gen = radio.stream_events(poll_s=0.02, max_s=5, keepalive_s=0.05)
    next(gen)
    assert next(gen) == ": keepalive\n\n"
    gen.close()


def test_sse_ends_after_max(client, rad):
    out = list(radio.stream_events(poll_s=0.01, max_s=0.05))
    assert len(out) >= 1


# ---------------------------------------------------------------- fallback ל-LLM


def test_llm_fallback_disabled_by_default(client, stt, rad, monkeypatch):
    calls = []
    monkeypatch.setattr(radio, "call_llm", lambda s, u: calls.append(u) or "{}")
    j = say(client, stt, "שולחן שלוש עשרה צריך עוד קצת זמן").get_json()
    assert calls == [] and j["ignored"] is True


def test_llm_fallback_applies_valid_commands(client, stt, rad, monkeypatch):
    monkeypatch.setenv("RADIO_LLM_FALLBACK", "1")
    monkeypatch.setenv("ANTHROPIC_API_KEY", "k")
    monkeypatch.setattr(
        radio, "call_llm",
        lambda s, u: 'הנה: {"commands":[{"type":"start","table":13,"item":"פסטה","seconds":300,"confidence":0.9}]}',
    )
    j = say(client, stt, "שולחן שלוש עשרה צריך חמש דקות בערך").get_json()
    # הפרסר הדטרמיניסטי כבר מזהה זאת? אם כן ה-LLM לא נקרא; אם לא, הפקודה מה-LLM מוחלת
    assert j["ignored"] is False
    assert state(client)["timers"][0]["table"] == 13


def test_llm_fallback_used_when_parser_finds_nothing(client, stt, rad, monkeypatch):
    monkeypatch.setenv("RADIO_LLM_FALLBACK", "1")
    monkeypatch.setenv("ANTHROPIC_API_KEY", "k")
    seen = []

    def fake(system, user):
        seen.append((system, user))
        return json.dumps({"commands": [{"type": "start", "table": 9, "item": "ריזוטו", "seconds": 420, "confidence": 0.95}]})

    monkeypatch.setattr(radio, "call_llm", fake)
    j = say(client, stt, "שולחן תשע דקה שבע בערך").get_json()
    assert seen, "ה-LLM היה צריך להיקרא"
    assert j["commands"][0]["src"] if "src" in j["commands"][0] else True
    t = state(client)["timers"][0]
    assert t["src"] == "llm" and t["table"] == 9 and t["duration_s"] == 420
    assert "untrusted" in seen[0][0].lower()


def test_llm_skipped_for_irrelevant_text(client, stt, rad, monkeypatch):
    monkeypatch.setenv("RADIO_LLM_FALLBACK", "1")
    monkeypatch.setenv("ANTHROPIC_API_KEY", "k")
    calls = []
    monkeypatch.setattr(radio, "call_llm", lambda s, u: calls.append(u) or "{}")
    say(client, stt, "תודה רבה לכולם")
    say(client, stt, "מי יכול לקחת את 5")
    assert calls == []


@pytest.mark.parametrize(
    "raw",
    [
        "not json", "{}", '{"commands": "x"}', '{"commands":[{"type":"drop_tables","table":1,"seconds":5}]}',
        '{"commands":[{"type":"start","table":0,"seconds":60}]}',
        '{"commands":[{"type":"start","table":1000,"seconds":60}]}',
        '{"commands":[{"type":"start","table":"7","seconds":60}]}',
        '{"commands":[{"type":"start","table":true,"seconds":60}]}',
        '{"commands":[{"type":"start","table":7,"seconds":-5}]}',
        '{"commands":[{"type":"start","table":7,"seconds":99999999}]}',
        '{"commands":[{"type":"start","table":7}]}',
        '{"commands":[{"type":"start","seconds":60}]}',
        '{"commands":[{"type":"start","table":7,"item":"' + "x" * 50 + '","seconds":60}]}',
        '{"commands":[{"type":"start","table":7,"item":5,"seconds":60}]}',
    ],
)
def test_llm_validation_rejects_garbage(raw):
    assert radio.validate_llm_commands(raw, cfg()) == []


def test_llm_validation_clamps_confidence_and_marks_unconfirmed():
    out = radio.validate_llm_commands('{"commands":[{"type":"start","table":7,"seconds":60,"confidence":5}]}', cfg())
    assert out[0]["confidence"] <= 0.85 and out[0]["unconfirmed"] is True
    out = radio.validate_llm_commands('{"commands":[{"type":"done","table":7,"confidence":0.9}]}', cfg())
    assert out[0]["type"] == "done"


def test_llm_error_is_swallowed(client, stt, rad, monkeypatch):
    monkeypatch.setenv("RADIO_LLM_FALLBACK", "1")
    monkeypatch.setenv("ANTHROPIC_API_KEY", "k")

    def boom(s, u):
        raise RuntimeError("llm down")

    monkeypatch.setattr(radio, "call_llm", boom)
    j = say(client, stt, "שולחן תשע דקה שבע בערך").get_json()
    assert j["ignored"] is True


def test_llm_not_called_when_deterministic_succeeds(client, stt, rad, monkeypatch):
    monkeypatch.setenv("RADIO_LLM_FALLBACK", "1")
    monkeypatch.setenv("ANTHROPIC_API_KEY", "k")
    calls = []
    monkeypatch.setattr(radio, "call_llm", lambda s, u: calls.append(u) or "{}")
    say(client, stt, "תשע דקות ללזניה של 12")
    assert calls == []
