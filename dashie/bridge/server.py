#!/usr/bin/env python3
"""Dashie bridge: מקבל WAV מה-ESP32, מתמלל, מייצר תשובה קצרה ומחזיר JSON.

הרצה:
    pip install flask requests
    export ELEVENLABS_API_KEY=...      # תמלול (Scribe)
    export ANTHROPIC_API_KEY=...       # תשובה (אופציונלי; בלעדיו יש תשובות מובנות)
    export ELEVENLABS_VOICE_ID=...     # קול לדיבור בעברית (TTS)
    python server.py                   # מאזין על 0.0.0.0:8787

ב-config.h של הקושחה: BRIDGE_URL "http://<IP של המחשב/שרת>:8787/voice"
אם הטלפון הוא ה-hotspot, הריצו את השרת על מחשב/Raspberry Pi באותה רשת, או על שרת עם HTTPS.
"""
import io
import os
import re
import uuid
import wave
import datetime

import requests
from flask import Flask, Response, jsonify, request

app = Flask(__name__)

# מצב משמרת (סיכום משמרת): shift.py
from shift import bp as shift_bp  # noqa: E402

app.register_blueprint(shift_bp)

# תיאום בין מכשיר הרכב למכשיר הכיס: hub.py
from hub import bp as hub_bp, handle_voice_intent  # noqa: E402

app.register_blueprint(hub_bp)

# טיימרי שולחנות מהקשר: radio.py
from radio import bp as radio_bp  # noqa: E402

app.register_blueprint(radio_bp)

EL_KEY = os.environ.get("ELEVENLABS_API_KEY")
AN_KEY = os.environ.get("ANTHROPIC_API_KEY")
MODEL = os.environ.get("DASHIE_MODEL", "claude-haiku-5-5")
# קול ElevenLabs לעברית (חובה כדי לקבל דיבור). בחרו קול בספריית הקולות והדביקו את ה-ID.
VOICE_ID = os.environ.get("ELEVENLABS_VOICE_ID")
TTS_MODEL = os.environ.get("DASHIE_TTS_MODEL", "eleven_flash_v2_5")  # מהיר; eleven_v3 לאיכות גבוהה יותר
AUDIO = {}  # id -> wav bytes (זיכרון בלבד, עד 20 קבצים אחרונים)

SYSTEM = (
    "You are Dashie, a tiny in-car voice assistant on a dashboard screen. "
    "The driver is driving: answer in at most 160 characters, plain text, no emoji, "
    "no lists. Reply in the language the driver spoke (Hebrew or English). "
    "Never suggest looking at the screen for details."
)


def transcribe(wav_bytes: bytes) -> str:
    if not EL_KEY:
        return ""
    r = requests.post(
        "https://api.elevenlabs.io/v1/speech-to-text",
        headers={"xi-api-key": EL_KEY},
        files={"file": ("audio.wav", wav_bytes, "audio/wav")},
        data={"model_id": "scribe_v1"},
        timeout=20,
    )
    r.raise_for_status()
    return r.json().get("text", "").strip()


def llm(text: str) -> str:
    if not AN_KEY:
        return ""
    r = requests.post(
        "https://api.anthropic.com/v1/messages",
        headers={
            "x-api-key": AN_KEY,
            "anthropic-version": "2023-06-01",
            "content-type": "application/json",
        },
        json={
            "model": MODEL,
            "max_tokens": 120,
            "system": SYSTEM,
            "messages": [{"role": "user", "content": text}],
        },
        timeout=20,
    )
    r.raise_for_status()
    return r.json()["content"][0]["text"].strip()[:160]


def tts_wav(text: str) -> bytes:
    """ElevenLabs TTS -> WAV 16kHz mono 16bit (הרמקול של M5Stack מנגן WAV)."""
    if not (EL_KEY and VOICE_ID and text):
        return b""
    r = requests.post(
        f"https://api.elevenlabs.io/v1/text-to-speech/{VOICE_ID}",
        params={"output_format": "pcm_16000"},
        headers={"xi-api-key": EL_KEY, "content-type": "application/json"},
        json={"text": text, "model_id": TTS_MODEL},
        timeout=25,
    )
    r.raise_for_status()
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(r.content)
    return buf.getvalue()


def store_audio(wav: bytes) -> str:
    key = uuid.uuid4().hex[:12]
    AUDIO[key] = wav
    while len(AUDIO) > 20:
        AUDIO.pop(next(iter(AUDIO)))
    return key


def builtin(text: str) -> str:
    t = text.lower()
    if re.search(r"שעה|time", t):
        return datetime.datetime.now().strftime("It is %H:%M")
    if re.search(r"שלום|היי|hello|hi", t):
        return "Hi! Drive safe."
    return "I heard you. Connect an API key for smarter answers."


@app.post("/voice")
def voice():
    wav = request.get_data()
    if len(wav) < 1000:
        return jsonify(text="", reply="Didn't catch that", action="face:confused")
    try:
        text = transcribe(wav)
        if not text:
            return jsonify(text="", reply="Didn't catch that", action="face:confused")
        # פקודות Hub ("איפה הכיס שלי", "תקריא את הסיכום") מטופלות לפני ה-LLM
        intent = handle_voice_intent(text, request.headers.get("X-Device-Id"))
        if intent:
            reply, action = intent["reply"], intent["action"]
        else:
            reply, action = llm(text) or builtin(text), "face:happy"
        audio_url = None
        try:
            wav = tts_wav(reply)
            if wav:
                audio_url = f"{request.host_url}audio/{store_audio(wav)}.wav"
        except requests.RequestException:
            pass  # בלי דיבור, עדיין מציגים טקסט
        return jsonify(text=text, reply=reply, action=action, audio_url=audio_url)
    except requests.RequestException as e:
        return jsonify(text="", reply="Bridge error", action="face:confused", error=str(e)), 200


@app.get("/audio/<key>.wav")
def audio(key):
    wav = AUDIO.get(key)
    if not wav:
        return Response(status=404)
    return Response(wav, mimetype="audio/wav")


@app.get("/health")
def health():
    return jsonify(ok=True)


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8787)
