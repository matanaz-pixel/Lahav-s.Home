import json
import smtplib

import pytest

import shift
from conftest import GOOD_JSON, finish, make_wav, put_note, start


# ---------------------------------------------------------------- זרימה מלאה


def test_full_flow(client, fakes, env):
    fakes.email_fail = False
    fakes.docs_url = "https://docs.google.com/document/d/abc/edit"
    assert start(client).get_json()["status"] == "ok"
    r1 = put_note(client, seq=1).get_json()
    assert r1 == {"status": "saved", "seq": 1, "transcribed": True}
    put_note(client, seq=2)

    r = finish(client)
    assert r.status_code == 200
    j = r.get_json()
    assert j["status"] == "done"
    assert j["summary_md"].startswith("# סיכום משמרת — יום חמישי, 12/06/2025, 16:00–23:40")
    for sec in shift.SECTIONS:
        assert f"## {sec}" in j["summary_md"]
    assert "## גרסה מוקראת" in j["summary_md"]
    assert j["delivered"]["email"] is True
    assert j["delivered"]["docs"] == "https://docs.google.com/document/d/abc/edit"
    assert j["audio_url"] and j["audio_url"] == j["audio_urls"][0]
    assert j["cached"] is False

    # נשמר לדיסק
    d = env / "shifts" / "s1"
    assert (d / "n001.wav").exists() and (d / "n002.wav").exists()
    assert (d / "transcripts.json").exists() and (d / "summary.md").exists()
    assert json.loads((d / "transcripts.json").read_text())["notes"][0]["text"] == "הגריל איחר בעשר דקות"

    # מייל: נושא וקובץ מצורף, HTML RTL
    assert len(fakes.emails) == 1
    em = fakes.emails[0]
    assert em["subject"] == "סיכום משמרת — יום חמישי, 12/06/2025, 16:00–23:40"
    assert 'dir="rtl"' in em["html"] and "<h2>הלך המשמרת</h2>" in em["html"]
    assert em["filename"].endswith(".md")

    # אחזור
    assert client.get("/shift/s1/summary.md").status_code == 200
    g = client.get("/shift/s1").get_json()
    assert g["status"] == "done" and len(g["notes"]) == 2

    # הקובץ המוקרא
    audio = client.get(j["audio_url"].replace("http://localhost", ""))
    assert audio.status_code == 200 and audio.data[:4] == b"RIFF"


def test_prompt_contains_notes_times_and_rules(client, fakes):
    start(client)
    put_note(client, seq=1, t="2025-06-12T19:05:00")
    finish(client)
    system, user = fakes.llm_calls[0]
    assert "לא דווח" in system and "JSON" in system
    assert "הגריל איחר בעשר דקות" in user and "2025-06-12T19:05:00" in user
    assert "סבבי שאלות פנויים: 2" in user


def test_finish_is_idempotent_and_does_not_redeliver(client, fakes):
    start(client)
    put_note(client)
    finish(client)
    r = finish(client).get_json()
    assert r["cached"] is True and r["status"] == "done"
    assert len(fakes.emails) == 1 and len(fakes.llm_calls) == 1
    # force מייצר מחדש
    finish(client, force=True)
    assert len(fakes.emails) == 2


def test_new_note_after_done_regenerates(client, fakes):
    start(client)
    put_note(client, seq=1)
    finish(client)
    put_note(client, seq=2)
    r = finish(client).get_json()
    assert r["cached"] is False and len(fakes.llm_calls) == 2


# ---------------------------------------------------------------- כשלי משלוח


def test_email_failure_does_not_fail_summary(client, fakes):
    fakes.email_fail = True
    start(client)
    put_note(client)
    r = finish(client)
    j = r.get_json()
    assert r.status_code == 200 and j["status"] == "done"
    assert j["delivered"]["email"] is False
    assert "smtp down" in j["delivered"]["errors"]["email"]
    assert client.get("/shift/s1/summary.md").status_code == 200


def test_drive_failure_does_not_fail_summary(client, fakes, monkeypatch):
    def boom(title, html):
        raise RuntimeError("drive 403")

    monkeypatch.setattr(shift, "drive_upload", boom)
    start(client)
    put_note(client)
    j = finish(client).get_json()
    assert j["status"] == "done" and j["delivered"]["email"] is True
    assert j["delivered"]["docs"] is None and "drive 403" in j["delivered"]["errors"]["docs"]


