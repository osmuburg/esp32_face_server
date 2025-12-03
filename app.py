#!/usr/bin/env python3
# app.py - servidor Flask para reconocimiento facial
import os
import io
from datetime import datetime
from flask import Flask, request, jsonify, send_from_directory
import face_recognition
from PIL import Image
import numpy as np

# Optional: Telegram notifications
try:
    import telegram
except Exception:
    telegram = None

# CONFIG
KNOWN_DIR = "known_faces"
LOG_DIR = "logs"
TELEGRAM_TOKEN = os.environ.get("7998500572:AAFCKRu1cITqLfJF_kB6neVoodaxzZmFR-c", "")    # set via env var (recommended)
TELEGRAM_CHAT_ID = os.environ.get("5998082378", "")  # set via env var

# Create folders if not exist
os.makedirs(KNOWN_DIR, exist_ok=True)
os.makedirs(LOG_DIR, exist_ok=True)

app = Flask(__name__)

bot = None
if telegram and TELEGRAM_TOKEN and TELEGRAM_CHAT_ID:
    try:
        bot = telegram.Bot(token=TELEGRAM_TOKEN)
    except Exception as e:
        print("Telegram init error:", e)
        bot = None

# In-memory encodings
known_encodings = []
known_names = []

def load_known_faces():
    """Carga todas las imágenes en KNOWN_DIR y calcula encodings."""
    global known_encodings, known_names
    known_encodings = []
    known_names = []
    for fname in os.listdir(KNOWN_DIR):
        path = os.path.join(KNOWN_DIR, fname)
        if not os.path.isfile(path):
            continue
        name, ext = os.path.splitext(fname)
        if ext.lower() not in [".jpg", ".jpeg", ".png"]:
            continue
        try:
            image = face_recognition.load_image_file(path)
            encs = face_recognition.face_encodings(image)
            if len(encs) > 0:
                known_encodings.append(encs[0])
                known_names.append(name)
                print(f"Loaded: {name}")
            else:
                print(f"No face found in {fname}, skipping.")
        except Exception as e:
            print(f"Error loading {fname}: {e}")

load_known_faces()

def notify_telegram(text):
    if bot:
        try:
            bot.send_message(chat_id=TELEGRAM_CHAT_ID, text=text)
        except Exception as e:
            print("Telegram send error:", e)

def save_attempt_image(img_bytes, prefix="attempt"):
    """Guarda la imagen recibida en logs con timestamp y devuelve el path."""
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    fname = f"{prefix}_{ts}.jpg"
    path = os.path.join(LOG_DIR, fname)
    with open(path, "wb") as f:
        f.write(img_bytes)
    return path

@app.route("/known", methods=["GET"])
def list_known():
    return jsonify({"known": known_names})

@app.route("/register", methods=["POST"])
def register():
    """
    Registrar una nueva cara:
    - form-data 'name' (string)
    - form-data 'image' (file)
    """
    name = request.form.get("name", None)
    file = request.files.get("image", None)
    if not name or not file:
        return jsonify({"error": "name and image required"}), 400

    # sanitize name (no spaces)
    name_clean = name.strip().replace(" ", "_")
    ext = os.path.splitext(file.filename)[1] or ".jpg"
    save_path = os.path.join(KNOWN_DIR, f"{name_clean}{ext}")
    file.save(save_path)

    # try to compute encoding to verify
    try:
        image = face_recognition.load_image_file(save_path)
        encs = face_recognition.face_encodings(image)
        if len(encs) == 0:
            # remove file
            os.remove(save_path)
            return jsonify({"error": "no_face_found"}), 400
    except Exception as e:
        return jsonify({"error": f"error_processing_image: {e}"}), 500

    load_known_faces()
    notify_telegram(f"[{datetime.now()}] Nuevo registro: {name_clean}")
    return jsonify({"status": "ok", "name": name_clean})

@app.route("/recognize", methods=["POST"])
def recognize():
    """
    Endpoint principal que acepta:
    - raw image/jpeg in body (Content-Type: image/jpeg)  <-- used by your ESP32 code
    - OR multipart/form-data with 'image' file and optional 'action' <-- alternative
    Returns JSON:
      {"authorized": true/false, "name": <name or null>, "reason": ...}
    """
    # 1) get image bytes either raw or multipart
    img_bytes = None
    action = None
    if request.content_type and request.content_type.startswith("image/"):
        # raw JPEG posted
        img_bytes = request.get_data()
    else:
        # try multipart form
        action = request.form.get("action", None)
        file = request.files.get("image", None)
        if file:
            img_bytes = file.read()
        else:
            return jsonify({"error": "no image provided"}), 400

    # save attempt for logging
    saved = save_attempt_image(img_bytes, prefix="recv")

    # load image into face_recognition
    try:
        image = face_recognition.load_image_file(io.BytesIO(img_bytes))
    except Exception as e:
        notify_telegram(f"[{datetime.now()}] Error leyendo imagen: {e}")
        return jsonify({"error": "invalid_image"}), 400

    face_locations = face_recognition.face_locations(image)
    face_encodings = face_recognition.face_encodings(image, face_locations)

    if len(face_encodings) == 0:
        notify_telegram(f"[{datetime.now()}] No face detected. Action={action}")
        return jsonify({"authorized": False, "reason": "no_face", "name": None})

    # compare first face to known encodings
    encoding = face_encodings[0]
    if len(known_encodings) == 0:
        # no known faces
        notify_telegram(f"[{datetime.now()}] No known faces in DB.")
        return jsonify({"authorized": False, "reason": "no_known_faces", "name": None})

    matches = face_recognition.compare_faces(known_encodings, encoding, tolerance=0.5)
    distances = face_recognition.face_distance(known_encodings, encoding)

    best_idx = None
    if any(matches):
        best_idx = int(np.argmin(distances))
        name = known_names[best_idx]
        notify_telegram(f"[{datetime.now()}] Autorizado: {name} (dist={distances[best_idx]:.3f})")
        return jsonify({"authorized": True, "name": name, "distance": float(distances[best_idx])})
    else:
        notify_telegram(f"[{datetime.now()}] Intento NO autorizado (dist_min={float(np.min(distances)) if len(distances)>0 else 'n/a'})")
        return jsonify({"authorized": False, "name": None, "reason": "not_matched"})

# (optional) download known image
@app.route("/known/<name>", methods=["GET"])
def get_known(name):
    # returns file if exists
    for fname in os.listdir(KNOWN_DIR):
        n, ext = os.path.splitext(fname)
        if n == name:
            return send_from_directory(KNOWN_DIR, fname)
    return jsonify({"error": "not_found"}), 404

if __name__ == "__main__":
    # Run on all interfaces so ESP32 can reach it
    app.run(host="0.0.0.0", port=5000, debug=False)
