---
name: brewlink-external-client
description: >-
  Consumer guide for using BrewLink from external systems (recipe managers, dashboards, Home Assistant, bots, CLIs). Covers discovery, push-button pairing, authentication, the WebSocket / MQTT / HTTP REST APIs, and the per-equipment data schemas (state, telemetry, controls, recipes) of every supported connected equipment model (currently Grainfather G30 Connect).
---

# BrewLink Consumer Skill: Using BrewLink & Its Equipment Schemas

Use this skill when you are building or operating **something that talks to a BrewLink gateway** (not firmware development). It tells you how to find and pair with BrewLink, how to authenticate, how to list the connected brewing equipment, and, for each supported equipment model, exactly which state, telemetry, control actions, and recipe format to use.

It documents only what external clients see. Internal gateway details (Wi-Fi onboarding, BLE scanning, NimBLE, task layout) are intentionally omitted.

## 0. Mental Model

* **Gateway**: one BrewLink ESP32 (`gateway_id`, 12-char uppercase hex). Paired clients talk to it over HTTP REST, WebSocket (`/ws`), or MQTT.
* **Equipment**: up to **3** simultaneously connected Bluetooth devices, addressed by `slot` (1..3), unique `name`, `equipment_type`, or `mac`. List them via `GET /api/equipment` (or WebSocket / MQTT `list_equipment`).
* **No shared schema**: the transport envelope (`req_id`, `action`, `equipment_type`, `slot`, `params`, events such as `telemetry` / `cmd_ack`) is common, but **the contents of `state`, `params`, and `recipe` are defined per equipment model**. Always read the `model` field of the equipment entry and use that model's schema in [Section 6](#6-equipment-schemas-per-model).
* **Workflow**: discover → pair (get `api_token`) → `GET /api/equipment` → pick a device (check `model` and `capabilities`) → read telemetry / send controls / upload recipe using that model's schema → watch `cmd_ack`.
* **Fail-fast**: commands to a disconnected device return `409 EQUIPMENT_OFFLINE` immediately; nothing is queued.

