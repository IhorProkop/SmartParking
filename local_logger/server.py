# Smart Parking local logger.
# Pure-stdlib HTTP server + SQLite. Receives access events from the ESP32
# and exposes a dashboard and JSON/CSV endpoints.
#
# Run:
#     python server.py
# Then open http://localhost:5000

import csv
import http.server
import io
import json
import os
import socketserver
import sqlite3
import sys
from datetime import datetime
from urllib.parse import urlparse, parse_qs

HOST = "0.0.0.0"
PORT = 5000

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DB_PATH = os.path.join(BASE_DIR, "smart_parking.db")


# ---------- DB ----------
def init_db():
    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()
    cur.execute("""
        CREATE TABLE IF NOT EXISTS access_logs (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            created_at      TEXT    NOT NULL,
            uid             TEXT    NOT NULL,
            access_status   TEXT    NOT NULL,
            message         TEXT,
            free_spots      INTEGER,
            p1_status       TEXT,
            p1_distance     INTEGER,
            p2_status       TEXT,
            p2_distance     INTEGER,
            p3_status       TEXT,
            p3_distance     INTEGER,
            raw_json        TEXT
        )
    """)
    conn.commit()
    conn.close()


def insert_log(payload, raw_json):
    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()
    created_at = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    cur.execute("""
        INSERT INTO access_logs (
            created_at, uid, access_status, message, free_spots,
            p1_status, p1_distance,
            p2_status, p2_distance,
            p3_status, p3_distance,
            raw_json
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        created_at,
        str(payload.get("uid", "")),
        str(payload.get("access_status", "")),
        str(payload.get("message", "")),
        _to_int(payload.get("free_spots")),
        str(payload.get("p1_status", "")),
        _to_int(payload.get("p1_distance")),
        str(payload.get("p2_status", "")),
        _to_int(payload.get("p2_distance")),
        str(payload.get("p3_status", "")),
        _to_int(payload.get("p3_distance")),
        raw_json,
    ))
    new_id = cur.lastrowid
    conn.commit()
    conn.close()
    return new_id, created_at


def fetch_logs(limit=100, status_filter=None):
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    cur = conn.cursor()
    if status_filter in ("GRANTED", "DENIED"):
        cur.execute("""
            SELECT id, created_at, uid, access_status, message, free_spots,
                   p1_status, p1_distance,
                   p2_status, p2_distance,
                   p3_status, p3_distance
            FROM access_logs
            WHERE access_status = ?
            ORDER BY id DESC
            LIMIT ?
        """, (status_filter, limit))
    else:
        cur.execute("""
            SELECT id, created_at, uid, access_status, message, free_spots,
                   p1_status, p1_distance,
                   p2_status, p2_distance,
                   p3_status, p3_distance
            FROM access_logs
            ORDER BY id DESC
            LIMIT ?
        """, (limit,))
    rows = [dict(r) for r in cur.fetchall()]
    conn.close()
    return rows


def fetch_all_logs_for_csv():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    cur = conn.cursor()
    cur.execute("""
        SELECT id, created_at, uid, access_status, message, free_spots,
               p1_status, p1_distance,
               p2_status, p2_distance,
               p3_status, p3_distance
        FROM access_logs
        ORDER BY id ASC
    """)
    rows = [dict(r) for r in cur.fetchall()]
    conn.close()
    return rows


def fetch_stats():
    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()
    cur.execute("SELECT COUNT(*) FROM access_logs")
    total = cur.fetchone()[0]
    cur.execute("SELECT COUNT(*) FROM access_logs WHERE access_status = 'GRANTED'")
    granted = cur.fetchone()[0]
    cur.execute("SELECT COUNT(*) FROM access_logs WHERE access_status = 'DENIED'")
    denied = cur.fetchone()[0]
    cur.execute("""
        SELECT created_at, uid, free_spots
        FROM access_logs
        ORDER BY id DESC
        LIMIT 1
    """)
    row = cur.fetchone()
    conn.close()

    if row:
        last_event_time, last_uid, last_free_spots = row[0], row[1], row[2]
    else:
        last_event_time, last_uid, last_free_spots = None, None, None

    return {
        "total": total,
        "granted": granted,
        "denied": denied,
        "last_event_time": last_event_time,
        "last_uid": last_uid,
        "last_free_spots": last_free_spots,
    }


def clear_logs():
    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()
    cur.execute("DELETE FROM access_logs")
    # sqlite_sequence only exists after the first INSERT. Ignore if missing.
    try:
        cur.execute("DELETE FROM sqlite_sequence WHERE name='access_logs'")
    except sqlite3.OperationalError:
        pass
    conn.commit()
    conn.close()


def _to_int(v):
    try:
        if v is None or v == "":
            return None
        return int(v)
    except (TypeError, ValueError):
        return None


# ---------- HTML ----------
DASHBOARD_HTML = """<!DOCTYPE html>
<html lang="uk">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Parking - Local Logs</title>
<style>
  * { box-sizing: border-box; }
  body { font-family: Arial, sans-serif; background:#0f172a; color:#e2e8f0; margin:0; padding:20px; line-height:1.4; }
  .wrap { max-width: 1180px; margin: 0 auto; }
  h1 { color:#4ade80; margin:0 0 4px 0; font-size:22px; }
  .meta { color:#94a3b8; margin-bottom:16px; font-size:13px; }

  .cards { display:grid; grid-template-columns:repeat(auto-fit,minmax(160px,1fr)); gap:10px; margin-bottom:16px; }
  .card { background:#1e293b; padding:12px 14px; border-radius:8px; border:1px solid #1e293b; }
  .card .label { color:#94a3b8; font-size:11px; text-transform:uppercase; letter-spacing:.6px; }
  .card .value { color:#e2e8f0; font-size:22px; font-weight:bold; margin-top:2px; word-break:break-all; }
  .card.granted .value { color:#4ade80; }
  .card.denied .value { color:#f87171; }

  .recent { background:#1e293b; border-radius:8px; padding:12px 14px; margin-bottom:16px; }
  .recent h2 { margin:0 0 10px; color:#93c5fd; font-size:12px; text-transform:uppercase; letter-spacing:1px; }
  .recent ul { list-style:none; padding:0; margin:0; }
  .recent li { display:flex; gap:10px; padding:4px 0; border-bottom:1px solid #334155; font-size:13px; flex-wrap:wrap; }
  .recent li:last-child { border:none; }
  .recent .t { color:#94a3b8; font-family:monospace; min-width:155px; }
  .recent .u { font-family:monospace; min-width:140px; }
  .recent .m { color:#94a3b8; flex:1; }

  .toolbar { display:flex; flex-wrap:wrap; gap:8px; margin-bottom:12px; align-items:center; }
  .toolbar button, .toolbar a.btn {
    background:#1e293b; color:#e2e8f0; border:1px solid #334155;
    padding:7px 14px; border-radius:6px; cursor:pointer; font-size:13px; text-decoration:none; font-family:inherit;
  }
  .toolbar button:hover, .toolbar a.btn:hover { background:#334155; }
  .toolbar button.active { background:#1d4ed8; border-color:#1d4ed8; color:#fff; }
  .toolbar button.danger { color:#fca5a5; border-color:#7f1d1d; }
  .toolbar button.danger:hover { background:#7f1d1d; color:#fff; }
  .toolbar input[type="search"] {
    background:#0f172a; color:#e2e8f0; border:1px solid #334155;
    padding:7px 10px; border-radius:6px; font-size:13px; min-width:180px; font-family:inherit;
  }
  .toolbar .sep { flex:1; }

  table { border-collapse: collapse; width:100%; background:#1e293b; border-radius:8px; overflow:hidden; }
  th, td { padding:8px 10px; border-bottom:1px solid #334155; font-size:13px; text-align:left; }
  tr:last-child td { border-bottom:none; }
  th { background:#111827; color:#93c5fd; position:sticky; top:0; font-weight:normal; text-transform:uppercase; font-size:11px; letter-spacing:.5px; }

  .badge { display:inline-block; padding:3px 10px; border-radius:999px; font-size:11px; font-weight:bold; letter-spacing:.5px; }
  .badge.granted { background:#14532d; color:#86efac; }
  .badge.denied  { background:#7f1d1d; color:#fca5a5; }
  .badge.full    { background:#78350f; color:#fcd34d; }
  .badge.wrong   { background:#7f1d1d; color:#fca5a5; }

  .FREE { color:#4ade80; }
  .BUSY { color:#f87171; }
  .empty { color:#94a3b8; padding:20px; text-align:center; }

  @media (max-width: 560px) {
    .toolbar input[type="search"] { min-width:120px; flex:1; }
    th, td { padding:6px 6px; font-size:12px; }
  }
</style>
</head>
<body>
<div class="wrap">
  <h1>Smart Parking — Local Logs</h1>
  <div class="meta">Auto refresh every 2 s · Database: smart_parking.db</div>

  <div class="cards" id="cards"></div>

  <div class="recent">
    <h2>Recent Activity</h2>
    <ul id="recent-list"><li><span class="m">Loading...</span></li></ul>
  </div>

  <div class="toolbar">
    <button id="f-ALL"     onclick="setFilter('ALL')">All</button>
    <button id="f-GRANTED" onclick="setFilter('GRANTED')">Granted</button>
    <button id="f-DENIED"  onclick="setFilter('DENIED')">Denied</button>
    <button id="f-FULL"    onclick="setFilter('FULL')">Parking Full</button>
    <button id="f-WRONG"   onclick="setFilter('WRONG')">Wrong Card</button>
    <input type="search" id="search-uid" placeholder="Search UID..." oninput="onSearch()">
    <span class="sep"></span>
    <a class="btn" href="/download.csv">Download CSV</a>
    <button class="danger" onclick="clearLogs()">Clear logs</button>
  </div>

  <div id="content"><div class="empty">Loading...</div></div>
</div>

<script>
let currentFilter = "ALL";
let searchUid = "";
let allLogs = [];

function statusBadge(row) {
  const s = row.access_status || "";
  const m = (row.message || "").toLowerCase();
  if (s === "GRANTED") return '<span class="badge granted">GRANTED</span>';
  if (s === "DENIED" && m.indexOf("full") !== -1)  return '<span class="badge full">PARKING FULL</span>';
  if (s === "DENIED" && m.indexOf("wrong") !== -1) return '<span class="badge wrong">WRONG CARD</span>';
  if (s === "DENIED") return '<span class="badge denied">DENIED</span>';
  return '<span>' + (s || "-") + '</span>';
}
function slotCls(s) { return s === "FREE" ? "FREE" : (s === "BUSY" ? "BUSY" : ""); }

function matchesFilter(row) {
  const m = (row.message || "").toLowerCase();
  if (currentFilter === "ALL")     return true;
  if (currentFilter === "GRANTED") return row.access_status === "GRANTED";
  if (currentFilter === "DENIED")  return row.access_status === "DENIED";
  if (currentFilter === "FULL")    return row.access_status === "DENIED" && m.indexOf("full")  !== -1;
  if (currentFilter === "WRONG")   return row.access_status === "DENIED" && m.indexOf("wrong") !== -1;
  return true;
}
function matchesSearch(row) {
  if (!searchUid) return true;
  return (row.uid || "").toLowerCase().indexOf(searchUid) !== -1;
}

function setFilter(f) {
  currentFilter = f;
  ["ALL","GRANTED","DENIED","FULL","WRONG"].forEach(function(k){
    const el = document.getElementById("f-" + k);
    if (el) el.classList.toggle("active", k === f);
  });
  applyAndRender();
}
function onSearch() {
  searchUid = (document.getElementById("search-uid").value || "").toLowerCase().trim();
  applyAndRender();
}

function renderCards(stats) {
  if (!stats) return;
  const c = document.getElementById("cards");
  function cell(label, value, extraCls) {
    return '<div class="card ' + (extraCls || "") + '">'
      + '<div class="label">' + label + '</div>'
      + '<div class="value">' + (value == null || value === "" ? "—" : value) + '</div></div>';
  }
  c.innerHTML = ""
    + cell("Total events",     stats.total)
    + cell("Granted",          stats.granted, "granted")
    + cell("Denied",           stats.denied,  "denied")
    + cell("Last UID",         stats.last_uid)
    + cell("Last event time",  stats.last_event_time)
    + cell("Last free spots",  stats.last_free_spots);
}

function renderRecent(rows) {
  const ul = document.getElementById("recent-list");
  if (!rows || rows.length === 0) {
    ul.innerHTML = '<li><span class="m">No events yet.</span></li>';
    return;
  }
  const top = rows.slice(0, 5);
  let html = "";
  for (const r of top) {
    html += '<li>'
      + '<span class="t">' + (r.created_at || "") + '</span>'
      + statusBadge(r)
      + '<span class="u">' + (r.uid || "—") + '</span>'
      + '<span class="m">' + (r.message || "") + '</span>'
      + '</li>';
  }
  ul.innerHTML = html;
}

function renderTable(rows) {
  if (!rows || rows.length === 0) {
    document.getElementById("content").innerHTML = '<div class="empty">No logs to show.</div>';
    return;
  }
  let html = '<table><thead><tr>'
    + '<th>ID</th><th>Time</th><th>UID</th><th>Status</th>'
    + '<th>Free</th>'
    + '<th>P1</th><th>cm</th>'
    + '<th>P2</th><th>cm</th>'
    + '<th>P3</th><th>cm</th>'
    + '<th>Message</th></tr></thead><tbody>';
  for (const r of rows) {
    html += '<tr>'
      + '<td>' + r.id + '</td>'
      + '<td>' + (r.created_at || "") + '</td>'
      + '<td>' + (r.uid || "") + '</td>'
      + '<td>' + statusBadge(r) + '</td>'
      + '<td>' + (r.free_spots == null ? "" : r.free_spots) + '</td>'
      + '<td class="' + slotCls(r.p1_status) + '">' + (r.p1_status || "") + '</td>'
      + '<td>' + (r.p1_distance == null ? "" : r.p1_distance) + '</td>'
      + '<td class="' + slotCls(r.p2_status) + '">' + (r.p2_status || "") + '</td>'
      + '<td>' + (r.p2_distance == null ? "" : r.p2_distance) + '</td>'
      + '<td class="' + slotCls(r.p3_status) + '">' + (r.p3_status || "") + '</td>'
      + '<td>' + (r.p3_distance == null ? "" : r.p3_distance) + '</td>'
      + '<td>' + (r.message || "") + '</td>'
      + '</tr>';
  }
  html += '</tbody></table>';
  document.getElementById("content").innerHTML = html;
}

function applyAndRender() {
  const filtered = allLogs.filter(function(r){ return matchesFilter(r) && matchesSearch(r); });
  renderRecent(allLogs);   // recent always shows latest across filters
  renderTable(filtered);
}

async function refresh() {
  try {
    const [statsRes, logsRes] = await Promise.all([
      fetch("/api/stats"),
      fetch("/api/logs"),
    ]);
    const stats = await statsRes.json();
    const logs  = await logsRes.json();
    if (stats && stats.ok) renderCards(stats);
    if (logs && logs.ok)   { allLogs = logs.logs; applyAndRender(); }
  } catch (e) { /* network blip — keep last render */ }
}
async function clearLogs() {
  if (!confirm("Delete all logs?")) return;
  try {
    const res  = await fetch("/api/clear", { method: "POST" });
    const data = await res.json();
    if (!data.ok) alert("Clear failed: " + (data.error || "unknown"));
  } catch (e) {
    alert("Clear failed: " + e);
  }
  refresh();
}

setFilter("ALL");
refresh();
setInterval(refresh, 2000);
</script>
</body>
</html>
"""


# ---------- HTTP handler ----------
class Handler(http.server.BaseHTTPRequestHandler):

    def log_message(self, fmt, *args):
        # Keep console clean; we print our own [LOG SAVED] line on success.
        return

    def _send_json(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def _send_html(self, code, html):
        body = html.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_csv(self, code, body_bytes, filename):
        self.send_response(code)
        self.send_header("Content-Type", "text/csv; charset=utf-8")
        self.send_header("Content-Disposition", 'attachment; filename="{}"'.format(filename))
        self.send_header("Content-Length", str(len(body_bytes)))
        self.end_headers()
        self.wfile.write(body_bytes)

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if path == "/" or path == "/index.html":
            self._send_html(200, DASHBOARD_HTML)
            return

        if path == "/api/health":
            self._send_json(200, {"ok": True, "message": "Smart Parking logger is running"})
            return

        if path == "/api/logs":
            try:
                qs = parse_qs(parsed.query)
                status = (qs.get("status", ["ALL"])[0] or "ALL").upper()
                if status not in ("ALL", "GRANTED", "DENIED"):
                    status = "ALL"
                rows = fetch_logs(limit=100, status_filter=status if status != "ALL" else None)
                self._send_json(200, {"ok": True, "logs": rows, "filter": status})
            except Exception as e:
                self._send_json(500, {"ok": False, "error": str(e)})
            return

        if path == "/api/stats":
            try:
                stats = fetch_stats()
                resp = {"ok": True}
                resp.update(stats)
                self._send_json(200, resp)
            except Exception as e:
                self._send_json(500, {"ok": False, "error": str(e)})
            return

        if path == "/download.csv":
            try:
                rows = fetch_all_logs_for_csv()
                buf = io.StringIO()
                writer = csv.writer(buf, lineterminator="\n")
                writer.writerow([
                    "id", "created_at", "uid", "access_status", "message",
                    "free_spots",
                    "p1_status", "p1_distance",
                    "p2_status", "p2_distance",
                    "p3_status", "p3_distance",
                ])
                for r in rows:
                    writer.writerow([
                        r["id"], r["created_at"], r["uid"], r["access_status"], r["message"],
                        r["free_spots"],
                        r["p1_status"], r["p1_distance"],
                        r["p2_status"], r["p2_distance"],
                        r["p3_status"], r["p3_distance"],
                    ])
                body = buf.getvalue().encode("utf-8")
                self._send_csv(200, body, "smart_parking_logs.csv")
            except Exception as e:
                self._send_json(500, {"ok": False, "error": str(e)})
            return

        self._send_json(404, {"ok": False, "error": "Not found"})

    def do_POST(self):
        path = urlparse(self.path).path

        if path == "/api/log":
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                length = 0

            raw = self.rfile.read(length) if length > 0 else b""
            try:
                raw_text = raw.decode("utf-8", errors="replace")
            except Exception:
                raw_text = ""

            try:
                payload = json.loads(raw_text) if raw_text else {}
                if not isinstance(payload, dict):
                    raise ValueError("JSON must be an object")
            except Exception as e:
                self._send_json(400, {"ok": False, "error": "Invalid JSON: " + str(e)})
                return

            try:
                new_id, created_at = insert_log(payload, raw_text)
                print("[LOG SAVED] #{} {} | {} | {} | {}".format(
                    new_id,
                    created_at,
                    payload.get("uid", ""),
                    payload.get("access_status", ""),
                    payload.get("message", ""),
                ), flush=True)
                self._send_json(200, {"ok": True, "id": new_id})
            except Exception as e:
                self._send_json(500, {"ok": False, "error": str(e)})
            return

        if path == "/api/clear":
            try:
                clear_logs()
                print("[LOGS CLEARED]", flush=True)
                self._send_json(200, {"ok": True, "message": "Logs cleared"})
            except Exception as e:
                self._send_json(500, {"ok": False, "error": str(e)})
            return

        self._send_json(404, {"ok": False, "error": "Not found"})


class ThreadingHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    init_db()
    print("Smart Parking local logger")
    print("DB file : {}".format(DB_PATH))
    print("Listen  : http://{}:{}".format(HOST, PORT))
    print("Dashboard on this PC: http://localhost:{}".format(PORT))
    print("Endpoints:")
    print("  GET  /")
    print("  GET  /api/health")
    print("  GET  /api/logs[?status=ALL|GRANTED|DENIED]")
    print("  GET  /api/stats")
    print("  GET  /download.csv")
    print("  POST /api/log")
    print("  POST /api/clear")
    print("Press Ctrl+C to stop.")
    try:
        with ThreadingHTTPServer((HOST, PORT), Handler) as httpd:
            httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
        sys.exit(0)


if __name__ == "__main__":
    main()
