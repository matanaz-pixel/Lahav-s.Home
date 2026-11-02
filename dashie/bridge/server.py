#!/usr/bin/env python3
"""Dashie bridge: מקבל WAV מה-ESP32, מתמלל, מייצר תשובה קצרה ומחזיר JSON.

הרצה:
    pip install flask requests
    export ELEVENLABS_API_KEY=...      # תמלול (Scribe)
    export ANTHROPIC_API_KEY=...       # תשובה (אופציונלי; בלעדיו יש תשובות מובנות)
    python server.py                   # מאזין על 0.0.0.0:8787

ב-config.h של הקושחה: BRIDGE_URL "http://<IP של המחשב/שרת>:8787/voice"
אם הטלפון הוא ה-hotspot, הריצו את השרת על מחשב/Raspberry Pi באותה רשת, או על שרת עם HTTPS.
"""
import os
import re
import datetime

import requests
from flask import Flask, jsonify, request

app = Flask(__name__)

EL_KEY = os.environ.get("ELEVENLABS_API_KEY")
AN_KEY = os.environ.get("ANTHROPIC_API_KEY")
MODEL = os.environ.get("DASHIE_MODEL", "claude-haiku-5-5")

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
        reply = llm(text) or builtin(text)
        return jsonify(text=text, reply=reply, action="face:happy", audio_b64=None)
    except requests.RequestException as e:
        return jsonify(text="", reply="Bridge error", action="face:confused", error=str(e)), 200


@app.get("/health")
def health():
    return jsonify(ok=True)


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8787)