| Supported model (`model`) | `equipment_type` | Schema section |
| :--- | :---: | :--- |
| Grainfather G30 Connect | `boiler` | [6.1](#61-grainfather-g30-connect-boiler) |

*(`fermenter` and `cooler` types are reserved; no real model is supported yet. When a model is added it gets its own subsection in Section 6.)*

---
## 1. Network Broadcast Discovery & Push-Button Pairing

Because the ESP32 gateway operates headlessly when the user holds the physical **`BOOT` button for 3 seconds** (or clicks **Start 60s Pairing Broadcast** in the Web UI / `POST /api/pair/start`), BrewLink supports **code-free push-button pairing** with automatic network broadcast discovery.

While the **60-second** pairing window is active, BrewLink emits a discovery beacon **immediately on start** and then every **2 seconds** until **1 client claims the window** (single-claim lockout) or the 60 seconds expire.

### 1.1. Discovery Broadcast Channels
* **UDP Broadcast (Local LAN & SoftAP)**:
  * **Port**: `53540` (external clients bind a UDP socket to `0.0.0.0:53540` with `SO_REUSEADDR` — and `SO_REUSEPORT` on macOS/BSD if multiple listeners share the port).
  * **Destinations sent by BrewLink**:
    * On **Wi-Fi LAN (STA)**: BrewLink sends to both the **STA subnet-directed broadcast** (e.g. `192.168.1.255:53540`) and the **limited broadcast** (`255.255.255.255:53540`), advertising the gateway's STA IP.
    * On **Setup SoftAP (`BrewLink-Setup-XXXX`)**: Whenever SoftAP clients are connected or STA is offline, BrewLink sends to `192.168.4.255:53540` (plus `255.255.255.255:53540` if STA is offline), advertising `192.168.4.1`.
  * **Firewall note**: Ensure the client host OS allows inbound UDP packets on port `53540` on the private/LAN network profile.
* **MQTT Discovery Topic**:
  * Topic: `brewlink/discovery/<gateway_id>` (clients subscribe to `brewlink/discovery/+`, QoS 0, non-retained).
  * **Discovery-only**: MQTT discovery tells a broker-connected client the gateway's LAN IP and `claim_url`, but **claiming is HTTP-only** (`POST` to `claim_url`). BrewLink does **not** accept UDP reply datagrams or MQTT claim messages.

### 1.2. Discovery Beacon JSON Payload
```json
{
  "event": "brewlink_pairing",
  "gateway_id": "C44F331A8B20",
  "name": "BrewLink Gateway",
  "ip": "192.168.1.50",
  "http_port": 80,
  "claim_url": "http://192.168.1.50/api/pair/claim",
  "ws_url": "ws://192.168.1.50/ws",
  "remaining_sec": 58
}
```

| Field | Type | Meaning |
| :--- | :--- | :--- |
| `event` | string | Always `"brewlink_pairing"`; ignore any datagram without this exact value. |
| `gateway_id` | string | 12-char uppercase hex ESP32 hardware ID. |
| `name` | string | Human-readable gateway label (`"BrewLink Gateway"`). |
| `ip` | string | Gateway IPv4 reachable on the interface that carried this beacon (STA IP on LAN, `"192.168.4.1"` on SoftAP). |
| `http_port` | int | HTTP/WebSocket port (default `80`; included as `:<port>` in URLs if non-80). |
| `claim_url` | string | Exact HTTP endpoint to `POST` to claim this pairing window (`http://<ip>/api/pair/claim`). |
| `ws_url` | string | WebSocket endpoint to connect to after pairing (`ws://<ip>/ws`). |
| `remaining_sec` | int | Seconds remaining in the 60-second broadcast window. |

### 1.3. Step-by-Step Client Auto-Connect Flow During Broadcast
1. **Listen** on UDP `0.0.0.0:53540` (and/or subscribe to MQTT `brewlink/discovery/+` if the client can reach the gateway's LAN IP over HTTP).
2. **Parse & filter** UTF-8 JSON payloads for `beacon.event === "brewlink_pairing"`.
3. **Immediately answer via HTTP `POST`** to `beacon.claim_url` (fallback: `http://<beacon.ip || udp_sender_ip>/api/pair/claim`) with header `Content-Type: application/json` and body:
   ```json
   {"client_name": "MyBrewApp"}
   ```
   * **No PIN required** during the open broadcast window (do not prompt the user for a PIN unless doing manual pairing).
   * **Payload limit**: Keep the JSON body under `256` bytes (`client_name` up to 25 chars before BrewLink appends `-<4hex>`).
   * **Browser / Web App clients**: CORS is enabled (`Access-Control-Allow-Origin: *`, `OPTIONS` preflight supported), so browser clients that know the gateway IP can `fetch(claim_url, { method: "POST", ... })` directly.
4. **Respect single-claim lockout**:
   * **Only 1 client can pair per 60-second broadcast window.** As soon as BrewLink persists the client and returns `200 OK`, it immediately closes the window and stops broadcasting beacons.
   * Stop listening/claiming once you receive `200 OK`.
   * If you receive `403 PAIRING_WINDOW_CLOSED`, another client claimed the window or it expired — ask the user to trigger pairing again (hold `BOOT` 3s or click Start in Web UI).
   * If you receive `500 PAIRING_SAVE_FAILED`, NVS persistence failed and the window stays open — safe to retry briefly.
5. **Persist credentials & connect immediately** (no gateway reboot required):
   * Save `client_name`, `api_token` (`bl_` + 64 hex chars), `gateway_id`, `ws_url`, `mqtt_broker_uri`, `mqtt_topic_key` (`tk_` + 16 hex chars), and `mqtt_topic_prefix`.
   * **HTTP REST**: Immediately call `GET http://<ip>/api/equipment` with `Authorization: Bearer <api_token>` (or `X-BrewLink-Token: <api_token>`).
   * **WebSocket**: Immediately connect to `ws_url` (`ws://<ip>/ws?token=<api_token>` or `Authorization: Bearer <api_token>`).
   * **MQTT**: Immediately connect to `mqtt_broker_uri`, subscribe to `<mqtt_topic_prefix><gateway_id>/equipment/+/telemetry` and `<mqtt_topic_prefix><gateway_id>/equipment`, and publish signed commands to `<mqtt_topic_prefix><gateway_id>/cmd`. BrewLink automatically subscribes to the paired command topics and republishes retained `/status` and `/equipment` on pairing.

### 1.4. Minimal Broadcast Discovery & Auto-Claim Examples

**Python (stdlib only — see also `tools/beerxml_uploader.py --pair`)**:
```python
import json, socket, urllib.request

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("0.0.0.0", 53540))
sock.settimeout(65)

try:
    while True:
        data, (sender_ip, _) = sock.recvfrom(1024)
        try:
            beacon = json.loads(data.decode("utf-8"))
        except json.JSONDecodeError:
            continue
        if beacon.get("event") != "brewlink_pairing":
            continue

        claim_url = beacon.get("claim_url") or f"http://{beacon.get('ip') or sender_ip}/api/pair/claim"
        req = urllib.request.Request(
            claim_url,
            data=json.dumps({"client_name": "MyBrewApp"}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=10) as resp:
            creds = json.loads(resp.read().decode("utf-8"))
        # Persist creds["api_token"], creds["ws_url"], creds["mqtt_topic_prefix"], creds["gateway_id"]
        print("Paired:", creds["client_name"], creds["ws_url"])
        break
finally:
    sock.close()
```

**Node.js (built-in `dgram` + `fetch`)**:
```javascript
const dgram = require('dgram');

const sock = dgram.createSocket({ type: 'udp4', reuseAddr: true });
let claiming = false;

sock.on('message', async (msg, rinfo) => {
  if (claiming) return;
  let beacon;
  try { beacon = JSON.parse(msg.toString('utf8')); } catch { return; }
  if (beacon.event !== 'brewlink_pairing') return;

  claiming = true;
  const claimUrl = beacon.claim_url || `http://${beacon.ip || rinfo.address}/api/pair/claim`;
  try {
    const res = await fetch(claimUrl, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ client_name: 'MyBrewApp' }),
    });
    if (!res.ok) throw new Error(`HTTP ${res.status}: ${await res.text()}`);
    const creds = await res.json();
    console.log('Paired:', creds.client_name, creds.ws_url);
    sock.close();
  } catch (err) {
    claiming = false;
    console.error('Claim failed:', err.message);
  }
});

