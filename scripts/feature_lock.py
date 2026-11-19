#!/usr/bin/env python3
"""Fails if a feature locked in FEATURES.md is no longer in Source/ (nothing gets dropped without the owner saying so)."""
import pathlib, sys
root = pathlib.Path(__file__).resolve().parent.parent
text = "\n".join(p.read_text(errors="ignore") for p in (root / "Source").rglob("*") if p.suffix in (".cpp", ".h", ".mm"))
locks = [l.split("lock:", 1)[1].strip() for l in (root / "FEATURES.md").read_text().splitlines() if l.startswith("- lock:")]
missing = [s for s in locks if s.lower() not in text.lower()]
for s in missing: print("FEATURE LOCK: missing from Source/:", s)
print(f"feature lock: {len(locks) - len(missing)}/{len(locks)} present")
sys.exit(1 if missing else 0)
