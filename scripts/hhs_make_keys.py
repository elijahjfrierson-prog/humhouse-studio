#!/usr/bin/env python3
"""Make N random HumHouse license keys for the license server (only you run this).

  python3 scripts/hhs_make_keys.py --count 100000 --out ~/hhs-keys

Writes keys.txt (the keys you sell: KEEP PRIVATE, never commit) and keys.sql
(only SHA-256 hashes) which you load into the server:
  cd server/license && npx wrangler d1 execute humhouse-license --remote --file ~/hhs-keys/keys.sql
"""
import argparse, hashlib, os, secrets

ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"   # Crockford base32: no I, L, O, U

def new_key():
    raw = "".join(secrets.choice(ALPHABET) for _ in range(20))   # 100 random bits
    return raw, "HHS-" + "-".join(raw[i:i + 5] for i in range(0, 20, 5))

def key_hash(raw):
    return hashlib.sha256(("HHSKEY|" + raw).encode()).hexdigest()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=100000)
    ap.add_argument("--out", required=True)
    ap.add_argument("--note", default="batch")
    ap.add_argument("--max-machines", type=int, default=3)
    a = ap.parse_args()
    os.makedirs(os.path.expanduser(a.out), exist_ok=True)
    out = os.path.expanduser(a.out)
    seen = set()
    with open(os.path.join(out, "keys.txt"), "w") as kt, open(os.path.join(out, "keys.sql"), "w") as ks:
        rows = []
        while len(seen) < a.count:
            raw, pretty = new_key()
            if raw in seen: continue
            seen.add(raw)
            kt.write(pretty + "\n")
            rows.append(f"('{key_hash(raw)}',{a.max_machines},'{a.note}')")
            if len(rows) == 500:
                ks.write("INSERT OR IGNORE INTO keys (hash, max_machines, note) VALUES " + ",".join(rows) + ";\n"); rows = []
        if rows:
            ks.write("INSERT OR IGNORE INTO keys (hash, max_machines, note) VALUES " + ",".join(rows) + ";\n")
    print(f"{a.count} keys -> {out}/keys.txt (private) + {out}/keys.sql (hashes for the server)")

if __name__ == "__main__":
    main()
