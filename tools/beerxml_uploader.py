#!/usr/bin/env python3
"""
BrewLink BeerXML 1.0 Recipe Converter & External Uploader Reference Client.

Supports:
1. Converting BeerXML 1.0 (.xml) recipes into the compact BrewLink Unified Recipe JSON
   schema (compatible with Boilers like Grainfather G30, Fermenters, and Coolers).
2. Automatic network discovery & zero-code push-button pairing (`--pair`) by listening
   for BrewLink UDP broadcast beacons on port `53540` and claiming `POST /api/pair/claim`
   (or claiming directly via `--host` / optional `--pair-pin`).
3. Uploading recipes to BrewLink over:
   - HTTP REST (`POST /api/equipment/recipe`)
   - Full-Duplex WebSockets (`ws://<host>/ws`)
   - MQTT (`brewlink/<topic_key>/<gateway_id>/equipment/<mac>/cmd` with HMAC-SHA256 signing)
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import socket
import sys
import time
import urllib.error
import urllib.request
import uuid
import xml.etree.ElementTree as ET
from typing import Any, Dict, List, Optional

BREWLINK_DISCOVERY_UDP_PORT = 53540


def _find_text(elem: Optional[ET.Element], tag: str, default: str = "") -> str:
    if elem is None:
        return default
    child = elem.find(tag)
    if child is not None and child.text:
        return child.text.strip()
    return default


def _find_float(elem: Optional[ET.Element], tag: str, default: float = 0.0) -> float:
    txt = _find_text(elem, tag, "")
    if not txt:
        return default
    try:
        return float(txt)
    except ValueError:
        return default


def parse_beerxml_to_brewlink_recipe(xml_content: str) -> Dict[str, Any]:
    """
    Parse a BeerXML 1.0 XML string and return a BrewLink Unified Recipe dictionary.
    Follows the Grainfather G30 water calculation and hop addition deduplication rules
    while also extracting fermentation steps and gravities for fermenters/chillers.
    """
    root = ET.fromstring(xml_content)
    recipe_el = root.find("RECIPE") if root.tag.upper() == "RECIPES" else root
    if recipe_el is None or recipe_el.tag.upper() != "RECIPE":
        raise ValueError("No <RECIPE> element found in BeerXML document")

    name = _find_text(recipe_el, "NAME", "Beer Recipe")[:31]
    boil_time_min = int(round(_find_float(recipe_el, "BOIL_TIME", 60.0)))
    if boil_time_min <= 0:
        boil_time_min = 60

    # 1. Mash Steps (max 8)
    mash_steps: List[Dict[str, Any]] = []
    mash_el = recipe_el.find("MASH")
    mash_steps_parent = mash_el.find("MASH_STEPS") if mash_el is not None else None
    step_nodes = (
        mash_steps_parent.findall("MASH_STEP")
        if mash_steps_parent is not None
        else recipe_el.findall(".//MASH_STEP")
    )
    for idx, step_node in enumerate(step_nodes[:8]):
        step_name = _find_text(step_node, "NAME", f"Step {idx + 1}")[:23]
        step_temp = round(_find_float(step_node, "STEP_TEMP", 65.0), 1)
        step_time = int(round(_find_float(step_node, "STEP_TIME", 60.0)))
        mash_steps.append(
            {
                "name": step_name,
                "temp_c": step_temp,
                "duration_min": max(0, step_time),
            }
        )

    # 2. Boil Additions & Hop Stand (max 8 unique stops in descending order)
    boil_stops_set = set()
    hop_stand_min = 0
    for hop_node in recipe_el.findall(".//HOP"):
        use = _find_text(hop_node, "USE", "Boil").lower()
        hop_time = int(round(_find_float(hop_node, "TIME", 0.0)))
        if use == "boil" and 0 < hop_time <= boil_time_min:
            boil_stops_set.add(hop_time)
        elif use in ("aroma", "whirlpool") and hop_time > hop_stand_min:
            hop_stand_min = hop_time

    for misc_node in recipe_el.findall(".//MISC"):
        use = _find_text(misc_node, "USE", "").lower()
        misc_time = int(round(_find_float(misc_node, "TIME", 0.0)))
        if use == "boil" and 0 < misc_time <= boil_time_min:
            boil_stops_set.add(misc_time)

    boil_additions_min = sorted(boil_stops_set, reverse=True)[:8]

    # 3. Mash & Sparge Water Calculation (Grainfather G30 formula with BeerXML overrides)
    total_grain_kg = 0.0
    for ferm_node in recipe_el.findall(".//FERMENTABLE"):
        f_type = _find_text(ferm_node, "TYPE", "Grain").lower()
        amt = _find_float(ferm_node, "AMOUNT", 0.0)
        if amt > 0.0 and f_type in ("grain", "adjunct"):
            total_grain_kg += amt

    mash_water_l = 16.5
    sparge_water_l = 14.0
    if total_grain_kg > 0.0:
        mash_water_l = round((total_grain_kg * 2.7) + 3.5, 1)
        boil_size_l = _find_float(recipe_el, "BOIL_SIZE", 0.0)
        batch_size_l = _find_float(recipe_el, "BATCH_SIZE", 20.0)
        target_preboil_l = boil_size_l if boil_size_l > 0.0 else (batch_size_l * 1.15)
        sparge_water_l = max(
            0.0, round(target_preboil_l - mash_water_l + (total_grain_kg * 0.8), 1)
        )

    if step_nodes:
        first_infuse = _find_float(step_nodes[0], "INFUSE_AMOUNT", 0.0)
        if first_infuse > 0.0:
            mash_water_l = round(first_infuse, 1)

    explicit_mash_w = _find_float(recipe_el, "MASH_WATER", -1.0)
    if explicit_mash_w >= 0.0:
        mash_water_l = round(explicit_mash_w, 1)

    explicit_sparge_w = _find_float(recipe_el, "SPARGE_WATER", -1.0)
    if explicit_sparge_w >= 0.0:
        sparge_water_l = round(explicit_sparge_w, 1)

    # 4. Fermentation Steps (Primary / Secondary / Tertiary for Fermenters & Chillers)
    fermentation_steps: List[Dict[str, Any]] = []
    ferm_stages = int(round(_find_float(recipe_el, "FERMENTATION_STAGES", 1.0)))
    pri_age = int(round(_find_float(recipe_el, "PRIMARY_AGE", 0.0)))
    pri_temp = _find_float(recipe_el, "PRIMARY_TEMP", -999.0)
    if pri_age > 0 and pri_temp > -100.0:
        fermentation_steps.append(
            {"name": "Primary", "temp_c": round(pri_temp, 1), "duration_days": pri_age}
        )

    if ferm_stages >= 2:
        sec_age = int(round(_find_float(recipe_el, "SECONDARY_AGE", 0.0)))
        sec_temp = _find_float(recipe_el, "SECONDARY_TEMP", -999.0)
        if sec_age > 0 and sec_temp > -100.0:
            fermentation_steps.append(
                {
                    "name": "Secondary",
                    "temp_c": round(sec_temp, 1),
                    "duration_days": sec_age,
                }
            )

    if ferm_stages >= 3:
        ter_age = int(round(_find_float(recipe_el, "TERTIARY_AGE", 0.0)))
        ter_temp = _find_float(recipe_el, "TERTIARY_TEMP", -999.0)
        if ter_age > 0 and ter_temp > -100.0:
            fermentation_steps.append(
                {
                    "name": "Tertiary",
                    "temp_c": round(ter_temp, 1),
                    "duration_days": ter_age,
                }
            )

    target_og = round(_find_float(recipe_el, "OG", 0.0), 3)
    target_fg = round(_find_float(recipe_el, "FG", 0.0), 3)

    return {
        "name": name,
        "boil_time_min": boil_time_min,
        "hop_stand_min": hop_stand_min,
        "mash_water_l": mash_water_l,
        "sparge_water_l": sparge_water_l,
        "mash_steps": mash_steps,
        "boil_additions_min": boil_additions_min,
        "fermentation_steps": fermentation_steps,
        "target_og_sg": target_og,
        "target_fg_sg": target_fg,
    }


def compute_mqtt_signature(api_token: str, req_id: str, action: str, timestamp: int) -> str:
    """Compute HMAC-SHA256 signature for BrewLink MQTT command authentication."""
    msg = f"{req_id}:{action}:{timestamp}".encode("utf-8")
    return hmac.new(api_token.encode("utf-8"), msg, hashlib.sha256).hexdigest()


def build_command_envelope(
    recipe: Dict[str, Any],
    equipment_type: str = "boiler",
    target_mac: Optional[str] = None,
    slot: Optional[int] = None,
    target_name: Optional[str] = None,
    api_token: Optional[str] = None,
    req_id: Optional[str] = None,
) -> Dict[str, Any]:
    """Build the unified BrewLink command envelope for HTTP, WebSocket, or MQTT."""
    rid = req_id or f"req-{uuid.uuid4().hex[:8]}"
    ts = int(time.time())
    envelope: Dict[str, Any] = {
        "req_id": rid,
        "action": "load_recipe",
        "equipment_type": equipment_type,
        "timestamp": ts,
        "params": {"recipe": recipe},
    }
    if slot is not None and slot >= 1:
        envelope["slot"] = slot
    if target_name:
        envelope["target_name"] = target_name
    if target_mac:
        envelope["target_mac"] = target_mac
    if api_token:
        envelope["sig"] = compute_mqtt_signature(api_token, rid, "load_recipe", ts)
    return envelope


def list_paired_equipment_http(host: str, api_token: str) -> Dict[str, Any]:
    """List all paired/connected BLE equipment devices on BrewLink (`GET /api/equipment`)."""
    url = f"http://{host}/api/equipment"
    req = urllib.request.Request(
        url,
        headers={"Authorization": f"Bearer {api_token}"},
        method="GET",
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        err_body = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {exc.code}: {err_body}") from exc


def claim_pairing(
    host: str,
    pin: Optional[str] = None,
    client_name: str = "BeerXML Uploader",
    claim_url: Optional[str] = None,
) -> Dict[str, Any]:
    """Claim an open BrewLink pairing window (zero-code push-button or optional PIN)."""
    url = claim_url or f"http://{host}/api/pair/claim"
    payload: Dict[str, Any] = {"client_name": client_name}
    if pin:
        payload["pin"] = pin
    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.loads(resp.read().decode("utf-8"))


def discover_and_claim_pairing(
    client_name: str = "BeerXML Uploader",
    timeout_sec: int = 65,
    udp_port: int = BREWLINK_DISCOVERY_UDP_PORT,
) -> Dict[str, Any]:
    """
    Listen on UDP port 53540 for a BrewLink pairing broadcast beacon
    (`{"event": "brewlink_pairing", ...}`) and automatically claim pairing
    without needing the gateway IP or a PIN code.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", udp_port))
    sock.settimeout(timeout_sec)

    print(
        f"Listening on UDP :{udp_port} for BrewLink pairing broadcast "
        f"(hold BOOT button on ESP32 for 3s)...",
        file=sys.stderr,
    )
    try:
        while True:
            data, addr = sock.recvfrom(1024)
            try:
                beacon = json.loads(data.decode("utf-8", errors="ignore"))
            except json.JSONDecodeError:
                continue
            if beacon.get("event") == "brewlink_pairing":
                gw_ip = beacon.get("ip") or addr[0]
                claim_url = beacon.get("claim_url") or f"http://{gw_ip}/api/pair/claim"
                print(
                    f"Discovered BrewLink gateway '{beacon.get('gateway_id')}' at {gw_ip} "
                    f"({beacon.get('remaining_sec')}s remaining). Claiming...",
                    file=sys.stderr,
                )
                return claim_pairing(
                    host=gw_ip,
                    pin=None,
                    client_name=client_name,
                    claim_url=claim_url,
                )
    finally:
        sock.close()


