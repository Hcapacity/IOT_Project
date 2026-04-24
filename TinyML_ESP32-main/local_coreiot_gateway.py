import asyncio
import json
import logging
import threading
import time
import warnings

import paho.mqtt.client as mqtt
from amqtt.broker import Broker


logging.basicConfig(level=logging.ERROR)
logging.getLogger("amqtt").setLevel(logging.ERROR)
warnings.filterwarnings("ignore", category=DeprecationWarning, module=r"amqtt(\..*)?")


# =========================
# Local MQTT broker settings
# =========================
LOCAL_BROKER_BIND = "0.0.0.0:1883"
LOCAL_BROKER_HOST = "127.0.0.1"
LOCAL_BROKER_PORT = 1883

# ESP32 firmware currently publishes to this topic
LOCAL_TELEMETRY_TOPIC = "v1/devices/me/telemetry"

# For display only; set this to your PC LAN IP used by ESP32
LOCAL_SERVER_IP = "192.168.1.9"


# =========================
# CoreIOT gateway settings
# =========================
COREIOT_HOST = "app.coreiot.io"
COREIOT_PORT = 1883
ACCESS_TOKEN = "srQf3iXbZDdZjL8PuatC"
COREIOT_TELEMETRY_TOPIC = "v1/gateway/telemetry"

# Device name that will appear on CoreIOT
COREIOT_DEVICE_NAME = "ESP32_S3"


# =========================
# Runtime behavior
# =========================
PUSH_INTERVAL_SECONDS = 5
TELEMETRY_STORE_FILE = "telemetry_store.jsonl"


broker_config = {
    "listeners": {
        "default": {
            "type": "tcp",
            "bind": LOCAL_BROKER_BIND,
        }
    },
    "sys_interval": 10,
    "auth": {
        # Keep true for easiest bring-up.
        # If you need strict username/password auth, switch to TinyBroker.py + plugin flow.
        "allow-anonymous": True,
    },
    "topic-check": {
        "enabled": True,
        "plugins": ["topic_taboo"],
    },
}


pending_lock = threading.Lock()
pending_packets = []


def start_local_broker():
    async def broker_main():
        broker = Broker(broker_config)
        await broker.start()
        print(f"[LocalBroker] Started at {LOCAL_BROKER_BIND}")
        while True:
            await asyncio.sleep(3600)

    asyncio.run(broker_main())


def append_telemetry_file(packet):
    record = {
        "ts": int(time.time() * 1000),
        "device": COREIOT_DEVICE_NAME,
        "payload": packet,
    }
    with open(TELEMETRY_STORE_FILE, "a", encoding="utf-8") as f:
        f.write(json.dumps(record, ensure_ascii=True) + "\n")


def normalize_packet(packet):
    if not isinstance(packet, dict):
        return None

    # Already in {"ts": ..., "values": {...}}
    if "values" in packet and isinstance(packet.get("values"), dict):
        return {
            "ts": int(packet.get("ts", int(time.time() * 1000))),
            "values": packet["values"],
        }

    # Raw telemetry dict from ESP32: {"temperature": ..., "humidity": ...}
    return {
        "ts": int(time.time() * 1000),
        "values": packet,
    }


def local_on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f"[Collector] Subscribed to {LOCAL_TELEMETRY_TOPIC}")
        client.subscribe(LOCAL_TELEMETRY_TOPIC, qos=1)
    else:
        print(f"[Collector] Local connect failed rc={rc}")


def local_on_message(client, userdata, msg):
    try:
        packet = json.loads(msg.payload.decode("utf-8"))
    except json.JSONDecodeError:
        print("[Collector] Invalid JSON telemetry")
        return

    normalized = normalize_packet(packet)
    if normalized is None:
        print("[Collector] Invalid telemetry schema")
        return

    append_telemetry_file(normalized)

    with pending_lock:
        pending_packets.append(normalized)

    print("[Collector] Saved telemetry packet")


def build_gateway_payload(batch):
    return {
        COREIOT_DEVICE_NAME: batch
    }


def pop_pending_batch():
    with pending_lock:
        if not pending_packets:
            return None
        batch = pending_packets[:]
        pending_packets.clear()
    return batch


def main():
    broker_thread = threading.Thread(target=start_local_broker, daemon=True)
    broker_thread.start()

    time.sleep(1)
    print(f"[INFO] ESP32 connect to local broker: {LOCAL_SERVER_IP}:{LOCAL_BROKER_PORT}")
    print(f"[INFO] Local telemetry topic: {LOCAL_TELEMETRY_TOPIC}")
    print(f"[INFO] CoreIOT device name (gateway): {COREIOT_DEVICE_NAME}")

    local_client = mqtt.Client(client_id="LocalGatewayCollector")
    local_client.on_connect = local_on_connect
    local_client.on_message = local_on_message
    local_client.connect(LOCAL_BROKER_HOST, LOCAL_BROKER_PORT, 60)
    local_client.loop_start()

    cloud_client = mqtt.Client(client_id="CoreIOTForwarder")
    cloud_client.username_pw_set(ACCESS_TOKEN, "")
    cloud_client.connect(COREIOT_HOST, COREIOT_PORT, 60)
    cloud_client.loop_start()
    print("[Cloud] Connected to CoreIOT gateway")

    try:
        while True:
            time.sleep(PUSH_INTERVAL_SECONDS)
            batch = pop_pending_batch()
            if batch is None:
                continue

            payload = build_gateway_payload(batch)
            cloud_client.publish(COREIOT_TELEMETRY_TOPIC, json.dumps(payload), qos=1)
            print(f"[Cloud] Pushed {len(batch)} packet(s) to CoreIOT")
    except KeyboardInterrupt:
        print("[System] Stopped")
    finally:
        local_client.loop_stop()
        local_client.disconnect()
        cloud_client.loop_stop()
        cloud_client.disconnect()


if __name__ == "__main__":
    main()
