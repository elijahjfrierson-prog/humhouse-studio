#!/usr/bin/env python3
"""HumHouse license admin (only you run this). Needs HHS_ADMIN_TOKEN and HHS_LICENSE_URL in the environment.

  python3 scripts/hhs_license_admin.py lookup   HHS-XXXXX-XXXXX-XXXXX-XXXXX
  python3 scripts/hhs_license_admin.py revoke   HHS-...          # key stops working on every computer
  python3 scripts/hhs_license_admin.py unrevoke HHS-...
  python3 scripts/hhs_license_admin.py free     HHS-... [MACHINE-ID]   # free one or all computer slots
"""
import json, os, sys, urllib.request

def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("lookup", "revoke", "unrevoke", "free"):
        sys.exit(__doc__)
    body = {"key": sys.argv[2]}
    if len(sys.argv) > 3: body["mid"] = sys.argv[3]
    req = urllib.request.Request(os.environ["HHS_LICENSE_URL"].rstrip("/") + "/v1/admin/" + sys.argv[1],
                                 data=json.dumps(body).encode(), method="POST",
                                 headers={"content-type": "application/json", "authorization": "Bearer " + os.environ["HHS_ADMIN_TOKEN"]})
    try:
        print(json.dumps(json.load(urllib.request.urlopen(req)), indent=2))
    except urllib.error.HTTPError as e:
        print(e.code, e.read().decode())

if __name__ == "__main__":
    main()