def test_tts_failure_gives_no_audio_but_summary(client, fakes, monkeypatch):
    def boom(text):
        raise shift.requests.RequestException("tts down")

    monkeypatch.setattr(shift, "tts_wav", boom)
    start(client)
    put_note(client)
    j = finish(client).get_json()
    assert j["status"] == "done" and j["audio_url"] is None and j["audio_urls"] == []
    assert j["spoken"]


# ---------------------------------------------------------------- פרופיל


def test_no_profile_still_works(client, fakes):
    start(client)
    put_note(client)
    assert finish(client).get_json()["status"] == "done"
    assert "אין פרופיל סגנון" in fakes.llm_calls[0][0]


def test_profile_and_samples_go_into_system_prompt(client, fakes, env):
    pd = env / "profile"
    (pd / "samples").mkdir(parents=True)
    (pd / "profile.md").write_text("קו חם: אורי. מילה מיוחדת: ZEBRA42", encoding="utf-8")
    (pd / "samples" / "a.md").write_text("# סיכום ישן\nסגנון: ישיר MARKER77", encoding="utf-8")
    start(client)
    put_note(client)
    finish(client)
    system = fakes.llm_calls[0][0]
    assert "ZEBRA42" in system and "MARKER77" in system
    assert "אין פרופיל סגנון" not in system


def test_example_files_are_not_loaded(client, fakes, env):
    pd = env / "profile"
    (pd / "samples.example").mkdir(parents=True)
    (pd / "profile.example.md").write_text("SHOULD_NOT_LOAD", encoding="utf-8")
    (pd / "samples.example" / "s.md").write_text("SHOULD_NOT_LOAD", encoding="utf-8")
    start(client)
    put_note(client)
    finish(client)
    assert "SHOULD_NOT_LOAD" not in fakes.llm_calls[0][0]


def test_repo_example_profile_exists_and_is_complete():
    from pathlib import Path

    base = Path(shift.__file__).parent / "profile"
    ex = (base / "profile.example.md").read_text(encoding="utf-8")
    for needle in ["## מי אני", "## עמדות", "## הצוות", "## מונחים", "## הטון", "## מה לא לכלול", "profile.md"]:
        assert needle in ex
    sample = (base / "samples.example" / "sample_01.md").read_text(encoding="utf-8")
    for sec in shift.SECTIONS + ["גרסה מוקראת"]:
        assert f"## {sec}" in sample


# ---------------------------------------------------------------- need_info


def test_need_info_flow(client, fakes):
    fakes.llm_responses = [
        json.dumps({"summary_md": "", "spoken": "", "questions": ["מה היה עם הגריל בסוף?", "q2", "q3", "q4"]}),
        json.dumps(GOOD_JSON, ensure_ascii=False),
    ]
    start(client)
    put_note(client, seq=1)
    r = finish(client).get_json()
    assert r["status"] == "need_info" and len(r["questions"]) == 3 and r["round"] == 1
    assert client.get("/shift/s1").get_json()["status"] == "need_info"
    assert fakes.emails == []  # לא שולחים כשחסר מידע

    put_note(client, seq=2, kind="answer")
    r2 = finish(client).get_json()
    assert r2["status"] == "done"
    user = fakes.llm_calls[1][1]
    assert "kind=answer" in user and "מה היה עם הגריל בסוף?" in user
    assert "סבבי שאלות פנויים: 1" in user


def test_need_info_capped_at_two_rounds(client, fakes):
    q = json.dumps({"summary_md": "", "spoken": "", "questions": ["עוד שאלה?"]}, ensure_ascii=False)
    fakes.llm_responses = [q, q, q]
    start(client)
    put_note(client, seq=1)
    assert finish(client).get_json()["status"] == "need_info"
    put_note(client, seq=2, kind="answer")
    assert finish(client).get_json()["status"] == "need_info"
    put_note(client, seq=3, kind="answer")
    r = finish(client).get_json()
    # בסבב השלישי אין סבבים פנויים: שאלות נחתכות והסיכום מופק, מה שחסר מסומן "לא דווח"
    assert r["status"] == "done"
    assert "סבבי שאלות פנויים: 0" in fakes.llm_calls[2][1]


# ---------------------------------------------------------------- idempotency וקלט


def test_duplicate_note_seq_is_idempotent(client, fakes, env):
    start(client)
    assert put_note(client, seq=1).get_json()["status"] == "saved"
    r = put_note(client, seq=1).get_json()
    assert r["status"] == "duplicate"
    notes = json.loads((env / "shifts" / "s1" / "transcripts.json").read_text())["notes"]
    assert len(notes) == 1 and fakes.stt_calls == 1


