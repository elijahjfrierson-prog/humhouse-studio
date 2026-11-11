// HumHouse license server (Cloudflare Worker + D1).
// POST /v1/activate {key, mid, ver}  -> {ok, pass} | {ok:false, error}
// POST /v1/deactivate {key, mid}     -> frees this computer's slot
// Admin (Authorization: Bearer ADMIN_TOKEN): /v1/admin/lookup|revoke|unrevoke|free {key[, mid]}
import MONITOR_HTML from "./monitor.html";
const PASS_DAYS = 1;   // daily check-in: the app renews silently in the background; a computer offline over 24 h sees the key screen at next launch
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

// every event is also a sync op (id unique): the home License Server pulls ours and pushes its own (origin 'home')
async function log(env, hash, mid, result, req, ver = "") {
  const cf = (req && req.cf) || {};
  await env.DB.prepare("INSERT INTO events (at, key_hash, mid, result, ip, country, city, lat, lon, id, origin, ver) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, 'cf', ?11)")
    .bind(Math.floor(Date.now() / 1000), hash, mid, result, (req && req.headers.get("cf-connecting-ip")) || "",
          String(cf.country || ""), String(cf.city || ""), Number(cf.latitude) || 0, Number(cf.longitude) || 0, crypto.randomUUID(), ver).run();
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
    await log(env, hash, mid, "new", req, ver);
  } else await log(env, hash, mid, "renew", req, ver);
  return json({ ok: true, pass: await makePass(env, hash, mid, now) });
}

async function deactivate(env, body) {
  const norm = normalizeKey(body.key), mid = String(body.mid || "").toUpperCase();
  if (!norm || !validMid(mid)) return json({ ok: false, error: "Bad request." }, 400);
  const hash = await keyHash(norm);
  const r = await env.DB.prepare("DELETE FROM activations WHERE key_hash = ?1 AND mid = ?2").bind(hash, mid).run();
  if (r.meta.changes) await log(env, hash, mid, "free", null);
  return json({ ok: true, freed: r.meta.changes });
}

// apply one op's state change (events that only report, like 'full' or 'unknown', change nothing)
async function applyOp(env, o) {
  if (o.result === "new" || o.result === "renew")
    await env.DB.prepare("INSERT INTO activations (key_hash, mid, first_seen, last_seen, app_version) VALUES (?1, ?2, ?3, ?3, ?4) " +
      "ON CONFLICT (key_hash, mid) DO UPDATE SET last_seen = MAX(last_seen, ?3), app_version = ?4").bind(o.key_hash, o.mid, o.at, o.ver || "").run();
  else if (o.result === "free")
    await (o.mid === "*" ? env.DB.prepare("DELETE FROM activations WHERE key_hash = ?1").bind(o.key_hash)
                         : env.DB.prepare("DELETE FROM activations WHERE key_hash = ?1 AND mid = ?2").bind(o.key_hash, o.mid)).run();
  else if (o.result === "revoke" || o.result === "unrevoke")
    await env.DB.prepare("UPDATE keys SET status = ?2 WHERE hash = ?1").bind(o.key_hash, o.result === "revoke" ? "revoked" : "active").run();
}

// home License Server sync: it pushes its ops, gets ours since its cursor; Cloudflare learns its public address
async function sync(env, body, req) {
  const ops = Array.isArray(body.ops) ? body.ops.slice(0, 500) : [];
  for (const o of ops) {
    if (!/^[0-9a-f-]{8,64}$/i.test(String(o.id || "")) || !/^[0-9a-f]{64}$/.test(String(o.key_hash || ""))) continue;
    const ins = await env.DB.prepare("INSERT OR IGNORE INTO events (at, key_hash, mid, result, ip, country, city, lat, lon, id, origin, ver) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, 'home', ?11)")
      .bind(Number(o.at) || 0, o.key_hash, String(o.mid || "").slice(0, 32), String(o.result || "").slice(0, 16), String(o.ip || "").slice(0, 64),
            String(o.country || "").slice(0, 8), String(o.city || "").slice(0, 64), Number(o.lat) || 0, Number(o.lon) || 0, o.id, String(o.ver || "").slice(0, 32)).run();
    if (ins.meta.changes) await applyOp(env, o);
  }
  const port = Number(body.port) | 0, ip = req.headers.get("cf-connecting-ip") || "";
  if (port > 0 && port < 65536 && ip)
    await env.DB.prepare("INSERT INTO home (k, url, seen) VALUES (1, ?1, ?2) ON CONFLICT (k) DO UPDATE SET url = ?1, seen = ?2")
      .bind("http://" + (ip.includes(":") ? "[" + ip + "]" : ip) + ":" + port, Math.floor(Date.now() / 1000)).run();
  const since = Number(body.since) || 0;
  const out = await env.DB.prepare("SELECT rowid seq, at, key_hash, mid, result, ip, country, city, lat, lon, id, ver FROM events WHERE rowid > ?1 AND origin != 'home' ORDER BY rowid LIMIT 1000").bind(since).all();
  const last = out.results.length ? out.results[out.results.length - 1].seq : since;
  return json({ ok: true, applied: ops.length, ops: out.results, cursor: last, ip });
}

