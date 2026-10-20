#!/usr/bin/env python3
"""HumHouse Studio license key maker (only you run this).

  python3 hhs_keygen.py --name "Buyer Name" --email buyer@mail.com --machine ABCD-1234-EF56-7890 [--machine 2ND-ID]

The buyer copies each Machine ID from the HumHouse Studio activation window (up to 2 computers).
Needs humhouse_private_key.json next to this script (or --key path). NEVER share or commit that file.
"""
import argparse, base64, hashlib, json, os, sys, time, uuid

PREFIX = bytes.fromhex("3031300d060960864801650304020105000420")   # PKCS#1 v1.5 SHA-256 DigestInfo

def sign(payload, n, d):
    k = (n.bit_length() + 7) // 8
    t = PREFIX + hashlib.sha256(payload).digest()
    em = b"\x00\x01" + b"\xff" * (k - len(t) - 3) + b"\x00" + t
    return format(pow(int.from_bytes(em, "big"), d, n), "x")

def main():
    ap = argparse.ArgumentParser(description="Make a HumHouse Studio license key")
    ap.add_argument("--batch", type=int, default=0, help="make N ready-to-sell keys (no Machine ID needed), one per line")
    ap.add_argument("--name", default="")
    ap.add_argument("--email", default="")
    ap.add_argument("--machine", action="append", default=[], help="Machine ID from the activation window (use twice for 2 computers)")
    ap.add_argument("--expires-days", type=int, default=0, help="0 = never expires")
    ap.add_argument("--key", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "humhouse_private_key.json"))
    a = ap.parse_args()
    k = json.load(open(a.key))
    now = int(time.time())
    if a.batch > 0:
        for _ in range(a.batch):
            payload = json.dumps({"lic_id": "HHS-" + uuid.uuid4().hex[:12].upper(), "product_sku": "humhouse_studio",
                                  "issued_at": now, "expires_at": 0, "any_machine": True}, separators=(",", ":")).encode()
            print("HHS1." + base64.b64encode(payload).decode() + "." + sign(payload, int(k["n"], 16), int(k["d"], 16)))
        return
    machines = [m.strip().upper() for m in a.machine if m.strip()]
    if not a.name or not a.email: sys.exit("--name and --email are needed (or use --batch N)")
    if not 1 <= len(machines) <= 2: sys.exit("a key holds 1 or 2 Machine IDs")
    payload = json.dumps({
        "lic_id": "HHS-" + uuid.uuid4().hex[:12].upper(),
        "name": a.name, "user_email": a.email, "product_sku": "humhouse_studio",
        "issued_at": now, "expires_at": now + a.expires_days * 86400 if a.expires_days > 0 else 0,
        "hardware_ids": machines,
    }, separators=(",", ":")).encode()
    print("HHS1." + base64.b64encode(payload).decode() + "." + sign(payload, int(k["n"], 16), int(k["d"], 16)))

if __name__ == "__main__":
    main()
