#!/usr/bin/env python3
"""Build Resources/License/keyprints.bin from keys.txt (only you run this, after hhs_make_keys.py).

  python3 scripts/hhs_keyprints.py ~/hhs-keys/keys.txt

Each sold key -> first 16 bytes of SHA-256("HHSOFF|" + key), sorted. Safe to ship: a fingerprint can't be
turned back into a key, and 16 bytes means no one can search for a fake key that matches one. The app uses it so a real key unlocks even when the license server is down.
"""
import hashlib, os, sys

ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

def norm(k):
    s = k.strip().upper()
    if s.startswith("HHS"): s = s[3:]
    s = "".join(c for c in s if c.isalnum()).replace("O", "0").replace("I", "1").replace("L", "1")
    return s if len(s) == 20 and all(c in ALPHABET for c in s) else None

def main():
    keys = [n for n in (norm(l) for l in open(os.path.expanduser(sys.argv[1]))) if n]
    prints = sorted({hashlib.sha256(("HHSOFF|" + k).encode()).digest()[:16] for k in keys})
    out = os.path.join(os.path.dirname(__file__), "..", "Resources", "License", "keyprints.bin")
    with open(out, "wb") as f: f.write(b"".join(prints))
    print(f"{len(prints)} key fingerprints -> {os.path.normpath(out)}")

if __name__ == "__main__":
    main()