// first run of the home server: every key (hashes only) and every activation, page by page
async function dump(env, body) {
  const after = String(body.after || ""), lim = 5000;
  const k = await env.DB.prepare("SELECT hash, status, max_machines FROM keys WHERE hash > ?1 ORDER BY hash LIMIT ?2").bind(after, lim).all();
  const res = { ok: true, keys: k.results, more: k.results.length === lim };
  if (!after) {
    res.activations = (await env.DB.prepare("SELECT key_hash, mid, first_seen, last_seen, app_version FROM activations").all()).results;
    res.cursor = (await env.DB.prepare("SELECT COALESCE(MAX(rowid), 0) c FROM events").first()).c;
  }
  return json(res);
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
  // sales = each key's first activation ("new"), placed where it happened; the monitor multiplies by the Value box
  const sales = await env.DB.prepare(
    "SELECT country, city, AVG(lat) lat, AVG(lon) lon, COUNT(*) n, SUM(at > ?1) week FROM " +
    "(SELECT e.key_hash, e.country, e.city, e.lat, e.lon, e.at FROM events e WHERE e.result = 'new' AND e.at = " +
    "(SELECT MIN(x.at) FROM events x WHERE x.key_hash = e.key_hash AND x.result = 'new') GROUP BY e.key_hash) " +
    "GROUP BY country, city ORDER BY n DESC LIMIT 500").bind(since).all();
  return json({ ok: true, now: Math.floor(Date.now() / 1000), totals: tot, events: ev.results, keys, sales: sales.results });
}

async function admin(env, path, body, req) {
  if (path === "feed") return feed(env);
  if (path === "sync") return sync(env, body, req);
  if (path === "dump") return dump(env, body);
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
    if (r.meta.changes) await log(env, hash, "", path, null);
    return json({ ok: r.meta.changes > 0 });
  }
  if (path === "free") {
    const r = body.mid
      ? await env.DB.prepare("DELETE FROM activations WHERE key_hash = ?1 AND mid = ?2").bind(hash, String(body.mid).toUpperCase()).run()
      : await env.DB.prepare("DELETE FROM activations WHERE key_hash = ?1").bind(hash).run();
    if (r.meta.changes) await log(env, hash, body.mid ? String(body.mid).toUpperCase() : "*", "free", null);
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
    if (req.method === "GET" && url.pathname === "/v1/servers") {   // where the home License Server is (seen in the last 2 min)
      const h = await env.DB.prepare("SELECT url, seen FROM home WHERE k = 1").first();
      return json({ ok: true, home: h && h.seen > Date.now() / 1000 - 120 ? h.url : "" });
    }
    if (req.method !== "POST") return json({ ok: true, service: "humhouse-license" });
    let body;
    try { body = await req.json(); } catch { return json({ ok: false, error: "Bad request." }, 400); }
    if (url.pathname === "/v1/activate") return activate(env, body, req);
    if (url.pathname === "/v1/deactivate") return deactivate(env, body);
    if (url.pathname.startsWith("/v1/admin/")) {
      const tok = (req.headers.get("authorization") || "").replace(/^Bearer /, "");
      if (!sameSecret(tok, env.ADMIN_TOKEN)) return json({ ok: false, error: "unauthorized" }, 401);
      return admin(env, url.pathname.slice(10), body, req);
    }
    return json({ ok: false, error: "not found" }, 404);
  }
};
