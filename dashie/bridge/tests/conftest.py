import io
import json
import sys
import wave
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

ENV_KEYS = [
    "BRIDGE_TOKEN", "SMTP_HOST", "SMTP_PORT", "SMTP_USER", "SMTP_PASS", "SMTP_FROM", "SHIFT_EMAIL_TO",
    "GDRIVE_FOLDER_ID", "GOOGLE_SA_JSON", "ELEVENLABS_API_KEY", "ELEVENLABS_VOICE_ID", "ANTHROPIC_API_KEY",
]


def make_wav(n=1600) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(b"\x01\x00" * n)
    return buf.getvalue()


GOOD_JSON = {
    "summary_md": "## הלך המשמרת\nמשמרת עמוסה.\n\n## אירועים חריגים\nלא דווח\n\n## הצוות לפי עמדות\nלא דווח\n\n## איך נהגתי\nלא דווח\n\n## למחר\nלהזמין דג.",
    "spoken": "משמרת עמוסה. מחר להזמין דג.",
    "questions": [],
}


class Fakes:
    """אוסף ממוקים לשירותים החיצוניים, עם רישום קריאות."""

    def __init__(self):
        self.transcripts = ["הגריל איחר בעשר דקות", "מחר להזמין דג"]
        self.stt_calls = 0
        self.stt_fail = False
        self.llm_calls = []
        self.llm_responses = [json.dumps(GOOD_JSON, ensure_ascii=False)]
        self.emails = []
        self.email_fail = False
        self.docs = []
        self.docs_url = None
        self.tts_calls = []


@pytest.fixture
def env(tmp_path, monkeypatch):
    for k in ENV_KEYS:
        monkeypatch.delenv(k, raising=False)
    monkeypatch.setenv("SHIFTS_DIR", str(tmp_path / "shifts"))
    monkeypatch.setenv("PROFILE_DIR", str(tmp_path / "profile"))
    return tmp_path


@pytest.fixture
def fakes(env, monkeypatch):
    import shift

    f = Fakes()

    def stt(wav):
        f.stt_calls += 1
        if f.stt_fail:
            raise RuntimeError("stt down")
        return f.transcripts[(f.stt_calls - 1) % len(f.transcripts)]

    def llm(system, user):
        f.llm_calls.append((system, user))
        i = min(len(f.llm_calls) - 1, len(f.llm_responses) - 1)
        r = f.llm_responses[i]
        if isinstance(r, Exception):
            raise r
        return r

    def email(subject, text, html, md, filename):
        if f.email_fail:
            raise RuntimeError("smtp down")
        f.emails.append({"subject": subject, "text": text, "html": html, "md": md, "filename": filename})

    def drive(title, html):
        if f.docs_url is None:
            return None
        f.docs.append((title, html))
        return f.docs_url

    def tts(text):
        f.tts_calls.append(text)
        return make_wav(800)

    monkeypatch.setattr(shift, "transcribe_wav", stt)
    monkeypatch.setattr(shift, "call_llm", llm)
    monkeypatch.setattr(shift, "send_email", email)
    monkeypatch.setattr(shift, "drive_upload", drive)
    monkeypatch.setattr(shift, "tts_wav", tts)
    return f


@pytest.fixture
def client(env):
    import server

    server.app.config["TESTING"] = True
    return server.app.test_client()


def put_note(client, shift_id="s1", seq=1, t="2025-06-12T19:00:00", kind=None, headers=None, wav=None):
    h = {"X-Shift-Id": shift_id, "X-Seq": str(seq), "X-Time": t}
    if kind:
        h["X-Kind"] = kind
    h.update(headers or {})
    return client.post("/shift/note", data=wav or make_wav(), headers=h, content_type="audio/wav")


def start(client, shift_id="s1", headers=None):
    return client.post(
        "/shift/start",
        json={"shift_id": shift_id, "started_at": "2025-06-12T16:00:00"},
        headers=headers or {},
    )


def finish(client, shift_id="s1", headers=None, **extra):
    body = {"shift_id": shift_id, "ended_at": "2025-06-12T23:40:00"}
    body.update(extra)
    return client.post("/shift/finish", json=body, headers=headers or {})
