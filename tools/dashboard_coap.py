"""
IoT Systems Design - Lab Dashboard (Thread / CoAP / CBOR Version)
Application and Service Domain (ASD) Server

Reads CoAP Observe notifications from /env/temp by parsing the OpenThread CLI
monitor output of Node B (the client running `coap observe`). This avoids the
need for a Border Router (which arrives in Lab 5) — Node B is the bridge.

Usage:
    # Terminal 1 — keep `idf.py monitor` running on Node B with `coap observe`
    # active and the CLI logs streaming. Then redirect that monitor output to
    # a file (most ESP-IDF monitors support --print-filter or you can use
    # `tee`):
    idf.py -p /dev/ttyUSB1 monitor | tee /tmp/nodeB.log

    # Terminal 2 — point the dashboard at the same log file:
    python tools/dashboard_coap.py --log /tmp/nodeB.log

The dashboard tails the log, decodes the CBOR payload of each Observe
notification, and updates the same Chart.js telemetry view as the HTTP and
MQTT dashboards from Labs 0 and 0.5.
"""

import argparse
import logging
import re
import struct
import threading
import time
from collections import deque
from pathlib import Path

from flask import Flask, jsonify, render_template_string

app = Flask(__name__)
log = logging.getLogger("werkzeug")
log.setLevel(logging.ERROR)

# --- CoAP / CBOR Configuration ---
COAP_RESOURCE = "/env/temp"
CONTENT_FORMAT = 60  # application/cbor

# --- Shared state (protected by lock) ---
state_lock = threading.Lock()
latest_sensor_data = {"error": "Waiting for first Observe notification..."}
notify_count = 0
start_time = time.time()
recent_payloads = deque(maxlen=20)  # last 20 raw CBOR hex strings, for evidence

# --- CBOR decoder for the {"t": float16} contract from SOP-03 ---

def float16_to_float(h: int) -> float:
    """Decode IEEE 754 half-precision (16-bit) to Python float."""
    sign = (h >> 15) & 0x1
    exp = (h >> 10) & 0x1F
    mant = h & 0x3FF
    if exp == 0:
        if mant == 0:
            return -0.0 if sign else 0.0
        # subnormal
        val = mant / 1024.0 * 2 ** (-14)
    elif exp == 31:
        return float("nan") if mant else (float("-inf") if sign else float("inf"))
    else:
        val = (1 + mant / 1024.0) * 2 ** (exp - 15)
    return -val if sign else val


def decode_env_temp_cbor(payload: bytes):
    """Decode the 6-byte CBOR object {"t": <float16>} produced by SOP-03 §5.

    Returns the float on success, or None if the bytes don't match the contract.
    """
    if len(payload) != 6:
        return None
    if payload[0] != 0xA1:               # map(1)
        return None
    if payload[1] != 0x61 or payload[2] != ord("t"):  # text(1) "t"
        return None
    if payload[3] != 0xF9:               # float16 tag
        return None
    h = struct.unpack(">H", payload[4:6])[0]
    return float16_to_float(h)


# --- Log tailing thread ---

# OpenThread CLI prints CoAP responses on Node B as e.g.:
#   coap response from fdde:ad00:beef::1 with payload: a16174f94e40
# and Observe notifications as e.g.:
#   coap notification from fdde:ad00:beef::1 with payload: a16174f94e40
# We capture either form.
PAYLOAD_RE = re.compile(
    r"coap (?:response|notification).*payload[: ]+([0-9a-fA-F]+)",
    re.IGNORECASE,
)


def tail_log(path: Path):
    """Follow the file like `tail -f` and parse CoAP payload lines."""
    global latest_sensor_data, notify_count

    while not path.exists():
        print(f"[CoAP] Waiting for log file: {path}")
        time.sleep(1)

    with path.open("r", errors="ignore") as f:
        f.seek(0, 2)  # end of file
        while True:
            line = f.readline()
            if not line:
                time.sleep(0.2)
                continue

            m = PAYLOAD_RE.search(line)
            if not m:
                continue

            hex_payload = m.group(1)
            try:
                payload = bytes.fromhex(hex_payload)
            except ValueError:
                continue

            temp = decode_env_temp_cbor(payload)
            if temp is None:
                print(f"[CoAP] Unrecognized payload (not env-reading): {hex_payload}")
                continue

            with state_lock:
                latest_sensor_data = {
                    "temperature": round(temp, 2),
                    "raw_cbor": hex_payload,
                    "bytes": len(payload),
                }
                notify_count += 1
                recent_payloads.append(hex_payload)

            print(f"[CoAP] /env/temp = {temp:.2f} C  (CBOR: {hex_payload})")


# --- Frontend (User Domain) — same skeleton as Lab 0 / Lab 0.5 dashboards ---

