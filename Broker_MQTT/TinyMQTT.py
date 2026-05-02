import asyncio
import json

from hbmqtt.broker import Broker
import threading
import time
import socket
import paho.mqtt.client as mqtt
from passlib.hash import sha256_crypt

# === Static Broker Settings (edit as needed) ===
BROKER_IP = "192.168.137.1"
BROKER_PORT = 1883
BROKER_USER = "mqttclient"
BROKER_PASS = "12345678"
PASSWORD_FILE = "password_file.txt"

# === Gateway Settings (ThingsBoard/CoreIOT) ===
GATEWAY_HOST = "app.coreiot.io"
GATEWAY_PORT = 1883
GATEWAY_ACCESS_TOKEN = "srQf3iXbZDdZjL8PuatC"
GATEWAY_PUBLISH_INTERVAL_SEC = 5

# In-memory store: device_id -> list of samples
DEVICE_SAMPLES = {}
DEVICE_PENDING = {}
DEVICE_LOCK = threading.Lock()

# === Broker Configuration ===
broker_config = {
    'listeners': {
        'default': {
            'type': 'tcp',
            'bind': f'0.0.0.0:{BROKER_PORT}'
        }
    },
    'sys_interval': 10,
    'auth': {
        'allow-anonymous': False,
        'plugins': ['auth_file'],
        'password-file': PASSWORD_FILE
    },
    'topic-check': {
        'enabled': True,
        'plugins': ['topic_taboo']
    }
}

def ensure_password_file():
    hashed = sha256_crypt.hash(BROKER_PASS)
    with open(PASSWORD_FILE, "w", encoding="utf-8") as f:
        f.write(f"{BROKER_USER}:{hashed}\n")


def start_broker():
    async def broker_coro():
        broker = Broker(broker_config)
        await broker.start()
        print("MQTT Broker started...")

        print(f"Broker IP (use this in ESP32): {BROKER_IP}")

    # Each thread needs its own event loop
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    loop.run_until_complete(broker_coro())
    loop.run_forever()

def get_local_ip():
    try:
        hostname = socket.gethostname()
        _, _, all_ips = socket.gethostbyname_ex(hostname)
        valid_ips = [ip for ip in all_ips if not ip.startswith("127.")]

        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(("8.8.8.8", 80))
            internet_ip = s.getsockname()[0]
        except OSError:
            internet_ip = None
        finally:
            s.close()

        hotspot_ip = None
        for ip in valid_ips:
            if ip != internet_ip:
                hotspot_ip = ip
                break

        for ip in valid_ips:
            if ip.startswith("192.168.137."):
                hotspot_ip = ip

        if hotspot_ip:
            return hotspot_ip
        if internet_ip:
            return internet_ip
        return "0.0.0.0"
    except OSError:
        return "0.0.0.0"

# === MQTT Subscriber (in thread) ===
def run_subscriber():
    broker_address = "127.0.0.1"
    topic = "#" #subscribe to all topics

    def on_message(client, userdata, msg):
        payload_text = msg.payload.decode("utf-8")
        device_id = "unknown"

        parts = msg.topic.split("/")
        if len(parts) >= 3 and parts[0] == "devices" and parts[2] == "telemetry":
            device_id = parts[1]

        try:
            data = json.loads(payload_text)
        except json.JSONDecodeError:
            print("Received (non-JSON):", msg.topic, payload_text)
            return

        if isinstance(data, dict) and "device_id" in data:
            device_id = str(data["device_id"])

        if not isinstance(data, dict):
            print("Received (non-dict JSON):", msg.topic, payload_text)
            return

        sample = {
            "temperature": data.get("temperature"),
            "humidity": data.get("humidity"),
            "rain_prob": data.get("rain_prob"),
            "ts": data.get("ts")
        }

        with DEVICE_LOCK:
            DEVICE_SAMPLES.setdefault(device_id, []).append(sample)
            DEVICE_PENDING.setdefault(device_id, []).append(sample)
            total = len(DEVICE_SAMPLES[device_id])

        print(f"Stored sample for {device_id} (total={total}): {sample}")

    def on_subscribe(client, userdata, mid, granted_qos):
        print("✅ Subscribed successfully.")

    def on_connect(client, userdata, flags, rc):
        print("Connected.")
        client.subscribe(topic, qos=0)

    client = mqtt.Client("PythonSubscriber")
    client.username_pw_set(BROKER_USER, BROKER_PASS)
    client.on_message = on_message
    client.on_subscribe = on_subscribe
    client.on_connect = on_connect

    # Wait a bit for the broker to start
    time.sleep(2)

    client.connect(broker_address, BROKER_PORT)
    client.loop_forever()


def start_gateway_publisher():
    global DEVICE_PENDING

    def on_connect(client, userdata, flags, rc):
        if rc == 0:
            print("Gateway connected.")
        else:
            print(f"Gateway connect failed, rc={rc}")

    def on_publish(client, userdata, mid):
        print("Gateway message published.")

    gw_client = mqtt.Client("TinyGateway")
    gw_client.username_pw_set(GATEWAY_ACCESS_TOKEN)
    gw_client.on_connect = on_connect
    gw_client.on_publish = on_publish
    gw_client.connect(GATEWAY_HOST, GATEWAY_PORT, 60)
    gw_client.loop_start()

    while True:
        time.sleep(GATEWAY_PUBLISH_INTERVAL_SEC)

        with DEVICE_LOCK:
            if not DEVICE_PENDING:
                continue

            pending_snapshot = DEVICE_PENDING
            DEVICE_PENDING = {}

        telemetry = {}
        for device_id, samples in pending_snapshot.items():
            values_list = []
            for sample in samples:
                ts = sample.get("ts")
                if ts is None:
                    ts_ms = int(time.time() * 1000)
                else:
                    ts_ms = int(ts) * 1000 if int(ts) < 1_000_000_000_000 else int(ts)

                values = {
                    "temperature": sample.get("temperature"),
                    "humidity": sample.get("humidity"),
                    "rain_prob": sample.get("rain_prob")
                }

                values_list.append({"ts": ts_ms, "values": values})

            telemetry[device_id] = values_list

        payload = json.dumps(telemetry)
        gw_client.publish("v1/gateway/telemetry", payload)

if __name__ == "__main__":
    ensure_password_file()
    # Broker in one thread
    broker_thread = threading.Thread(target=start_broker, daemon=True)
    broker_thread.start()

    # Subscriber in another thread
    subscriber_thread = threading.Thread(target=run_subscriber, daemon=True)
    subscriber_thread.start()

    # Gateway publisher thread
    gateway_thread = threading.Thread(target=start_gateway_publisher, daemon=True)
    gateway_thread.start()


    # Keep the main program alive
    while True:
        time.sleep(1)