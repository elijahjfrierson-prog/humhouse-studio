# HumHouse license server (Cloudflare Worker + D1)

Regular HumHouse Studio activates here; Master has no key gate.

- `POST /v1/activate {key, mid, ver}`: max 3 computers per key, same computer reactivates for free, returns a 14-day pass signed for that Machine ID.
- `POST /v1/deactivate {key, mid}`
- Admin (`Authorization: Bearer ADMIN_TOKEN`): `/v1/admin/lookup|revoke|unrevoke|free {key|hash[, mid]}`, `/v1/admin/feed` (monitor data).
- `GET /monitor`: live map + suspicious-key flags (green to red), revoke / free buttons. Paste the admin token once.

## Deploy
```
npx wrangler d1 create humhouse-license          # put the id in wrangler.toml
npx wrangler d1 execute humhouse-license --remote --file schema.sql
python3 ../../scripts/hhs_make_keys.py OUT_DIR    # keys.txt (give to buyers) + keys.sql (hashes only)
npx wrangler d1 execute humhouse-license --remote --file OUT_DIR/keys.sql
npx wrangler secret put PASS_PRIVATE_KEY          # PKCS#8 PEM matching the modulus in Source/License.cpp
npx wrangler secret put ADMIN_TOKEN
npx wrangler deploy
```
Build the app with `-DHHS_LICENSE_URL=https://<your worker>.workers.dev` if the URL differs. Never commit keys.txt, the private key or the admin token.