sock.bind(53540, '0.0.0.0');
```

### 1.5. Base Addressing After Pairing
* **HTTP Base URL**: `http://<gateway-ip>/` (Port `80`)
* **WebSocket URL**: Returned in the pairing response (`ws_url`, e.g. `ws://<gateway-ip>/ws`; uses `192.168.4.1` automatically if claimed over SoftAP)
* **MQTT Broker URI**: Returned in the pairing response (`mqtt_broker_uri`)
* **MQTT Topic Key & Prefix**: Returned in the pairing response (`mqtt_topic_key`, `mqtt_topic_prefix`). Multiple paired clients (up to 8) share the gateway's active `mqtt_topic_key` while each keeps its own unique `api_token`; deleting all paired clients rotates the `mqtt_topic_key`.
* **Gateway Identifier (`gateway_id`)**: 12-character uppercase hex string derived from the ESP32 MAC (e.g. `C44F331A8B20`)

---

## 2. HTTP REST Endpoints for External Clients

All REST endpoints operate on `http://<gateway-ip>/`. Protected endpoints require authentication once paired:
* Header: `Authorization: Bearer <api_token>`
* Custom Header fallback: `x-BrewLink-Token: <api_token>`

### 2.1. External Client URL Catalog

| Method | URL Path | Auth | Purpose |
| :--- | :--- | :---: | :--- |
| **`POST`** | `/api/pair/claim` | None | **Claim pairing session**: Claim open push-button pairing window (code-free or optional PIN) for unique `<client_name>-<suffix>`, `api_token`, and `mqtt_topic_key`. |
| **`GET`** | `/api/pair/status` | None | **Pairing state & client registry**: Verify if clients are paired (`clients` array) or if a pairing window is currently open. |
| **`POST`** | `/api/pair/delete` | None | **Delete paired client**: Remove a paired external client by `client_name` and immediately revoke its `api_token`. |
| **`GET`** | `/api/equipment` | Bearer | **Equipment discovery**: Enumerate all connected brewing devices, MACs, types, and live states. |
| **`POST`** | `/api/equipment/recipe` | Bearer | **Recipe upload**: Transmit an automated recipe to target equipment (format depends on the model, see Section 6). |
| **`POST`** | `/api/equipment/control` | Bearer | **Hardware control**: Send direct commands; supported actions depend on the equipment model (see Section 6). |
| **`GET`** | `/api/status` | None | **Gateway telemetry**: Check gateway online state, active BLE connections, and firmware version. |

---

### 2.2. Endpoint Specifications

#### `POST /api/pair/claim` — Claim Push-Button Pairing Session (Code-Free)
When the 60-second pairing window is open (triggered via physical `BOOT` button hold for 3s), the external client claims the session **without needing a PIN** (physical button press acts as proof of authorization, with immediate single-claim lockout).
* **Unique Client Naming (`<presented_name>-<suffix>`)**: BrewLink takes the `"client_name"` presented by the external client (e.g., `"Brewfather"`), appends a random 4-character hex suffix `-<xxxx>` (e.g., `"Brewfather-A4F2"`), and guarantees that the resulting name is unique across all paired external clients (up to 8 clients).
* **Deletable Clients**: Any paired external client can be deleted from the BrewLink Web UI (`Client Pairing` tab) or via `POST /api/pair/delete` (`{"client_name": "Brewfather-A4F2"}`), which immediately revokes its `api_token`.

* **Request (Code-Free Push-Button Claim)**:
  ```http
  POST /api/pair/claim HTTP/1.1
  Host: <gateway-ip>
  Content-Type: application/json

  {
    "client_name": "Brewfather"
  }
  ```
  *(Note: An optional `"pin": "482910"` field may still be passed if the client prompts the user for the PIN shown in the Web UI, and will be validated if non-empty).*
* **Success Response (`200 OK`)**:
  ```json
  {
    "status": "paired",
    "gateway_id": "C44F331A8B20",
    "client_name": "Brewfather-A4F2",
    "api_token": "bl_8f3a9e2c4b1d6e7f8a0b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f",
    "ws_url": "ws://192.168.1.50/ws",
    "mqtt_broker_uri": "mqtt://192.168.1.100:1883",
    "mqtt_topic_key": "tk_a1b2c3d4e5f6a7b8",
    "mqtt_topic_prefix": "brewlink/tk_a1b2c3d4e5f6a7b8/"
  }
  ```