def claim_pairing_pin(host: str, pin: str, client_name: str = "BeerXML Uploader") -> Dict[str, Any]:
    """Exchange a 6-digit pairing PIN for a persistent api_token and mqtt_topic_key."""
    return claim_pairing(host=host, pin=pin, client_name=client_name)


def upload_recipe_http(
    host: str,
    api_token: str,
    recipe: Dict[str, Any],
    equipment_type: str = "boiler",
    target_mac: Optional[str] = None,
    slot: Optional[int] = None,
    target_name: Optional[str] = None,
) -> Dict[str, Any]:
    """Upload a BrewLink Unified Recipe via HTTP REST (POST /api/equipment/recipe)."""
    url = f"http://{host}/api/equipment/recipe"
    envelope = build_command_envelope(
        recipe=recipe,
        equipment_type=equipment_type,
        target_mac=target_mac,
        slot=slot,
        target_name=target_name,
        api_token=api_token,
    )
    body = json.dumps(envelope).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=body,
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_token}",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        err_body = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {exc.code}: {err_body}") from exc


def main() -> int:
    parser = argparse.ArgumentParser(
        description="BrewLink BeerXML 1.0 Converter & Recipe Uploader"
    )
    parser.add_argument("--xml", help="Path to BeerXML 1.0 (.xml) file")
    parser.add_argument("--host", help="BrewLink gateway IP or hostname (auto-discovered if --pair is used)")
    parser.add_argument("--token", help="Paired BrewLink API token (bl_...)")
    parser.add_argument(
        "--pair",
        action="store_true",
        help="Discover BrewLink via UDP broadcast (:53540) or claim directly on --host without a PIN",
    )
    parser.add_argument("--pair-pin", help="Optional 6-digit pairing PIN if claiming manually")
    parser.add_argument("--client-name", default="BeerXML Server", help="Client name when pairing")
    parser.add_argument(
        "--list-devices",
        action="store_true",
        help="List all paired/connected Bluetooth equipment devices on BrewLink (slot, name, equipment_type, mac)",
    )
    parser.add_argument(
        "--slot",
        type=int,
        choices=[1, 2, 3],
        help="Target equipment by 1-based BLE slot number (1..3) without needing a MAC address",
    )
    parser.add_argument(
        "--name",
        help="Target equipment by its guaranteed unique name (e.g. 'Grainfather G30 Connect' or 'Grainfather G30 Connect-8B22')",
    )
    parser.add_argument(
        "--equipment-type",
        choices=["boiler", "fermenter", "cooler"],
        default="boiler",
        help="Target equipment class (default: boiler)",
    )
    parser.add_argument("--mac", help="Optional target BLE MAC address (if omitted, routes by --slot, --name, or --equipment-type)")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Only convert BeerXML and print the BrewLink Unified Recipe JSON",
    )
    args = parser.parse_args()

    if args.pair or args.pair_pin:
        if args.host:
            res = claim_pairing(args.host, args.pair_pin, args.client_name)
        elif args.pair_pin:
            res = claim_pairing("192.168.4.1", args.pair_pin, args.client_name)
        else:
            res = discover_and_claim_pairing(client_name=args.client_name)
        print(json.dumps(res, indent=2))
        return 0

    host = args.host or "192.168.4.1"

    if args.list_devices:
        if not args.token:
            parser.error("--token is required with --list-devices")
        res = list_paired_equipment_http(host, args.token)
        print(json.dumps(res, indent=2))
        return 0

    if not args.xml:
        parser.error("--xml is required unless --pair, --pair-pin, or --list-devices is specified")

    with open(args.xml, "r", encoding="utf-8") as f:
        xml_data = f.read()

    recipe = parse_beerxml_to_brewlink_recipe(xml_data)
    envelope = build_command_envelope(
        recipe=recipe,
        equipment_type=args.equipment_type,
        target_mac=args.mac,
        slot=args.slot,
        target_name=args.name,
        api_token=args.token,
    )

    if args.dry_run or not args.token:
        print(json.dumps(envelope, indent=2))
        return 0

    result = upload_recipe_http(
        host=host,
        api_token=args.token,
        recipe=recipe,
        equipment_type=args.equipment_type,
        target_mac=args.mac,
        slot=args.slot,
        target_name=args.name,
    )
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
