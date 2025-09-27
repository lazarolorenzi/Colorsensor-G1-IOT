import os
import json
import threading
import time
from datetime import datetime, timezone
from typing import Any, Dict, Tuple

from flask import Flask, jsonify, request
from flask_cors import CORS
from sqlalchemy import create_engine, Integer, Float, String, DateTime, JSON, Text
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column, sessionmaker
from dotenv import load_dotenv
import paho.mqtt.client as mqtt

# -------------------- Config --------------------
load_dotenv()

MQTT_HOST   = os.getenv("MQTT_HOST", "test.mosquitto.org")
MQTT_PORT   = int(os.getenv("MQTT_PORT", "1883"))
MQTT_TOPICS = [t.strip() for t in os.getenv("MQTT_TOPICS", "LazaroNicolas/ambient/lux,LazaroNicolas/ambient/color,LazaroNicolas/ambient/led").split(",")]

DB_URL      = os.getenv("DB_URL", "sqlite:///ambient.db")
FLASK_PORT  = int(os.getenv("FLASK_PORT", "5000"))

# -------------------- DB (SQLAlchemy) --------------------
class Base(DeclarativeBase):
    pass

class LuxReading(Base):
    __tablename__ = "lux_readings"
    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    ts: Mapped[datetime] = mapped_column(DateTime(timezone=True), index=True)
    lux: Mapped[float] = mapped_column(Float)
    raw: Mapped[Dict[str, Any]] = mapped_column(JSON)

class ColorReading(Base):
    __tablename__ = "color_readings"
    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    ts: Mapped[datetime] = mapped_column(DateTime(timezone=True), index=True)
    r: Mapped[int] = mapped_column(Integer)
    g: Mapped[int] = mapped_column(Integer)
    b: Mapped[int] = mapped_column(Integer)
    h: Mapped[float] = mapped_column(Float)
    s: Mapped[float] = mapped_column(Float)
    v: Mapped[float] = mapped_column(Float)
    name: Mapped[str] = mapped_column(String(32))
    raw: Mapped[Dict[str, Any]] = mapped_column(JSON)

class LedEvent(Base):
    __tablename__ = "led_events"
    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    ts: Mapped[datetime] = mapped_column(DateTime(timezone=True), index=True)
    r: Mapped[int] = mapped_column(Integer)
    g: Mapped[int] = mapped_column(Integer)
    b: Mapped[int] = mapped_column(Integer)
    raw: Mapped[Dict[str, Any]] = mapped_column(JSON)

engine = create_engine(DB_URL, echo=False, future=True)
Base.metadata.create_all(engine)
SessionLocal = sessionmaker(bind=engine, expire_on_commit=False)

# -------------------- Flask --------------------
app = Flask(__name__)
CORS(app)  # libera para front-ends locais

# -------------------- MQTT Client (paho) --------------------
mqtt_client = mqtt.Client(client_id=f"flask-collector-{int(time.time())}", clean_session=True)

def _utcnow() -> datetime:
    return datetime.now(timezone.utc)

def parse_and_store(topic: str, payload: Dict[str, Any]) -> None:
    """Normaliza e armazena conforme o tópico."""
    with SessionLocal() as db:
        if topic.endswith("/lux"):
            lux = float(payload.get("lux", 0.0))
            rec = LuxReading(ts=_utcnow(), lux=lux, raw=payload)
            db.add(rec)
        elif topic.endswith("/color"):
            rgb = payload.get("rgb", [0,0,0])
            hsv = payload.get("hsv", {"h":0, "s":0, "v":0})
            name = payload.get("color", "desconhecido")
            rec = ColorReading(
                ts=_utcnow(),
                r=int(rgb[0]), g=int(rgb[1]), b=int(rgb[2]),
                h=float(hsv.get("h",0)), s=float(hsv.get("s",0)), v=float(hsv.get("v",0)),
                name=str(name),
                raw=payload
            )
            db.add(rec)
        elif topic.endswith("/led"):
            rgb = payload.get("led_rgb") or payload.get("rgb") or [0,0,0]
            rec = LedEvent(ts=_utcnow(), r=int(rgb[0]), g=int(rgb[1]), b=int(rgb[2]), raw=payload)
            db.add(rec)
        else:
            # se quiser, crie uma tabela genérica
            pass
        db.commit()

def on_connect(client, userdata, flags, rc):
    print(f"[MQTT] conectado (rc={rc}). Assinando tópicos...")
    for t in MQTT_TOPICS:
        client.subscribe(t, qos=0)
        print(f"  - {t}")