* **Errors**:
  * `400 Bad Request` — Invalid JSON or payload size >= 256 bytes.
  * `401 Unauthorized` (`INVALID_PIN`) — A non-empty `pin` was provided and did not match.
  * `403 Forbidden` (`PAIRING_WINDOW_CLOSED`) — Pairing window is not open, has expired, or was already claimed by another client.
  * `500 Internal Server Error` (`PAIRING_SAVE_FAILED`) — Gateway failed to persist credentials to NVS (pairing window stays open so the client may retry).
* **Client Action**: Persist `client_name`, `api_token`, `gateway_id`, `ws_url`, `mqtt_topic_key`, and `mqtt_topic_prefix` securely. They do not expire unless deleted or reset on the gateway, and work immediately across HTTP, WebSocket, and MQTT without rebooting the gateway.

---

#### `GET /api/equipment` — List Paired Bluetooth Equipment & State
External clients do **not** need to know BLE MAC addresses ahead of time. Query `GET /api/equipment` to list all paired Bluetooth equipment devices across BrewLink's 3 BLE Central slots (`slot` `1..3`):
* **Guaranteed Unique Equipment Names**: Every paired Bluetooth peripheral has a guaranteed unique `name` across all slots. If two identical models are paired (e.g. two `"Grainfather G30 Connect"` boilers), BrewLink automatically disambiguates the second device by appending its 2-byte MAC suffix (e.g. `"Grainfather G30 Connect-8B22"`).
* **Automatic Power-On Reconnection**: Paired Bluetooth devices are remembered by BrewLink (`paired: true`) and **automatically reconnect** whenever BrewLink boots or when a piece of brewing equipment is powered on. Devices that are paired but currently powered off appear in the list with `"paired": true` and `"connected": false`.

* **Request**:
  ```http
  GET /api/equipment HTTP/1.1
  Host: <gateway-ip>
  Authorization: Bearer <api_token>
  ```
* **Response (`200 OK`)**:
  ```json
  {
    "gateway_id": "C44F331A8B20",
    "count": 1,
    "equipment": [
      {
        "slot": 1,
        "paired": true,
        "connected": true,
        "name": "Grainfather G30 Connect",
        "model": "Grainfather G30 Connect",
        "equipment_type": "boiler",
        "mac": "C4:4F:33:1A:8B:22",
        "rssi": -62,
        "capabilities": {
          "recipe_upload": true,
          "mash_profile": true,
          "fermentation_profile": false,
          "pump_control": true,
          "heat_control": true
        },
        "state": {
          "current_temp_c": 64.8,
          "target_temp_c": 65.0,
          "boil_temp_c": 100.0,
          "heat_active": true,
          "heat_power_pct": 100,
          "pump_active": true,
          "auto_mode": true,
          "ramp_active": false,
          "timer": {
            "active": true,
            "paused": false,
            "remaining_seconds": 2400,
            "total_seconds": 3600
          },
          "stage": {
            "step_index": 1,
            "step_name": "Mash Step 1"
          },
          "interaction": {
            "required": false,
            "code": 0,
            "message": "Normal operation"
          }
        }
      }
    ]
  }
  ```

---

#### `POST /api/equipment/recipe` — Upload Brewing Recipe (Without Needing MAC)
Upload a recipe for automated execution. External clients can target equipment in four ways:
1. **By `equipment_type`** (default: `"boiler"`, `"fermenter"`, or `"cooler"`) — no `slot`, `target_name`, or `target_mac` needed.
2. **By `target_name`** (guaranteed unique equipment `name`, e.g. `"Grainfather G30 Connect"` or `"Grainfather G30 Connect-8B22"`).
3. **By `slot`** (`1`, `2`, or `3` from the equipment list) — no `target_mac` needed.
4. **By `target_mac`** (optional if you prefer MAC addressing).

* **Request** (example uses the G30 recipe schema, Section 6.1.4):
  ```http
  POST /api/equipment/recipe HTTP/1.1
  Host: <gateway-ip>
  Authorization: Bearer <api_token>
  Content-Type: application/json

  {
    "req_id": "req-1029",
    "action": "load_recipe",
    "equipment_type": "boiler",
    "slot": 1,
    "params": {
      "recipe": {
        "name": "West Coast IPA",
        "boil_time_min": 60,
        "hop_stand_min": 20,
        "mash_water_l": 16.5,
        "sparge_water_l": 14.0,
        "mash_steps": [
          {"name": "Mash In", "temp_c": 65.0, "duration_min": 60},
          {"name": "Mash Out", "temp_c": 75.0, "duration_min": 10}
        ],
        "boil_additions_min": [60, 15]
      }
    }
  }
  ```
