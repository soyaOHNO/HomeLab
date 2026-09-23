import copy
import json
import os
import threading
from contextlib import asynccontextmanager
from enum import Enum
from typing import Any

import paho.mqtt.client as mqtt
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel, Field
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

import sqlite3
import time
from pathlib import Path

from fastapi import Query


MQTT_HOST = os.getenv("MQTT_HOST", "mqtt")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_USERNAME = os.environ["MQTT_USERNAME"]
MQTT_PASSWORD = os.environ["MQTT_PASSWORD"]

TOPIC_SET = "home/aircon/aircon-01/set"
TOPIC_STATE = "home/aircon/aircon-01/state"
TOPIC_TELEMETRY = "home/aircon/aircon-01/telemetry"
TOPIC_AVAILABILITY = "home/aircon/aircon-01/availability"

DB_PATH = os.getenv("DB_PATH", "/data/home-iot.db")

TELEMETRY_SAVE_INTERVAL_SECONDS = 30
TELEMETRY_RETENTION_DAYS = 90

database_lock = threading.Lock()
last_saved_monotonic = 0.0
last_cleanup_epoch = 0


class Mode(str, Enum):
    AUTO = "auto"
    COOL = "cool"
    DRY = "dry"
    HEAT = "heat"
    FAN = "fan"


class FanSpeed(str, Enum):
    AUTO = "auto"
    MIN = "min"
    LOW = "low"
    MEDIUM = "medium"
    HIGH = "high"
    MAX = "max"


class SwingVertical(str, Enum):
    AUTO = "auto"
    HIGHEST = "highest"
    HIGH = "high"
    MIDDLE = "middle"
    LOW = "low"
    LOWEST = "lowest"


class AirconCommand(BaseModel):
    power: bool | None = None
    mode: Mode | None = None
    temperature: int | None = Field(
        default=None,
        ge=16,
        le=30,
    )
    fan: FanSpeed | None = None
    swing_vertical: SwingVertical | None = None
    quiet: bool | None = None
    powerful: bool | None = None


cache_lock = threading.Lock()

device_cache: dict[str, Any] = {
    "availability": "unknown",
    "state": None,
    "telemetry": None,
}


def initialize_database() -> None:
    Path(DB_PATH).parent.mkdir(parents=True, exist_ok=True)

    with sqlite3.connect(DB_PATH) as connection:
        connection.execute("PRAGMA journal_mode=WAL")
        connection.execute("PRAGMA synchronous=NORMAL")

        connection.execute(
            """
            CREATE TABLE IF NOT EXISTS telemetry (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                recorded_at INTEGER NOT NULL,
                temperature REAL NOT NULL,
                humidity REAL NOT NULL,
                rssi INTEGER
            )
            """
        )

        connection.execute(
            """
            CREATE INDEX IF NOT EXISTS idx_telemetry_recorded_at
            ON telemetry(recorded_at)
            """
        )

        connection.commit()

initialize_database()

def save_telemetry(payload: dict | str) -> None:
    global last_saved_monotonic
    global last_cleanup_epoch

    if isinstance(payload, str):
        try:
            payload = json.loads(payload)
        except json.JSONDecodeError:
            print("Invalid telemetry JSON:", payload)
            return

    if not isinstance(payload, dict):
        print("Invalid telemetry payload type:", type(payload))
        return

    temperature = payload.get("temperature")
    humidity = payload.get("humidity")
    rssi = payload.get("rssi")


    if not isinstance(temperature, (int, float)):
        return

    if not isinstance(humidity, (int, float)):
        return

    current_monotonic = time.monotonic()

    with database_lock:
        if (
            current_monotonic - last_saved_monotonic
            < TELEMETRY_SAVE_INTERVAL_SECONDS
        ):
            return

        recorded_at = int(time.time())

        with sqlite3.connect(DB_PATH) as connection:
            connection.execute(
                """
                INSERT INTO telemetry (
                    recorded_at,
                    temperature,
                    humidity,
                    rssi
                )
                VALUES (?, ?, ?, ?)
                """,
                (
                    recorded_at,
                    float(temperature),
                    float(humidity),
                    int(rssi) if isinstance(rssi, (int, float)) else None,
                ),
            )

            # 1日1回、90日より古いデータを削除
            if recorded_at - last_cleanup_epoch >= 86400:
                retention_limit = (
                    recorded_at
                    - TELEMETRY_RETENTION_DAYS * 86400
                )

                connection.execute(
                    """
                    DELETE FROM telemetry
                    WHERE recorded_at < ?
                    """,
                    (retention_limit,),
                )

                last_cleanup_epoch = recorded_at

            connection.commit()

        last_saved_monotonic = current_monotonic