def test_note_without_seq_gets_next(client, fakes):
    r1 = client.post("/shift/note", data=make_wav(), headers={"X-Shift-Id": "p"}, content_type="audio/wav")
    r2 = client.post("/shift/note", data=make_wav(), headers={"X-Shift-Id": "p"}, content_type="audio/wav")
    assert [r1.get_json()["seq"], r2.get_json()["seq"]] == [1, 2]


@pytest.mark.parametrize("sid", ["", "../etc", "a/b", "x" * 65, "שלום"])
def test_bad_shift_id_rejected(client, fakes, sid):
    assert put_note(client, shift_id=sid).status_code == 400
    assert client.post("/shift/start", json={"shift_id": sid}).status_code == 400
    assert client.post("/shift/finish", json={"shift_id": sid}).status_code == 400


def test_non_wav_and_tiny_rejected(client, fakes):
    r = client.post("/shift/note", data=b"x" * 2000, headers={"X-Shift-Id": "s1"})
    assert r.status_code == 415
    r = client.post("/shift/note", data=b"RIFF", headers={"X-Shift-Id": "s1"})
    assert r.status_code == 415


def test_finish_unknown_shift(client, fakes):
    assert finish(client, shift_id="nope").status_code == 404


def test_transcription_failure_saves_audio_and_retries_on_finish(client, fakes, env):
    fakes.stt_fail = True
    start(client)
    r = put_note(client, seq=1).get_json()
    assert r == {"status": "saved", "seq": 1, "transcribed": False}
    assert (env / "shifts" / "s1" / "n001.wav").exists()
    assert finish(client).status_code == 503  # אין שום תמלול: לא מסכמים על כלום
    fakes.stt_fail = False
    assert finish(client).get_json()["status"] == "done"
    assert "הגריל איחר" in fakes.llm_calls[0][1]


# ---------------------------------------------------------------- בלי LLM / פלט פגום


def test_llm_unavailable_falls_back_without_inventing(client, fakes):
    fakes.llm_responses = [RuntimeError("no key")]
    start(client)
    put_note(client)
    j = finish(client).get_json()
    assert j["status"] == "done" and j["llm"] is False
    assert "הגריל איחר בעשר דקות" in j["summary_md"]
    assert j["summary_md"].count("לא דווח") >= 4
    assert j["spoken"] == "" and j["audio_url"] is None
    assert j["delivered"]["email"] is True


def test_invalid_json_retried_once_then_ok(client, fakes):
    fakes.llm_responses = ["זה לא JSON", "```json\n" + json.dumps(GOOD_JSON, ensure_ascii=False) + "\n```"]
    start(client)
    put_note(client)
    j = finish(client).get_json()
    assert j["llm"] is True and len(fakes.llm_calls) == 2


def test_missing_sections_are_added_as_not_reported():
    status, md, spoken, q = shift.normalize_result(
        {"summary_md": "## הלך המשמרת\nטוב.", "spoken": "טוב.", "questions": []}, "# כותרת", 2
    )
    assert status == "done"
    for sec in shift.SECTIONS:
        assert f"## {sec}" in md
    assert md.startswith("# כותרת\n")


def test_model_title_and_spoken_section_are_replaced():
    body = "# כותרת מומצאת\n## הלך המשמרת\nטוב.\n\n## גרסה מוקראת\nסתם"
    _, md, _, _ = shift.normalize_result({"summary_md": body, "spoken": "אמיתי.", "questions": []}, "# כותרת אמיתית", 2)
    assert "מומצאת" not in md and "סתם" not in md
    assert md.count("## גרסה מוקראת") == 1 and "אמיתי." in md


def test_spoken_trimmed_to_word_limit():
    long = " ".join(["מילה"] * 300) + "."
    _, _, spoken, _ = shift.normalize_result({"summary_md": "x", "spoken": long, "questions": []}, "# t", 2)
    assert len(spoken.split()) <= shift.SPOKEN_MAX_WORDS


def test_split_spoken_chunks_under_limit():
    text = " ".join(f"משפט מספר {i} עם כמה מילים." for i in range(40))
    chunks = shift.split_spoken(text)
    assert len(chunks) > 1 and all(len(c) <= shift.SPOKEN_CHUNK_CHARS * 1.5 for c in chunks)
    assert " ".join(chunks).replace("  ", " ") == text