* **Success Response (`200 OK`)**:
  ```json
  {
    "status": "ok",
    "req_id": "req-1029",
    "mac": "C4:4F:33:1A:8B:22",
    "message": "Recipe transfer queued to equipment"
  }
  ```
* **Fail-Fast Error Response (`409 Conflict`)**:
  Returned immediately if the target BLE equipment is not connected (no stale queueing):
  ```json
  {
    "status": "error",
    "error_code": "EQUIPMENT_OFFLINE",
    "message": "Target equipment is disconnected from BrewLink"
  }
  ```

---

#### `POST /api/equipment/control` — Send Hardware Controls (Without Needing MAC)
Send immediate hardware commands targeting by `equipment_type` or `slot` (or optional `target_mac`):

* **Request**:
  ```http
  POST /api/equipment/control HTTP/1.1
  Host: <gateway-ip>
  Authorization: Bearer <api_token>
  Content-Type: application/json

  {
    "req_id": "req-302",
    "action": "set_pump",
    "equipment_type": "boiler",
    "slot": 1,
    "params": {
      "state": true
    }
  }
  ```
* **Supported Actions & Parameters** (shown for the G30 `boiler`; other models define their own in Section 6):
  * `set_pump`: `params: {"state": true|false}`
  * `set_heat`: `params: {"state": true|false}` (`power_pct` ignored by the G30)
  * `set_target_temp`: `params: {"temp_c": 65.5}`
  * `set_timer`: `params: {"duration_min": 60}`
  * `pause_timer` / `resume_timer` / `cancel_timer`: no params required.
  * `press_set`: press physical SET button on G30 (advance prompt or strike heating).
  * `ack_interaction`: acknowledge prompts (e.g. "Add Grain").
  * `dismiss_alert`: dismiss boil timer / hop addition alerts.
  * `dismiss_sparge_alert`: dismiss sparge prompt.
  * `cancel_session`: abort active automated recipe.

---

## 3. WebSockets URLs & Real-Time Protocol

BrewLink provides a full-duplex WebSocket endpoint on port `80` for bi-directional control, low-latency telemetry streaming, and asynchronous command confirmations.

### 3.1. Connection URLs & Authentication

* **URL (Header Auth)**: `ws://<gateway-ip>/ws`
  * Header: `Authorization: Bearer <api_token>`
* **URL (Query Parameter Auth)**: `ws://<gateway-ip>/ws?token=<api_token>`

---

### 3.2. Client-to-Gateway Messages (Listing Devices, Commands & Recipes)
Clients send UTF-8 text frames containing standard JSON command envelopes.

1. **List Paired Bluetooth Devices (`action: "list_equipment"`)**:
   ```json
   {
     "req_id": "req-ws-list-1",
     "action": "list_equipment"
   }
   ```
   The gateway immediately replies with an `event: "equipment_list"` frame containing all connected/paired BLE devices (`slot`, `name`, `equipment_type`, `mac`, `capabilities`).

2. **Send Command or Recipe (by `equipment_type` or `slot`, no MAC required)**:
   ```json
   {
     "req_id": "req-ws-401",
     "action": "set_target_temp",
     "equipment_type": "boiler",
     "slot": 1,
     "params": {
       "temp_c": 67.0
     }
   }
   ```

---

### 3.3. Gateway-to-Client Messages (Broadcast Events)
The gateway broadcasts real-time events to all authenticated WebSocket clients:

0. **Paired Equipment List (`event: "equipment_list"`)**:
   Sent in response to `{"action": "list_equipment"}` and automatically broadcast whenever a BLE device connects or disconnects:
   ```json
   {
     "event": "equipment_list",
     "req_id": "req-ws-list-1",
     "gateway_id": "C44F331A8B20",
     "count": 1,
     "equipment": [
       {
         "slot": 1,
         "connected": true,
         "name": "Grainfather G30 Connect",
         "model": "Grainfather G30 Connect",
         "equipment_type": "boiler",
         "mac": "C4:4F:33:1A:8B:22",
         "rssi": -60,
         "capabilities": {
           "recipe_upload": true,
           "mash_profile": true,
           "fermentation_profile": false,
           "pump_control": true,
           "heat_control": true
         }
       }
     ]
   }
   ```
1. **Telemetry Stream (`event: "telemetry"`)**:
   Sent at 1Hz or immediately when temperature changes by $\ge 0.2^\circ\text{C}$ or discrete states toggle:
   ```json
   {
     "event": "telemetry",
     "mac": "C44F331A8B22",
     "equipment_type": "boiler",
     "model": "Grainfather G30",
     "rssi": -60,
     "state": {
       "temperature": {"current_c": 65.0, "target_c": 65.0, "boil_c": 100.0},
       "heating": {"active": true, "power_pct": 100, "ramping": false},
       "pump": {"active": true},
       "timer": {"active": true, "paused": false, "remaining_seconds": 2340, "total_seconds": 3600},
       "stage": {"step_index": 1, "step_name": "Mash Step 1", "auto_mode": true},
       "interaction": {"required": false, "code": 0, "message": "Normal operation"}
     },
     "timestamp_ms": 1459200
   }
   ```
