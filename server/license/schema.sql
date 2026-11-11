-- keys: one row per sellable key (only the SHA-256 hash is stored, never the key itself)
CREATE TABLE IF NOT EXISTS keys (
  hash TEXT PRIMARY KEY,
  status TEXT NOT NULL DEFAULT 'active',      -- active | revoked
  max_machines INTEGER NOT NULL DEFAULT 3,
  note TEXT NOT NULL DEFAULT '',
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
-- activations: one row per computer that holds a key
CREATE TABLE IF NOT EXISTS activations (
  key_hash TEXT NOT NULL REFERENCES keys(hash),
  mid TEXT NOT NULL,
  first_seen INTEGER NOT NULL,
  last_seen INTEGER NOT NULL,
  app_version TEXT NOT NULL DEFAULT '',
  PRIMARY KEY (key_hash, mid)
);
CREATE TABLE IF NOT EXISTS events (
  at INTEGER NOT NULL, key_hash TEXT NOT NULL, mid TEXT NOT NULL, result TEXT NOT NULL, ip TEXT NOT NULL DEFAULT '',
  country TEXT NOT NULL DEFAULT '', city TEXT NOT NULL DEFAULT '', lat REAL NOT NULL DEFAULT 0, lon REAL NOT NULL DEFAULT 0,
  id TEXT, origin TEXT NOT NULL DEFAULT 'cf', ver TEXT NOT NULL DEFAULT ''
);
CREATE UNIQUE INDEX IF NOT EXISTS events_id ON events(id);
CREATE TABLE IF NOT EXISTS home (k INTEGER PRIMARY KEY, url TEXT NOT NULL, seen INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS events_key ON events(key_hash);
CREATE INDEX IF NOT EXISTS events_at ON events(at);