def test_title_weekday_correct():
    # 2025-06-12 היה יום חמישי
    assert "יום חמישי, 12/06/2025, 16:00–23:40" in shift.build_title("2025-06-12T16:00:00", "2025-06-12T23:40:00")
    assert shift.build_title(None, None) == "# סיכום משמרת"
    assert "יום ראשון" in shift.build_title("2025-06-15T09:00:00", None)  # 15/06/2025: ראשון


# ---------------------------------------------------------------- אבטחה


def test_token_required_when_set(client, fakes, monkeypatch):
    monkeypatch.setenv("BRIDGE_TOKEN", "sekret")
    assert start(client).status_code == 401
    assert put_note(client).status_code == 401
    assert finish(client).status_code == 401
    assert client.get("/shift/s1").status_code == 401
    assert client.get("/shift/s1/summary.md").status_code == 401
    assert start(client, headers={"Authorization": "Bearer wrong"}).status_code == 401
    auth = {"Authorization": "Bearer sekret"}
    assert start(client, headers=auth).status_code == 200
    assert put_note(client, headers=auth).status_code == 200
    j = finish(client, headers=auth).get_json()
    assert j["status"] == "done"
    assert client.get("/shift/s1/summary.md", headers=auth).status_code == 200


def test_audio_link_is_signed_when_token_set(client, fakes, monkeypatch):
    monkeypatch.setenv("BRIDGE_TOKEN", "sekret")
    auth = {"Authorization": "Bearer sekret"}
    start(client, headers=auth)
    put_note(client, headers=auth)
    url = finish(client, headers=auth).get_json()["audio_url"]
    path = url.replace("http://localhost", "")
    assert "?t=" in path
    assert client.get(path).status_code == 200  # הנגן במכשיר לא שולח כותרות
    assert client.get(path.split("?")[0]).status_code == 401
    assert client.get(path.split("?")[0] + "?t=bad").status_code == 401


def test_phone_page_served_without_token(client, monkeypatch):
    monkeypatch.setenv("BRIDGE_TOKEN", "sekret")
    r = client.get("/shift/phone")
    assert r.status_code == 200 and b"MediaRecorder" in r.data and b'dir="rtl"' in r.data
    assert b"sekret" not in r.data


def test_existing_voice_endpoint_still_works(client, env):
    r = client.post("/voice", data=b"tiny")
    assert r.status_code == 200 and r.get_json()["reply"] == "Didn't catch that"
    assert client.get("/health").get_json() == {"ok": True}


# ---------------------------------------------------------------- מייל אמיתי (SMTP ממוק)


class FakeSMTP:
    sent = []

    def __init__(self, host, port, timeout=None, context=None):
        self.host, self.port = host, port

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def starttls(self, context=None):
        self.tls = True

    def login(self, u, p):
        self.login_args = (u, p)

    def send_message(self, msg):
        FakeSMTP.sent.append(msg)


def test_send_email_goes_only_to_configured_address(env, monkeypatch):
    FakeSMTP.sent = []
    monkeypatch.setattr(smtplib, "SMTP", FakeSMTP)
    monkeypatch.setenv("SMTP_HOST", "smtp.test")
    monkeypatch.setenv("SMTP_USER", "me@test")
    monkeypatch.setenv("SMTP_PASS", "pw")
    monkeypatch.setenv("SHIFT_EMAIL_TO", "me@test")
    shift.send_email("נושא", "טקסט", "<p>html</p>", "# md".encode(), "shift-summary.md")
    msg = FakeSMTP.sent[0]
    assert msg["To"] == "me@test" and msg["Subject"] == "נושא"
    kinds = [p.get_content_type() for p in msg.walk()]
    assert "text/plain" in kinds and "text/html" in kinds
    assert any(p.get_filename() == "shift-summary.md" for p in msg.walk())


def test_send_email_not_configured_raises(env):
    with pytest.raises(RuntimeError, match="not_configured"):
        shift.send_email("s", "t", "h", b"", "f.md")


def test_deliver_reports_not_configured(env, monkeypatch):
    for fn in ("send_email", "drive_upload"):
        monkeypatch.setattr(shift, fn, getattr(shift, fn))
    r = shift.deliver("# כותרת\n\n## הלך המשמרת\nטוב")
    assert r["email"] is False and r["errors"]["email"] == "not_configured"
    assert r["docs"] is None and "docs" not in r["errors"]