2. **Command Acknowledgment (`event: "cmd_ack"`)**:
   Emitted when a command or multi-packet recipe sequence completes transmission over BLE:
   ```json
   {
     "event": "cmd_ack",
     "mac": "C44F331A8B22",
     "req_id": "req-ws-401",
     "action": "set_target_temp",
     "status": "success",
     "error_code": 0
   }
   ```
   *(If transmission fails or peripheral disconnects mid-transfer: `"status": "error"`, `"error_code": 261`)*.
3. **Equipment Lifecycle Manifest (`event: "connected"` / `"disconnected"`)**:
   ```json
   {
     "event": "connected",
     "gateway_id": "C44F331A8B20",
     "mac": "C44F331A8B22",
     "equipment_type": "boiler",
     "model": "Grainfather G30 Connect",
     "slot": 0,
     "rssi": -58,
     "capabilities": {
       "temperature_read": true,
       "temperature_set": true,
       "pump_control": true,
       "heat_control": true,
       "recipe_upload": true
     },
     "timestamp_ms": 1458000
   }
   ```

---

## 4. MQTT Broker Topics & Message Specifications

When external clients integrate via an MQTT broker, BrewLink names all topics under the client's secret `mqtt_topic_key` acquired during pairing:

$$\text{Topic Prefix} = \texttt{brewlink/} + \texttt{<mqtt\_topic\_key>/} + \texttt{<gateway\_id>/}$$

*(Example: `brewlink/tk_a1b2c3d4e5f6a7b8/C44F331A8B20/`)*

### 4.1. MQTT Topic Reference Table

| Topic Pattern | Client Role | QoS | Description |
| :--- | :---: | :---: | :--- |
| `brewlink/discovery/{gateway_id}` | Subscribe | 0 | **Pairing Broadcast**: Published every 2s during the 60s push-button pairing window with `claim_url` and `ip`. |
| `.../equipment` | Subscribe | 1 | **Paired BLE Equipment List (`retain: true`)**: JSON array of all connected/paired BLE devices (`slot`, `name`, `equipment_type`, `mac`, `capabilities`). Updated automatically on connect/disconnect or when `{"action": "list_equipment"}` is sent to `.../cmd`. |
| `.../cmd` | Publish | 1 | **MAC-Free Gateway Command Topic**: Send commands, recipes, or `list_equipment` targeting by `equipment_type` or `slot` (no MAC needed in topic). |
| `.../equipment/{MAC}/cmd` | Publish | 1 | Send command/recipe to specific equipment MAC (requires HMAC signature). |
| `.../equipment/{MAC}/telemetry` | Subscribe | 0 | Inbound telemetry stream for live dashboards (or subscribe to `.../equipment/+/telemetry` without knowing MACs). |
| `.../equipment/{MAC}/announce` | Subscribe | 1 | Equipment connected/disconnected lifecycle events (`.../equipment/+/announce`). |
| `.../equipment/{MAC}/snapshot` | Subscribe | 1 | State snapshot published when gateway establishes broker connection. |
| `.../equipment/{MAC}/cmd_ack` | Subscribe | 1 | Completion status & latency acknowledgment for published commands (`.../equipment/+/cmd_ack`). |
| `.../status` | Subscribe | 1 | Gateway online/offline status (Last Will and Testament, `retain: true`). |

---

### 4.2. MQTT Command Authentication (HMAC-SHA256)

Because MQTT command topics can be intercepted on shared brokers, all commands published by external clients must include an HMAC-SHA256 signature in the JSON payload:

* **Signing String**: `"{req_id}:{action}:{timestamp}"`
* **Secret Key**: `api_token` (`bl_...`)
* **Signature**: Hex-encoded lowercase HMAC-SHA256 (64 characters)

**Payload Example**:
```json
{
  "req_id": "req-mqtt-9912",
  "action": "load_recipe",
  "equipment_type": "boiler",
  "target_mac": "C44F331A8B22",
  "timestamp": 1712678000,
  "sig": "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
  "params": {
    "recipe": { ... }
  }
}
```

---

## 5. Client Implementation Cheatsheets

### 5.1. Node.js / JavaScript WebSocket Client
```javascript
const WebSocket = require('ws');

const GATEWAY_IP = "192.168.1.50";
const API_TOKEN = "bl_8f3a9e2c4b1d6e7f...";

const ws = new WebSocket(`ws://${GATEWAY_IP}/ws?token=${API_TOKEN}`);

ws.on('open', () => {
  console.log('Connected to BrewLink WebSocket');
  
  // Send pump command
  ws.send(JSON.stringify({
    req_id: `req-${Date.now()}`,
    action: "set_pump",
    equipment_type: "boiler",
    params: { state: true }
  }));
});

