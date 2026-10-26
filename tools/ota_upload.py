#!/usr/bin/env python3
"""OTA uploader for the reflow oven.

Reads target host + token from a config file (default: ~/.config/reflow_oven/upload.conf):
    host  = "reflow.local"
    token = "abc1234..."

Usage:
    python tools/ota_upload.py <path/to/firmware.bin>
"""
import http.client
import json
import os
import sys
import time
import urllib.error
import urllib.request

try:
    import tomllib  # Python 3.11+
except ImportError:
    import tomli as tomllib

try:
    from tqdm import tqdm
    HAVE_TQDM = True
except ImportError:
    HAVE_TQDM = False

CONFIG_DEFAULT = os.path.expanduser("~/.config/reflow_oven/upload.conf")
# Used only if /version doesn't report "max_size" — the device is the single
# source of truth for the slot size (#55).
MAX_BIN_FALLBACK = 0x170000
# Throttle (opt-in via env): on a heap-starved device the WiFi RX path can only
# buffer a little before malloc fails and the net stack wedges. Sending small
# chunks with a pause between them keeps the in-flight (unacked) data far below
# the TCP window, so the device's transient buffering stays tiny and the upload
# fits in the available heap. Defaults are the fast path (unchanged behaviour);
# set both to throttle, e.g. REFLOW_OTA_CHUNK=1024 REFLOW_OTA_DELAY_MS=15.
CHUNK = int(os.environ.get("REFLOW_OTA_CHUNK", "8192"))
DELAY_S = float(os.environ.get("REFLOW_OTA_DELAY_MS", "0")) / 1000.0
# One-time pause after sending headers, before the body. esp_ota_begin erases
# the whole slot up front (~10-15 s) before the device recvs anything; if the
# client streams during that window the TCP buffer fills and spikes device heap
# at its tightest moment. Pausing lets the erase finish against an empty pipe.
PRESLEEP_S = float(os.environ.get("REFLOW_OTA_PRESLEEP_MS", "0")) / 1000.0
# esp_app_desc_t sits right after the image header + first segment header.
ESP_APP_DESC_OFFSET = 0x20
ESP_APP_DESC_MAGIC = 0xABCD5432


def load_config():
    path = os.environ.get("REFLOW_OTA_CONFIG", CONFIG_DEFAULT)
    if not os.path.exists(path):
        sys.exit(f"config not found: {path}\nCreate with:\n  host  = \"reflow.local\"\n  token = \"...\"")
    with open(path, "rb") as f:
        return tomllib.load(f)


def get_version(host):
    url = f"http://{host}/version"
    try:
        with urllib.request.urlopen(url, timeout=5) as r:
            return json.loads(r.read())
    except Exception as e:
        sys.exit(f"cannot reach {url}: {e}")


def version_tuple(s):
    """Best-effort parse of 'vX.Y.Z[-rest]' -> (X, Y, Z). Returns None if unparseable."""
    import re
    m = re.match(r"v?(\d+)\.(\d+)\.(\d+)", s)
    if not m: return None
    return tuple(int(x) for x in m.groups())


def parse_bin_version(path):
    """Read the embedded esp_app_desc version string from a firmware .bin so we
    can actually compare against the device version (#54). Returns the version
    string, or None if the descriptor can't be located."""
    import struct
    try:
        with open(path, "rb") as f:
            head = f.read(ESP_APP_DESC_OFFSET + 48 + 32)
    except OSError:
        return None
    if len(head) < ESP_APP_DESC_OFFSET + 48 + 32:
        return None
    (magic,) = struct.unpack_from("<I", head, ESP_APP_DESC_OFFSET)
    if magic != ESP_APP_DESC_MAGIC:
        return None
    raw = head[ESP_APP_DESC_OFFSET + 16: ESP_APP_DESC_OFFSET + 16 + 32]
    return raw.split(b"\x00", 1)[0].decode("ascii", "replace") or None


def warn_if_downgrade(device, local_path):
    dv = device.get("version", "")
    lv = parse_bin_version(local_path)
    if lv:
        sys.stderr.write(f"  uploading:        {lv}\n")
    dvt, lvt = version_tuple(dv), version_tuple(lv or "")
    if dvt and lvt:
        if lvt < dvt:
            sys.stderr.write(f"  WARNING: this is a DOWNGRADE ({dv} -> {lv})\n")
        elif lvt == dvt:
            sys.stderr.write(f"  note: same version as device ({dv}); re-flash\n")
    elif lv is None:
        sys.stderr.write("  (could not read version from .bin; verify version increased post-upload)\n")


def upload(host, token, fw_path, max_size):
    size = os.path.getsize(fw_path)
    if max_size and size > max_size:
        sys.exit(f"firmware too large: {size} bytes (device slot max {int(max_size)})")
    # 60 s socket timeout (> the device's 30 s stall window) so a genuinely
    # stalled write fails instead of hanging forever (#56).
    conn = http.client.HTTPConnection(host, 80, timeout=60)
    conn.putrequest("POST", "/ota")
    conn.putheader("X-OTA-Token", token)
    conn.putheader("Content-Type", "application/octet-stream")
    conn.putheader("Content-Length", str(size))
    conn.endheaders()

    if PRESLEEP_S:
        sys.stderr.write(f"  pre-send pause {PRESLEEP_S:.0f}s (letting the slot erase finish)...\n")
        sys.stderr.flush()
        time.sleep(PRESLEEP_S)

    with open(fw_path, "rb") as f:
        if HAVE_TQDM:
            with tqdm(total=size, unit="B", unit_scale=True, desc="upload") as bar:
                while True:
                    chunk = f.read(CHUNK)
                    if not chunk: break
                    conn.send(chunk)
                    if DELAY_S: time.sleep(DELAY_S)
                    bar.update(len(chunk))
        else:
            sent = 0
            while True:
                chunk = f.read(CHUNK)
                if not chunk: break
                conn.send(chunk)
                if DELAY_S: time.sleep(DELAY_S)
                sent += len(chunk)
                sys.stderr.write(f"\r  upload: {sent}/{size} bytes")
                sys.stderr.flush()
            sys.stderr.write("\n")

    try:
        resp = conn.getresponse()
    except (http.client.HTTPException, OSError) as e:
        # The device reboots ~1 s after the 200; on some networks the socket
        # drops before the response is read. Upload almost certainly landed —
        # tell the user how to confirm rather than failing hard (#56).
        sys.stderr.write(
            f"\nupload sent but no response (device likely rebooting): {e}\n"
            f"confirm with:  curl http://{host}/version\n")
        conn.close()
        return
    body = resp.read().decode(errors="replace")
    print(f"{resp.status} {resp.reason}")
    print(body)
    conn.close()
    if resp.status >= 400:
        sys.exit(1)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: ota_upload.py <firmware.bin>")
    fw = sys.argv[1]
    if not os.path.exists(fw):
        sys.exit(f"file not found: {fw}")
    cfg = load_config()
    print(f"target: {cfg['host']}")
    info = get_version(cfg["host"])
    print(f"current device: {info.get('version', 'unknown')} (status: {info.get('status', '?')})")
    warn_if_downgrade(info, fw)
    max_size = info.get("max_size") or MAX_BIN_FALLBACK
    print(f"uploading {fw} ({os.path.getsize(fw)} bytes)...")
    upload(cfg["host"], cfg["token"], fw, max_size)


if __name__ == "__main__":
    main()