def on_connect(
    client: mqtt.Client,
    userdata: Any,
    flags: mqtt.ConnectFlags,
    reason_code: mqtt.ReasonCode,
    properties: mqtt.Properties | None,
) -> None:
    print(f"MQTT connected: {reason_code}")

    if reason_code != 0:
        return

    client.subscribe(
        [
            (TOPIC_STATE, 1),
            (TOPIC_TELEMETRY, 1),
            (TOPIC_AVAILABILITY, 1),
        ]
    )


def on_disconnect(
    client: mqtt.Client,
    userdata: Any,
    disconnect_flags: mqtt.DisconnectFlags,
    reason_code: mqtt.ReasonCode,
    properties: mqtt.Properties | None,
) -> None:
    print(f"MQTT disconnected: {reason_code}")


def on_message(
    client: mqtt.Client,
    userdata: Any,
    message: mqtt.MQTTMessage,
) -> None:
    payload = message.payload.decode("utf-8")

    if message.topic == TOPIC_AVAILABILITY:
        with cache_lock:
            device_cache["availability"] = payload
        return

    try:
        parsed = json.loads(payload)
    except json.JSONDecodeError:
        print(f"Invalid JSON on {message.topic}: {payload}")
        return

    if message.topic == TOPIC_STATE:
        with cache_lock:
            device_cache["state"] = parsed

    elif message.topic == TOPIC_TELEMETRY:
        with cache_lock:
            device_cache["telemetry"] = parsed

        # DB保存はcache_lockの外で行う
        save_telemetry(parsed)


mqtt_client = mqtt.Client(
    mqtt.CallbackAPIVersion.VERSION2,
    client_id="home-iot-server",
)

mqtt_client.username_pw_set(
    MQTT_USERNAME,
    MQTT_PASSWORD,
)

mqtt_client.on_connect = on_connect
mqtt_client.on_disconnect = on_disconnect
mqtt_client.on_message = on_message
mqtt_client.reconnect_delay_set(
    min_delay=1,
    max_delay=30,
)


@asynccontextmanager
async def lifespan(app: FastAPI):
    mqtt_client.connect_async(
        MQTT_HOST,
        MQTT_PORT,
        keepalive=60,
    )
    mqtt_client.loop_start()

    yield

    mqtt_client.disconnect()
    mqtt_client.loop_stop()


app = FastAPI(
    title="Home IoT API",
    version="0.1.0",
    lifespan=lifespan,
)
app.mount(
    "/static",
    StaticFiles(directory="app/static"),
    name="static",
)


@app.get("/", include_in_schema=False)
def web_ui():
    return FileResponse("app/static/index.html")


@app.get("/health")
def health() -> dict[str, Any]:
    return {
        "api": "online",
        "mqtt": mqtt_client.is_connected(),
    }


@app.get("/api/aircon")
def get_aircon() -> dict[str, Any]:
    with cache_lock:
        return copy.deepcopy(device_cache)


@app.post("/api/aircon")
def control_aircon(
    command: AirconCommand,
) -> dict[str, Any]:
    payload = command.model_dump(
        exclude_none=True,
        mode="json",
    )

    if not payload:
        raise HTTPException(
            status_code=400,
            detail="No settings were provided",
        )

    if not mqtt_client.is_connected():
        raise HTTPException(
            status_code=503,
            detail="MQTT is not connected",
        )

    result = mqtt_client.publish(
        TOPIC_SET,
        json.dumps(payload),
        qos=1,
        retain=False,
    )

    if result.rc != mqtt.MQTT_ERR_SUCCESS:
        raise HTTPException(
            status_code=503,
            detail="Failed to publish MQTT command",
        )

    return {
        "accepted": True,
        "command": payload,
    }

@app.get("/api/aircon/history")
def get_aircon_history(
    hours: int = Query(default=24, ge=1, le=720),
) -> dict[str, Any]:
    end_timestamp = int(time.time())
    start_timestamp = end_timestamp - hours * 3600

    if hours <= 6:
        bucket_seconds = 30
    elif hours <= 48:
        bucket_seconds = 300
    else:
        bucket_seconds = 1800

    with database_lock:
        with sqlite3.connect(DB_PATH) as connection:
            rows = connection.execute(
                """
                SELECT
                    CAST(recorded_at / ? AS INTEGER) * ? AS time_bucket,
                    AVG(temperature),
                    AVG(humidity),
                    AVG(rssi)
                FROM telemetry
                WHERE recorded_at >= ?
                GROUP BY time_bucket
                ORDER BY time_bucket ASC
                """,
                (
                    bucket_seconds,
                    bucket_seconds,
                    start_timestamp,
                ),
            ).fetchall()

    return {
        "hours": hours,
        "bucket_seconds": bucket_seconds,
        "points": [
            {
                "timestamp": row[0],
                "temperature": round(row[1], 2),
                "humidity": round(row[2], 2),
                "rssi": (
                    round(row[3], 1)
                    if row[3] is not None
                    else None
                ),
            }
            for row in rows
        ],
    }