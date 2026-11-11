// HumHouse license server (Cloudflare Worker + D1).
// POST /v1/activate {key, mid, ver}  -> {ok, pass} | {ok:false, error}
// POST /v1/deactivate {key, mid}     -> frees this computer's slot
// Admin (Authorization: Bearer ADMIN_TOKEN): /v1/admin/lookup|revoke|unrevoke|free {key[, mid]}
import MONITOR_HTML from "./monitor.html";
const PASS_DAYS = 14;
const ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";   // Crockford base32

export function normalizeKey(k) {
  let s = String(k || "").toUpperCase().replace(/^HHS-?/, "").replace(/[^0-9A-Z]/g, "")
    .replace(/O/g, "0").replace(/[IL]/g, "1");
  if (s.length !== 20 || [...s].some(c => !ALPHABET.includes(c))) return null;
  return s;
}
async function sha256Hex(text) {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return [...new Uint8Array(d)].map(b => b.toString(16).padStart(2, "0")).join("");
}
export const keyHash = norm => sha256Hex("HHSKEY|" + norm);

let signKey = null;
async function signer(env) {
  if (signKey) return signKey;
  const b64 = env.PASS_PRIVATE_KEY.replace(/-----[^-]+-----/g, "").replace(/\s+/g, "");
  const der = Uint8Array.from(atob(b64), c => c.charCodeAt(0));
  signKey = await crypto.subtle.importKey("pkcs8", der, { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" }, false, ["sign"]);
  return signKey;
}
async function makePass(env, hash, mid, now) {
  const payload = JSON.stringify({ sku: "humhouse_studio", lic: hash.slice(0, 16), mid, iat: now, exp: now + PASS_DAYS * 86400 });
  const bytes = new TextEncoder().encode(payload);
  const sig = new Uint8Array(await crypto.subtle.sign("RSASSA-PKCS1-v1_5", await signer(env), bytes));
  return "HHP1." + btoa(String.fromCharCode(...bytes)) + "." + [...sig].map(b => b.toString(16).padStart(2, "0")).join("");
}
const json = (o, status = 200) => new Response(JSON.stringify(o), { status, headers: { "content-type": "application/json" } });
const validMid = m => typeof m === "string" && /^[0-9A-F]{4}(-[0-9A-F]{4}){3}$/.test(m);

async function log(env, hash, mid, result, req) {
  const cf = req.cf || {};
  await env.DB.prepare("INSERT INTO events (at, key_hash, mid, result, ip, country, city, lat, lon) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)")
    .bind(Math.floor(Date.now() / 1000), hash, mid, result, req.headers.get("cf-connecting-ip") || "",
          String(cf.country || ""), String(cf.city || ""), Number(cf.latitude) || 0, Number(cf.longitude) || 0).run();
}

async function activate(env, body, req) {
  const norm = normalizeKey(body.key), mid = String(body.mid || "").toUpperCase();
  if (!norm) return json({ ok: false, error: "That isn't a HumHouse license key." }, 400);
  if (!validMid(mid)) return json({ ok: false, error: "Bad Machine ID." }, 400);
  const hash = await keyHash(norm), now = Math.floor(Date.now() / 1000);
  const row = await env.DB.prepare("SELECT status, max_machines FROM keys WHERE hash = ?1").bind(hash).first();
  if (!row) { await log(env, hash, mid, "unknown", req); return json({ ok: false, error: "This license key doesn't exist." }, 404); }
  if (row.status !== "active") { await log(env, hash, mid, "revoked", req); return json({ ok: false, revoked: true, error: "This license key was revoked." }, 403); }
  const ver = String(body.ver || "").slice(0, 32);
  // already on this computer: refresh (reinstalls never use a new slot)
  const upd = await env.DB.prepare("UPDATE activations SET last_seen = ?3, app_version = ?4 WHERE key_hash = ?1 AND mid = ?2")
    .bind(hash, mid, now, ver).run();
  if (upd.meta.changes === 0) {
    // claim a slot in one statement, so two computers at once can't both take the last one
    const ins = await env.DB.prepare(
      "INSERT OR IGNORE INTO activations (key_hash, mid, first_seen, last_seen, app_version) " +
      "SELECT ?1, ?2, ?3, ?3, ?4 WHERE (SELECT COUNT(*) FROM activations WHERE key_hash = ?1) < ?5")
      .bind(hash, mid, now, ver, row.max_machines).run();
    if (ins.meta.changes === 0) {
      await log(env, hash, mid, "full", req);
      return json({ ok: false, full: true, error: `This key is already used on ${row.max_machines} computers.` }, 403);
    }
    await log(env, hash, mid, "new", req);
  } else await log(env, hash, mid, "renew", req);
  return json({ ok: true, pass: await makePass(env, hash, mid, now) });
}

async function deactivate(env, body) {
  const norm = normalizeKey(body.key), mid = String(body.mid || "").toUpperCase();
  if (!norm || !validMid(mid)) return json({ ok: false, error: "Bad request." }, 400);
  const r = await env.DB.prepare("DELETE FROM activations WHERE key_hash = ?1 AND mid = ?2").bind(await keyHash(norm), mid).run();
  return json({ ok: true, freed: r.meta.changes });
}

async function feed(env) {
  const since = Math.floor(Date.now() / 1000) - 7 * 86400;
  const ev = await env.DB.prepare("SELECT at, key_hash, mid, result, country, city, lat, lon FROM events WHERE at > ?1 ORDER BY at DESC LIMIT 500").bind(since).all();
  // suspicion per key over 7 days: blocked 4th-computer tries, countries, IPs, distinct computers
  const st = await env.DB.prepare(
    "SELECT e.key_hash h, SUM(e.result = 'full') blocked, SUM(e.result = 'revoked') rev, COUNT(DISTINCT e.country) countries, " +
    "COUNT(DISTINCT e.ip) ips, COUNT(DISTINCT e.mid) mids, MAX(e.at) last, k.status status " +
    "FROM events e LEFT JOIN keys k ON k.hash = e.key_hash WHERE e.at > ?1 AND e.result != 'unknown' GROUP BY e.key_hash ORDER BY last DESC LIMIT 300").bind(since).all();
  const keys = st.results.map(r => ({ ...r, score: Math.min(100, r.blocked * 25 + Math.max(0, r.countries - 1) * 30 + Math.max(0, r.ips - 3) * 10 + Math.max(0, r.mids - 3) * 20 + r.rev * 10) }));
  const tot = await env.DB.prepare("SELECT (SELECT COUNT(*) FROM keys) keys, (SELECT COUNT(DISTINCT key_hash) FROM activations) used, (SELECT COUNT(*) FROM activations) machines, (SELECT COUNT(*) FROM keys WHERE status = 'revoked') revoked, (SELECT COUNT(*) FROM events WHERE result = 'unknown' AND at > ?1) unknown").bind(since).first();
  return json({ ok: true, now: Math.floor(Date.now() / 1000), totals: tot, events: ev.results, keys });
}

async function admin(env, path, body) {
  if (path === "feed") return feed(env);
  const norm = normalizeKey(body.key);
  const hash = norm ? await keyHash(norm) : (/^[0-9a-f]{64}$/.test(String(body.hash || "")) ? body.hash : null);
  if (!hash) return json({ ok: false, error: "bad key" }, 400);
  if (path === "lookup") {
    const k = await env.DB.prepare("SELECT status, max_machines, note FROM keys WHERE hash = ?1").bind(hash).first();
    const a = await env.DB.prepare("SELECT mid, first_seen, last_seen, app_version FROM activations WHERE key_hash = ?1").bind(hash).all();
    return json({ ok: !!k, key: k, machines: a.results });
  }
  if (path === "revoke" || path === "unrevoke") {
    const r = await env.DB.prepare("UPDATE keys SET status = ?2 WHERE hash = ?1").bind(hash, path === "revoke" ? "revoked" : "active").run();
    return json({ ok: r.meta.changes > 0 });
  }
  if (path === "free") {
    const r = body.mid
      ? await env.DB.prepare("DELETE FROM activations WHERE key_hash = ?1 AND mid = ?2").bind(hash, String(body.mid).toUpperCase()).run()
      : await env.DB.prepare("DELETE FROM activations WHERE key_hash = ?1").bind(hash).run();
    return json({ ok: true, freed: r.meta.changes });
  }
  return json({ ok: false, error: "unknown admin action" }, 404);
}

function sameSecret(a, b) {
  if (!a || !b || a.length !== b.length) return false;
  let d = 0;
  for (let i = 0; i < a.length; i++) d |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return d === 0;
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (req.method === "GET" && url.pathname === "/monitor")
      return new Response(MONITOR_HTML, { headers: { "content-type": "text/html; charset=utf-8", "cache-control": "no-store",
        "content-security-policy": "default-src 'self'; script-src 'self' 'unsafe-inline' https://cdn.jsdelivr.net; connect-src 'self' https://cdn.jsdelivr.net; style-src 'unsafe-inline'" } });
    if (req.method !== "POST") return json({ ok: true, service: "humhouse-license" });
    let body;
    try { body = await req.json(); } catch { return json({ ok: false, error: "Bad request." }, 400); }
    if (url.pathname === "/v1/activate") return activate(env, body, req);
    if (url.pathname === "/v1/deactivate") return deactivate(env, body);
    if (url.pathname.startsWith("/v1/admin/")) {
      const tok = (req.headers.get("authorization") || "").replace(/^Bearer /, "");
      if (!sameSecret(tok, env.ADMIN_TOKEN)) return json({ ok: false, error: "unauthorized" }, 401);
      return admin(env, url.pathname.slice(10), body);
    }
    return json({ ok: false, error: "not found" }, 404);
  }
};
