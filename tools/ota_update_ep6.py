#!/usr/bin/env python3
"""Discover one EP6 by MAC and upload an ESP-IDF application binary."""

import argparse
import json
import socket
import sys
import urllib.request
from pathlib import Path

DISCOVERY_PORT = 3232
DEFAULT_MAC = "80:F1:B2:D3:7D:16"


def discover(mac: str, timeout: float, broadcast: str) -> tuple[str, dict]:
    message = f"DY_OTA_DISCOVER EP6 {mac}".encode("ascii")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.settimeout(timeout)
        sock.bind(("", 0))
        sock.sendto(message, (broadcast, DISCOVERY_PORT))
        while True:
            payload, sender = sock.recvfrom(1024)
            info = json.loads(payload.decode("utf-8"))
            if info.get("product") == "EP6" and info.get("mac", "").upper() == mac.upper():
                return sender[0], info


def upload(ip: str, port: int, mac: str, firmware: Path, timeout: float) -> str:
    data = firmware.read_bytes()
    request = urllib.request.Request(
        f"http://{ip}:{port}/ota/update",
        data=data,
        method="POST",
        headers={
            "Content-Type": "application/octet-stream",
            "Content-Length": str(len(data)),
            "X-Device-MAC": mac,
        },
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read().decode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="EP6 MAC-targeted OTA updater")
    parser.add_argument("firmware", nargs="?", default="build_thpark/dy_ep4.bin")
    parser.add_argument("--mac", default=DEFAULT_MAC)
    parser.add_argument("--broadcast", default="192.168.0.255")
    parser.add_argument("--discover-timeout", type=float, default=8.0)
    parser.add_argument("--upload-timeout", type=float, default=180.0)
    parser.add_argument("--discover-only", action="store_true")
    args = parser.parse_args()
    firmware = Path(args.firmware)
    if not args.discover_only and not firmware.is_file():
        print(f"Firmware not found: {firmware}", file=sys.stderr)
        return 2
    print(f"Discovering EP6 {args.mac} ...")
    try:
        ip, info = discover(args.mac, args.discover_timeout, args.broadcast)
        port = int(info.get("ota_port", 3233))
        print(f"Found {ip}:{port}, version {info.get('version', '?')}")
        if args.discover_only:
            return 0
        print(f"Uploading {firmware} ({firmware.stat().st_size} bytes) ...")
        print(upload(ip, port, args.mac, firmware, args.upload_timeout))
        print("OTA accepted; device is restarting.")
        return 0
    except Exception as error:
        print(f"OTA failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