HTML_TEMPLATE = """
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>IoT Lab: CoAP Dashboard</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        body { font-family: system-ui, sans-serif; background-color: #f4f4f9; padding: 2rem; max-width: 800px; margin: 0 auto; color: #333; }
        .card { background: white; padding: 1.5rem; border-radius: 8px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); margin-bottom: 1.5rem; }
        .status { font-family: monospace; font-size: 0.9rem; color: #666; }
        .badge { background: #e9ecef; padding: 0.25rem 0.5rem; border-radius: 4px; font-size: 0.85rem; font-family: monospace; }
        .proto-note { background: #fff3cd; padding: 0.75rem 1rem; border-radius: 4px; font-size: 0.85rem; border-left: 4px solid #ffc107; margin-bottom: 1.5rem; }
        .stats { display: flex; gap: 1rem; margin-top: 1rem; }
        .stat { flex: 1; background: #e9ecef; padding: 0.75rem; border-radius: 4px; text-align: center; }
        .stat-value { font-size: 1.5rem; font-weight: bold; color: #0056b3; font-family: monospace; }
        .stat-label { font-size: 0.8rem; color: #666; margin-top: 0.25rem; }
        .raw { font-family: monospace; font-size: 0.85rem; color: #555; word-break: break-all; }
    </style>
</head>
<body>
    <h2>IoT Systems Design Lab: CoAP Implementation</h2>
    <p class="status">Resource: <span class="badge">{{ resource }}</span> Content-Format: <span class="badge">{{ content_format }} (application/cbor)</span></p>

    <div class="proto-note">
        <strong>Protocol:</strong> Unlike the HTTP and MQTT dashboards, this page does not poll
        anything and there is no broker. Data arrives via CoAP Observe (RFC 7641) — the ESP32
        <em>pushes</em> a 6-byte CBOR payload to <code>{{ resource }}</code> only when the temperature
        changes by more than 0.5&nbsp;&deg;C. Notifications travel through the Thread mesh; this
        dashboard reads them from Node B's serial monitor log.
    </div>

    <div class="card">
        <h3>Sensing Capability (CoAP Observe: {{ resource }})</h3>
        <canvas id="telemetryChart" height="100"></canvas>
        <p class="status" id="conn-status">Waiting for data...</p>
        <div class="stats">
            <div class="stat">
                <div class="stat-value" id="notify-count">0</div>
                <div class="stat-label">Observe notifications received</div>
            </div>
            <div class="stat">
                <div class="stat-value" id="poll-saved">0</div>
                <div class="stat-label">Equivalent 1&nbsp;Hz polls avoided</div>
            </div>
            <div class="stat">
                <div class="stat-value" id="bytes-per">6 B</div>
                <div class="stat-label">CBOR payload size</div>
            </div>
        </div>
        <p class="status" style="margin-top: 1rem;">Last raw CBOR: <span class="raw" id="raw-cbor">&mdash;</span></p>
    </div>

    <div class="card">
        <h3>Actuating Capability</h3>
        <p class="status">Not implemented in Lab 3 &mdash; arrives in Lab 4 (downlink valve control with CoAP CON + Mailbox pattern).</p>
    </div>

    <script>
        const ctx = document.getElementById('telemetryChart').getContext('2d');
        const chart = new Chart(ctx, {
            type: 'line',
            data: {
                labels: [],
                datasets: [{
                    label: 'Temperature (C)',
                    borderColor: '#0056b3',
                    data: [],
                    fill: false,
                    tension: 0.1
                }]
            },
            options: { animation: false, scales: { y: { beginAtZero: false } } }
        });

        function updateChart(temp) {
            const time = new Date().toLocaleTimeString();
            chart.data.labels.push(time);
            chart.data.datasets[0].data.push(temp);
            if (chart.data.labels.length > 20) {
                chart.data.labels.shift();
                chart.data.datasets[0].data.shift();
            }
            chart.update();
        }

        function fetchSensorData() {
            fetch('/api/sensor')
                .then(res => res.json())
                .then(data => {
                    const status = document.getElementById('conn-status');
                    if (data.error) {
                        status.innerText = "Waiting: " + data.error;
                        status.style.color = "orange";
                    } else if (data.temperature !== undefined) {
                        status.innerText = "Connected. Receiving CoAP Observe notifications.";
                        status.style.color = "green";
                        updateChart(data.temperature);
                        document.getElementById('raw-cbor').innerText = data.raw_cbor;
                    }
                });
        }

        function fetchStats() {
            fetch('/api/stats')
                .then(res => res.json())
                .then(s => {
                    document.getElementById('notify-count').innerText = s.notify_count;
                    document.getElementById('poll-saved').innerText = s.polls_avoided;
                });
        }

        // CoAP Observe is push-based, but the browser still polls Flask for the latest
        // value held in memory. The point is that the *radio side* did no polling.
        setInterval(fetchSensorData, 1000);
        setInterval(fetchStats, 1000);
    </script>
</body>
</html>
"""


@app.route("/")
def index():
    return render_template_string(
        HTML_TEMPLATE,
        resource=COAP_RESOURCE,
        content_format=CONTENT_FORMAT,
    )


@app.route("/api/sensor", methods=["GET"])
def api_sensor():
    with state_lock:
        return jsonify(latest_sensor_data)


@app.route("/api/stats", methods=["GET"])
def api_stats():
    with state_lock:
        elapsed = max(1, int(time.time() - start_time))
        polls_avoided = elapsed - notify_count  # what 1 Hz polling would have spent
        if polls_avoided < 0:
            polls_avoided = 0
        return jsonify(
            {
                "notify_count": notify_count,
                "polls_avoided": polls_avoided,
                "elapsed_seconds": elapsed,
                "recent_payloads": list(recent_payloads),
            }
        )


def main():
    parser = argparse.ArgumentParser(
        description="CoAP / CBOR dashboard — reads Observe notifications from "
        "Node B's monitor log."
    )
    parser.add_argument(
        "--log",
        required=True,
        type=Path,
        help="Path to the file holding Node B's `idf.py monitor` output. "
        "Tip: run `idf.py -p <port> monitor | tee /tmp/nodeB.log` and pass that path.",
    )
    parser.add_argument("--port", type=int, default=5000, help="Dashboard HTTP port")
    args = parser.parse_args()

    t = threading.Thread(target=tail_log, args=(args.log,), daemon=True)
    t.start()

    print(f"[*] CoAP Dashboard running.")
    print(f"[*] Resource: {COAP_RESOURCE}  (Content-Format = {CONTENT_FORMAT}, application/cbor)")
    print(f"[*] Tailing log: {args.log}")
    print(f"[*] Open http://localhost:{args.port} in your browser")

    app.run(host="0.0.0.0", port=args.port)


if __name__ == "__main__":
    main()