ws.on('message', (raw) => {
  const msg = JSON.parse(raw);
  if (msg.event === 'telemetry') {
    console.log(`Temp: ${msg.state.temperature.current_c}°C / Stage: ${msg.state.stage.step_name}`);
  } else if (msg.event === 'cmd_ack') {
    console.log(`Command ${msg.action} (${msg.req_id}) -> ${msg.status}`);
  }
});
```

### 5.2. Python MQTT Client (`paho-mqtt`)
```python
import paho.mqtt.client as mqtt
import json, time, hmac, hashlib

GATEWAY_ID = "C44F331A8B20"
TOPIC_KEY  = "tk_a1b2c3d4e5f6a7b8"
TARGET_MAC = "C44F331A8B22"
API_TOKEN  = "bl_8f3a9e2c4b1d6e7f..."

client = mqtt.Client(client_id="external_agent")
client.connect("192.168.1.100", 1883, 60)

# Subscribe to telemetry & command acks
client.subscribe(f"brewlink/{TOPIC_KEY}/{GATEWAY_ID}/equipment/{TARGET_MAC}/telemetry")
client.subscribe(f"brewlink/{TOPIC_KEY}/{GATEWAY_ID}/equipment/{TARGET_MAC}/cmd_ack")

# Publish signed command
req_id = f"req-{int(time.time())}"
action = "set_target_temp"
ts = int(time.time())
sig = hmac.new(API_TOKEN.encode(), f"{req_id}:{action}:{ts}".encode(), hashlib.sha256).hexdigest()

payload = json.dumps({
    "req_id": req_id,
    "action": action,
    "timestamp": ts,
    "sig": sig,
    "params": {"temp_c": 66.5}
})