# ---------------------------------------------------------------- Drive (ממוק)


def test_drive_upload_request_shape(env, monkeypatch):
    monkeypatch.setenv("GDRIVE_FOLDER_ID", "FOLDER1")
    monkeypatch.setenv("GOOGLE_SA_JSON", "{}")
    monkeypatch.setattr(shift, "_drive_token", lambda: "tok123")
    seen = {}

    class R:
        def raise_for_status(self):
            pass

        def json(self):
            return {"id": "DOC1", "webViewLink": "https://docs.google.com/document/d/DOC1/edit"}

    def post(url, params=None, headers=None, data=None, timeout=None):
        seen.update(url=url, params=params, headers=headers, data=data.decode())
        return R()

    monkeypatch.setattr(shift.requests, "post", post)
    url = shift.drive_upload("סיכום", "<p>שלום</p>")
    assert url.endswith("/DOC1/edit")
    assert seen["headers"]["Authorization"] == "Bearer tok123"
    assert "FOLDER1" in seen["data"] and "application/vnd.google-apps.document" in seen["data"]
    assert "<p>שלום</p>" in seen["data"] and seen["params"]["supportsAllDrives"] == "true"


def test_drive_not_configured_returns_none(env):
    assert shift.drive_upload("t", "h") is None


# ---------------------------------------------------------------- Markdown -> HTML


def test_md_to_html_escapes_and_structures():
    html = shift.md_to_html("# כותרת <script>\n\n- פריט **מודגש**\n- עוד\n\n1. אחד\n\nפסקה & סיום")
    assert "<script>" not in html and "&lt;script&gt;" in html
    assert "<ul>" in html and "<strong>מודגש</strong>" in html and "<ol>" in html
    assert "<p>פסקה &amp; סיום</p>" in html
    assert shift.wrap_rtl_html(html).count('dir="rtl"') >= 3


def test_stt_called_with_real_wav_bytes(client, fakes, monkeypatch):
    got = []
    monkeypatch.setattr(shift, "transcribe_wav", lambda b: got.append(b[:4]) or "טקסט")
    put_note(client, seq=1)
    assert got == [b"RIFF"]


# ---------------------------------------------------------------- צורת הבקשות לשירותים האמיתיים (ממוק)


class _Resp:
    def __init__(self, payload=None, content=b""):
        self._p, self.content = payload, content

    def raise_for_status(self):
        pass

    def json(self):
        return self._p


def test_real_service_request_shapes(env, monkeypatch):
    monkeypatch.setenv("ELEVENLABS_API_KEY", "elk")
    monkeypatch.setenv("ELEVENLABS_VOICE_ID", "VOICE9")
    monkeypatch.setenv("ANTHROPIC_API_KEY", "ank")
    monkeypatch.setenv("SHIFT_MODEL", "model-x")
    calls = []

    def post(url, **kw):
        calls.append((url, kw))
        if "speech-to-text" in url:
            return _Resp({"text": " שלום "})
        if "text-to-speech" in url:
            return _Resp(content=b"\x00\x01" * 100)
        return _Resp({"content": [{"text": "{}"}]})

    monkeypatch.setattr(shift.requests, "post", post)
    assert shift.transcribe_wav(make_wav()) == "שלום"
    assert shift.call_llm("SYS", "USER") == "{}"
    wav = shift.tts_wav("טקסט")
    assert wav[:4] == b"RIFF"
    stt, llm, tts = calls
    assert stt[1]["headers"]["xi-api-key"] == "elk" and stt[1]["data"]["model_id"] == "scribe_v1"
    assert llm[1]["headers"]["x-api-key"] == "ank" and llm[1]["json"]["model"] == "model-x"
    assert llm[1]["json"]["system"] == "SYS" and llm[1]["json"]["messages"][0]["content"] == "USER"
    assert "VOICE9" in tts[0] and tts[1]["params"]["output_format"] == "pcm_16000"


def test_missing_keys_raise_or_empty(env):
    with pytest.raises(RuntimeError):
        shift.transcribe_wav(b"x")
    with pytest.raises(RuntimeError):
        shift.call_llm("s", "u")
    assert shift.tts_wav("טקסט") == b""


def test_llm_http_error_falls_back(client, fakes):
    fakes.llm_responses = [shift.requests.RequestException("503")]
    start(client)
    put_note(client)
    assert finish(client).get_json()["llm"] is False