def on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode("utf-8"))
    except Exception as e:
        print(f"[MQTT] JSON inválido em {msg.topic}: {e}")
        return
    print(f"[MQTT] {msg.topic} <- {payload}")
    try:
        parse_and_store(msg.topic, payload)
    except Exception as e:
        print(f"[DB] erro ao salvar: {e}")

def start_mqtt_loop():
    mqtt_client.on_connect = on_connect
    mqtt_client.on_message = on_message
    mqtt_client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
    mqtt_client.loop_forever()

# Inicia o MQTT em thread separada para não bloquear o Flask
t = threading.Thread(target=start_mqtt_loop, daemon=True)
t.start()

# -------------------- Helpers API --------------------
def parse_range() -> Tuple[datetime, datetime, int, int]:
    """
    Lê ?start=ISO&end=ISO&limit=100&offset=0
    Se não vier, usa últimas 24h.
    """
    now = _utcnow()
    start_str = request.args.get("start")
    end_str   = request.args.get("end")
    limit     = int(request.args.get("limit", "200"))
    offset    = int(request.args.get("offset", "0"))

    if end_str:
        end = datetime.fromisoformat(end_str)
        if end.tzinfo is None: end = end.replace(tzinfo=timezone.utc)
    else:
        end = now

    if start_str:
        start = datetime.fromisoformat(start_str)
        if start.tzinfo is None: start = start.replace(tzinfo=timezone.utc)
    else:
        start = end - timedelta(hours=24)

    return start, end, min(max(limit,1), 2000), max(offset,0)

# -------------------- Endpoints --------------------
@app.get("/")
def root():
    return jsonify({"ok": True, "msg": "Ambient Match backend (Flask + MQTT + SQLite)"}), 200

@app.get("/api/lux")
def api_lux():
    from sqlalchemy import select, desc
    start, end, limit, offset = parse_range()
    with SessionLocal() as db:
        q = (db.query(LuxReading)
               .filter(LuxReading.ts >= start, LuxReading.ts <= end)
               .order_by(LuxReading.ts.desc())
               .offset(offset).limit(limit))
        data = [{"id": r.id, "ts": r.ts.isoformat(), "lux": r.lux, "raw": r.raw} for r in q.all()]
    return jsonify({"count": len(data), "items": data})

@app.get("/api/color")
def api_color():
    start, end, limit, offset = parse_range()
    with SessionLocal() as db:
        q = (db.query(ColorReading)
               .filter(ColorReading.ts >= start, ColorReading.ts <= end)
               .order_by(ColorReading.ts.desc())
               .offset(offset).limit(limit))
        data = [{
            "id": r.id, "ts": r.ts.isoformat(),
            "rgb": [r.r, r.g, r.b],
            "hsv": {"h": r.h, "s": r.s, "v": r.v},
            "name": r.name, "raw": r.raw
        } for r in q.all()]
    return jsonify({"count": len(data), "items": data})

@app.get("/api/led")
def api_led():
    start, end, limit, offset = parse_range()
    with SessionLocal() as db:
        q = (db.query(LedEvent)
               .filter(LedEvent.ts >= start, LedEvent.ts <= end)
               .order_by(LedEvent.ts.desc())
               .offset(offset).limit(limit))
        data = [{"id": r.id, "ts": r.ts.isoformat(), "rgb": [r.r, r.g, r.b], "raw": r.raw} for r in q.all()]
    return jsonify({"count": len(data), "items": data})

@app.get("/api/latest")
def api_latest():
    """Retorna o último registro de cada coleção (útil pra dashboard simples)."""
    with SessionLocal() as db:
        lux  = db.query(LuxReading).order_by(LuxReading.ts.desc()).first()
        col  = db.query(ColorReading).order_by(ColorReading.ts.desc()).first()
        led  = db.query(LedEvent).order_by(LedEvent.ts.desc()).first()
        return jsonify({
            "lux":  {"ts": lux.ts.isoformat(), "lux": lux.lux} if lux else None,
            "color":{"ts": col.ts.isoformat(), "rgb":[col.r,col.g,col.b], "hsv":{"h":col.h,"s":col.s,"v":col.v}, "name": col.name} if col else None,
            "led":  {"ts": led.ts.isoformat(), "rgb":[led.r,led.g,led.b]} if led else None
        })

if __name__ == "__main__":
    print(f"[BOOT] MQTT broker: {MQTT_HOST}:{MQTT_PORT}")
    print(f"[BOOT] Tópicos: {MQTT_TOPICS}")
    print(f"[BOOT] DB: {DB_URL}")
    app.run(host="0.0.0.0", port=FLASK_PORT, debug=True)
