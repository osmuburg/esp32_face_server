#!/usr/bin/env python3
# app.py - Servidor Flask de reconocimiento facial (Raspberry Pi Zero 2W)

import os
import io
import cv2
import numpy as np
from datetime import datetime
from flask import Flask, request, jsonify, send_from_directory
from PIL import Image

# ================== CONFIG ==================
BASE_DIR = os.path.dirname(os.path.abspath(__file__))

KNOWN_DIR = os.path.join(BASE_DIR, "known_faces")
LOG_DIR   = os.path.join(BASE_DIR, "logs")
MODEL_DIR = os.path.join(BASE_DIR, "models")

DETECTOR_PROTO = os.path.join(MODEL_DIR, "deploy.prototxt")
DETECTOR_MODEL = os.path.join(MODEL_DIR, "res10_300x300_ssd_iter_140000.caffemodel")
RECOGNIZER_MODEL = os.path.join(MODEL_DIR, "face_recognition_sface_2021dec.onnx")

SIMILARITY_THRESHOLD = 0.55  # recomendado: 0.5–0.65

TELEGRAM_TOKEN = os.environ.get("TELEGRAM_TOKEN", "")
TELEGRAM_CHAT_ID = os.environ.get("TELEGRAM_CHAT_ID", "")

# ============================================

os.makedirs(KNOWN_DIR, exist_ok=True)
os.makedirs(LOG_DIR, exist_ok=True)

app = Flask(__name__)

# ================== TELEGRAM ==================
try:
    import telegram
    bot = telegram.Bot(token=TELEGRAM_TOKEN) if TELEGRAM_TOKEN and TELEGRAM_CHAT_ID else None
except Exception:
    bot = None

def notify_telegram(msg):
    if bot:
        try:
            bot.send_message(chat_id=TELEGRAM_CHAT_ID, text=msg)
        except Exception as e:
            print("Telegram error:", e)

# ================== MODELOS ==================
print("📦 Cargando modelos...")
face_detector = cv2.dnn.readNetFromCaffe(DETECTOR_PROTO, DETECTOR_MODEL)
face_recognizer = cv2.dnn.readNetFromONNX(RECOGNIZER_MODEL)
print("✅ Modelos cargados")

# ================== UTILIDADES ==================
def save_attempt(img_bytes, prefix="recv"):
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    fname = f"{prefix}_{ts}.jpg"
    path = os.path.join(LOG_DIR, fname)
    with open(path, "wb") as f:
        f.write(img_bytes)
    return path

def detect_face(image):
    h, w = image.shape[:2]
    blob = cv2.dnn.blobFromImage(
        cv2.resize(image, (300, 300)),
        1.0,
        (300, 300),
        (104.0, 177.0, 123.0)
    )

    face_detector.setInput(blob)
    detections = face_detector.forward()

    best_conf = 0
    best_box = None

    for i in range(detections.shape[2]):
        conf = detections[0, 0, i, 2]
        if conf > 0.6 and conf > best_conf:
            box = detections[0, 0, i, 3:7] * np.array([w, h, w, h])
            best_box = box.astype("int")
            best_conf = conf

    return best_box

def extract_embedding(image):
    box = detect_face(image)
    if box is None:
        return None

    x1, y1, x2, y2 = box
    face = image[y1:y2, x1:x2]

    if face.size == 0:
        return None

    face = cv2.resize(face, (112, 112))
    blob = cv2.dnn.blobFromImage(
        face,
        1.0 / 255,
        (112, 112),
        (0, 0, 0),
        swapRB=True,
        crop=False
    )

    face_recognizer.setInput(blob)
    emb = face_recognizer.forward()
    return emb.flatten()

def cosine_similarity(a, b):
    return np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b))

# ================== BASE DE DATOS EN MEMORIA ==================
known_embeddings = []
known_names = []

def load_known_faces():
    global known_embeddings, known_names
    known_embeddings = []
    known_names = []

    for fname in os.listdir(KNOWN_DIR):
        path = os.path.join(KNOWN_DIR, fname)
        name, ext = os.path.splitext(fname)

        if ext.lower() not in [".jpg", ".jpeg", ".png"]:
            continue

        img = cv2.imread(path)
        if img is None:
            continue

        emb = extract_embedding(img)
        if emb is not None:
            known_embeddings.append(emb)
            known_names.append(name)
            print(f"✔ Rostro cargado: {name}")
        else:
            print(f"⚠ No se detectó rostro en {fname}")

load_known_faces()

# ================== ENDPOINTS ==================
@app.route("/known", methods=["GET"])
def list_known():
    return jsonify({"known": known_names})

@app.route("/register", methods=["POST"])
def register():
    name = request.form.get("name")
    file = request.files.get("image")

    if not name or not file:
        return jsonify({"error": "name and image required"}), 400

    name = name.strip().replace(" ", "_")
    path = os.path.join(KNOWN_DIR, f"{name}.jpg")
    file.save(path)

    img = cv2.imread(path)
    emb = extract_embedding(img)

    if emb is None:
        os.remove(path)
        return jsonify({"error": "no_face_detected"}), 400

    load_known_faces()
    notify_telegram(f"📌 Nuevo rostro registrado: {name}")
    return jsonify({"status": "ok", "name": name})

@app.route("/recognize", methods=["POST"])
def recognize():
    img_bytes = request.get_data()
    save_attempt(img_bytes)

    img = np.array(Image.open(io.BytesIO(img_bytes)).convert("RGB"))
    img = cv2.cvtColor(img, cv2.COLOR_RGB2BGR)

    emb = extract_embedding(img)
    if emb is None:
        notify_telegram("🚫 No se detectó rostro")
        return jsonify({"authorized": False, "reason": "no_face"})

    if not known_embeddings:
        return jsonify({"authorized": False, "reason": "no_known_faces"})

    similarities = [cosine_similarity(emb, k) for k in known_embeddings]
    best_idx = int(np.argmax(similarities))
    best_score = similarities[best_idx]

    if best_score >= SIMILARITY_THRESHOLD:
        name = known_names[best_idx]
        notify_telegram(f"✅ Acceso autorizado: {name} ({best_score:.2f})")
        return jsonify({"authorized":True, "name": name, "score": float(best_score)})
    else:
        notify_telegram(f"❌ Acceso denegado ({best_score:.2f})")
        return jsonify({"authorized": False, "reason": "not_matched"})

@app.route("/known/<name>", methods=["GET"])
def get_known(name):
    for fname in os.listdir(KNOWN_DIR):
        n, ext = os.path.splitext(fname)
        if n == name:
            return send_from_directory(KNOWN_DIR, fname)
    return jsonify({"error": "not_found"}), 404

# ================== MAIN ==================
if __name__ == "__main__":
    print("🚀 Servidor iniciado en puerto 5000")
    app.run(host="0.0.0.0", port=5000)