topic = f"brewlink/{TOPIC_KEY}/{GATEWAY_ID}/equipment/{TARGET_MAC}/cmd"
client.publish(topic, payload, qos=1)
```

---

## 6. Equipment Schemas (Per Model)

Each supported equipment model has **its own schema**; schemas are never shared between models. Identify the model from the `model` field in `GET /api/equipment`, the `connected` event, or telemetry, then use the matching subsection.

### 6.1. Grainfather G30 Connect (`boiler`)

* `model`: `"Grainfather G30 Connect"` (telemetry may report `"Grainfather G30"`)
* `equipment_type`: `"boiler"` (default target when `equipment_type` is omitted)
* Capabilities: `recipe_upload`, `mash_profile`, `pump_control`, `heat_control`; `fermentation_profile` is **false**.

#### 6.1.1. State (`state` in `GET /api/equipment`)

| Field | Type | Meaning |
| :--- | :--- | :--- |
| `current_temp_c` | number | Current wort temperature (°C) |
| `target_temp_c` | number | Active setpoint (°C) |
| `boil_temp_c` | number | Configured boil temperature (default 100) |
| `heat_active` | bool | Heating element on |
| `heat_power_pct` | int 0..100 | Heater power |
| `pump_active` | bool | Pump running |
| `auto_mode` | bool | `true` = running a recipe, `false` = manual |
| `ramp_active` | bool | Heating up to the next step's setpoint |
| `delayed_heat` | bool | A delayed heating start is scheduled |
| `interrupted` / `manual_power` / `sparge_alert` | bool | G30 `W` flags: process interrupted, manual power mode, sparge alert active |
| `error_code` | int | Last controller error code (`E` notification), `0` = none |
| `timer.active/paused` | bool | Timer state |
| `timer.remaining_seconds/total_seconds` | int | Timer progress |
| `stage.step_index` | int | Controller stage (1..N = mash steps) |
| `stage.step_name` | string (≤23) | e.g. `Add Strike Water`, `Add Grain`, `Mash Step N`, `Sparge`, `Boiling`, `Cooling`, `Idle`, `Manual Mode` |
| `interaction.required` | bool | G30 is waiting for the brewer |
| `interaction.code` | int | See table below |
| `interaction.message` | string (≤63) | Human-readable prompt |

**Interaction codes**

| Code | Meaning | Respond with |
| :---: | :--- | :--- |
| 0 | Normal operation | – |
| 1 | Add mash water, then press SET to start heating | `press_set` |
| 2 | Strike temperature reached: add grain | `ack_interaction` |
| 3 | Mash finished: sparge alert | `dismiss_sparge_alert` / `ack_interaction` |
| 4 | Sparge: add sparge water, SET skips to boil | `press_set` / `dismiss_sparge_alert` |
| 5 | Boil addition alert (hops/adjuncts) | `dismiss_alert` |
| 6 | Boil finished: start cooling | `ack_interaction` |

#### 6.1.2. Telemetry (WebSocket `event:"telemetry"`, MQTT `.../equipment/{MAC}/telemetry`)

```json
{
  "event": "telemetry", "mac": "C44F331A8B22", "name": "Grainfather G30 Connect",
  "equipment_type": "boiler", "model": "Grainfather G30 Connect", "rssi": -60,
  "state": {
    "temperature": {"current_c": 65.0, "target_c": 65.0, "boil_c": 100.0},
    "heating": {"active": true, "power_pct": 100, "ramping": false},
    "pump": {"active": true},
    "timer": {"active": true, "paused": false, "remaining_seconds": 2340, "total_seconds": 3600},
    "stage": {"step_index": 1, "step_name": "Mash Step 1", "auto_mode": true},
    "interaction": {"required": false, "code": 0, "message": "Normal operation"},
    "flags": {"delayed_heat": false, "interrupted": false, "manual_power": false, "sparge_alert": false},
    "error_code": 0
  },
  "timestamp_ms": 1459200
}
```
Note: telemetry groups fields (`temperature`, `heating`, `pump`, ...; extra flags under `flags`) while `GET /api/equipment` returns the flat state fields of 6.1.1. Same data, different shape.

#### 6.1.3. Control actions (`POST /api/equipment/control`, WebSocket, or MQTT `.../cmd`)

| `action` | `params` | Effect |
| :--- | :--- | :--- |
| `set_pump` | `{"state": bool}` | Pump on/off |
| `set_heat` | `{"state": bool}` | Heater on/off. (`power_pct` is accepted but **ignored**: the G30 has no power command; read `heat_power_pct` for the controller-managed output) |
| `set_target_temp` | `{"temp_c": number}` | Setpoint |
| `temp_up` / `temp_down` | none | Raise / lower the setpoint by 1 °C |
| `delayed_heat` | `{"minutes": 0-999, "seconds": 0-59}` | Schedule a delayed heating start (see `delayed_heat` state flag) |
| `set_timer` | `{"duration_min": int}` | Start timer |
| `pause_timer` | none | Pause the timer. Rejected (`INVALID_STATE`) if already paused |
| `resume_timer` | none | Resume the timer. Rejected if not paused |
| `toggle_timer` | none | Raw G30 pause/resume toggle |
| `cancel_timer` | none | Cancel the timer |
| `press_set` | none | Press the physical SET button (advance prompt / start strike heating) |
| `ack_interaction` | none | Acknowledge a prompt (e.g. grain added) |
| `dismiss_alert` | none | Dismiss boil / hop addition alert |
| `dismiss_sparge_alert` | none | Dismiss sparge prompt |
| `cancel_session` | none | Abort the active recipe |
| `disconnect_manual` / `disconnect_auto` | none | Leave manual / auto mode on the G30 without cancelling the session |
| `load_recipe` | `{"recipe": {...}}` | Upload recipe (6.1.4) |

While a recipe is running (`auto_mode: true`), `set_timer`, `cancel_timer`, `set_target_temp`, `temp_up` and `temp_down` are rejected because the G30 is master of the process.

**Not available** (not implemented by BrewLink): firmware version query, and recipe-level options (water-treatment alert, sparge counter/alert, boil power, skip-start) are fixed defaults. Recipe names are upper-cased, stripped to `A-Z 0-9 space - _`, and truncated to 19 characters on the G30.

Completion is reported by `cmd_ack` (`status: success|error`, `error_code`; `261` = BLE transfer failed / device disconnected mid-transfer). G30 commands are paced by the gateway (150 ms between BLE packets); a recipe upload is a multi-packet sequence so `cmd_ack` arrives after the last packet.

#### 6.1.4. Recipe (`params.recipe` for `load_recipe`)

| Field | Type | Limits | Notes |
| :--- | :--- | :--- | :--- |
| `name` | string | ≤ 31 chars | Recipe name |
| `boil_time_min` | int | – | Boil duration |
| `hop_stand_min` | int | – | Hop stand duration |
| `mash_water_l` | number | – | Strike water volume (L) |
| `sparge_water_l` | number | – | Sparge water volume (L) |
| `mash_steps[]` | `{name?, temp_c, duration_min}` | max **8** | Defaults 65 °C / 60 min if omitted |
| `boil_additions_min[]` | int[] | max **8** | Minutes remaining in boil at which each addition alert fires |
| `fermentation_steps[]`, `target_og_sg`, `target_fg_sg` | – | – | Accepted but **ignored by the G30** (it is not a fermenter) |

BrewLink transmits the recipe as provided and does not validate brewing sense. Convert recipes from other formats (e.g. BeerXML, BeerJSON) to this format on the client side before uploading. Typical brew-day flow after upload: `interaction.code 1` → `press_set` → heat to strike → code 2 → `ack_interaction` → mash steps → sparge prompts (3/4) → boil with addition alerts (5) → cooling (6).

---

## 7. Adding Another Equipment Model (for consumers of future versions)

When BrewLink adds support for a new model (for example a fermenter), it ships with its own subsection in Section 6 documenting its state, telemetry, control actions, and recipe/profile. Never assume a fermenter or cooler uses G30 field names; read the `model` and `capabilities` of each entry in the equipment list and use that model's schema only.
