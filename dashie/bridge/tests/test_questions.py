"""GET /shift/question/<n>.wav: קול לשאלות הסיכום (מטמון, חתימה, שפות)."""
import re

import pytest

import shift
from conftest import make_wav


def test_question_is_spoken_and_cached(client, fakes):
    r = client.get("/shift/question/1.wav?lang=he")
    assert r.status_code == 200 and r.mimetype == "audio/wav" and r.data[:4] == b"RIFF"
    assert fakes.tts_calls == ["איך עבר הקו?"]
    r2 = client.get("/shift/question/1.wav?lang=he")          # מהמטמון: בלי TTS נוסף
    assert r2.status_code == 200 and r2.data == r.data
    assert len(fakes.tts_calls) == 1


def test_english_questions(client, fakes):
    assert client.get("/shift/question/4.wav?lang=en").status_code == 200
    assert fakes.tts_calls == ["How did I handle things?"]


def test_default_language_is_hebrew(client, fakes):
    client.get("/shift/question/2.wav")
    assert fakes.tts_calls == ["היו אירועים חריגים?"]


@pytest.mark.parametrize("path", ["/shift/question/0.wav", "/shift/question/5.wav", "/shift/question/1.wav?lang=fr"])
def test_bad_question_or_language(client, fakes, path):
    assert client.get(path).status_code == 404
    assert fakes.tts_calls == []


def test_no_tts_configured_gives_404(client, fakes, monkeypatch):
    monkeypatch.setattr(shift, "tts_wav", lambda text: b"")
    assert client.get("/shift/question/1.wav?lang=he").status_code == 404


def test_tts_failure_gives_502(client, fakes, monkeypatch):
    import requests

    def boom(text):
        raise requests.ConnectionError("down")

    monkeypatch.setattr(shift, "tts_wav", boom)
    assert client.get("/shift/question/1.wav?lang=he").status_code == 502


def test_signature_required_with_token(client, fakes, monkeypatch):
    monkeypatch.setenv("BRIDGE_TOKEN", "sekret")
    assert client.get("/shift/question/1.wav?lang=he").status_code == 401
    assert client.get("/shift/question/1.wav?lang=he&t=bad").status_code == 401
    sig = shift._sign("_q", "question_1_he.wav")
    assert client.get(f"/shift/question/1.wav?lang=he&t={sig}").status_code == 200
    # חתימה של שפה אחרת/שאלה אחרת לא תקפה
    assert client.get(f"/shift/question/2.wav?lang=he&t={sig}").status_code == 401
    assert client.get(f"/shift/question/1.wav?lang=en&t={sig}").status_code == 401


def test_bearer_header_also_accepted(client, fakes, monkeypatch):
    monkeypatch.setenv("BRIDGE_TOKEN", "sekret")
    assert client.get("/shift/question/1.wav?lang=he", headers={"Authorization": "Bearer sekret"}).status_code == 200
    assert client.get("/shift/question/1.wav?lang=he", headers={"Authorization": "Bearer nope"}).status_code == 401


def test_question_url_helper_matches_endpoint(client, fakes, monkeypatch):
    monkeypatch.setenv("BRIDGE_TOKEN", "sekret")
    with client.application.test_request_context("/", base_url="http://bridge:8787"):
        url = shift.question_url(3, "he")
    assert url.startswith("http://bridge:8787/shift/question/3.wav?lang=he&t=")
    path = url.replace("http://bridge:8787", "")
    assert client.get(path).status_code == 200


def test_question_texts_match_firmware_strings():
    src = open("../firmware/src/strings.h", encoding="utf-8").read()
    for lang, idx in (("en", 0), ("he", 1)):
        for n, q in enumerate(shift.WRAPUP_QUESTIONS[lang], start=1):
            m = re.search(r"/\* S_Q%d \*/ \{\"([^\"]*)\", \"([^\"]*)\"\}" % n, src)
            assert m and m.group(idx + 1) == q, (lang, n)
