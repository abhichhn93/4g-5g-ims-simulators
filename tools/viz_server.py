#!/usr/bin/env python3
# viz_server.py — WebSocket + HTTP server for the telecom simulator visualizer.
# Tails sim_events.jsonl, enriches events with interview questions via
# tag_rules.json + interview_questions/*.yaml, then broadcasts over WebSocket.
# Usage:  python3 tools/viz_server.py   (from repo root or any simulator dir)
#         pip install websockets pyyaml  (only two external deps)

import asyncio
import glob
import json
import os
import pathlib
import sys
import time
import threading
from http.server import HTTPServer, SimpleHTTPRequestHandler

try:
    import websockets
except ImportError:
    print("[viz_server] ERROR: websockets not installed — run: pip install websockets pyyaml")
    sys.exit(1)

try:
    import yaml
except ImportError:
    print("[viz_server] ERROR: pyyaml not installed — run: pip install websockets pyyaml")
    sys.exit(1)

# ── Configuration ──────────────────────────────────────────────────────────────
REPO_ROOT    = pathlib.Path(__file__).parent.parent.resolve()
JSONL_PATHS  = [
    REPO_ROOT / "sim_events.jsonl",
    REPO_ROOT / "4g-simulator" / "sim_events.jsonl",
    REPO_ROOT / "5g-simulator" / "sim_events.jsonl",
    REPO_ROOT / "ims-simulator" / "sim_events.jsonl",
    pathlib.Path("sim_events.jsonl").resolve(),   # CWD
]
TAG_RULES_PATH = REPO_ROOT / "tag_rules.json"
QUESTIONS_GLOB = str(REPO_ROOT / "interview_questions" / "*.yaml")
WS_PORT   = 8765
HTTP_PORT = 8080
UI_FILE   = pathlib.Path(__file__).parent / "viz_ui.html"

# ── Load tag rules ─────────────────────────────────────────────────────────────
def load_tag_rules():
    if not TAG_RULES_PATH.exists():
        print(f"[viz_server] WARNING: tag_rules.json not found at {TAG_RULES_PATH}")
        return []
    with open(TAG_RULES_PATH) as f:
        data = json.load(f)
    return data.get("message_patterns", [])

# ── Load interview questions ────────────────────────────────────────────────────
def load_questions():
    qs = []
    for path in glob.glob(QUESTIONS_GLOB):
        try:
            with open(path) as f:
                data = yaml.safe_load(f)
            if isinstance(data, list):
                qs.extend(data)
            elif isinstance(data, dict) and "questions" in data:
                qs.extend(data["questions"])
        except Exception as e:
            print(f"[viz_server] WARNING: could not parse {path}: {e}")
    return qs

# ── Tag matching ───────────────────────────────────────────────────────────────
def get_tags_for_msg(msg: str, tag_rules: list) -> list:
    tags = set()
    for rule in tag_rules:
        pattern = rule.get("pattern", "")
        if pattern and pattern.lower() in msg.lower():
            tags.update(rule.get("auto_tags", []))
    return list(tags)

def find_top_questions(tags: list, questions: list, n: int = 3) -> list:
    if not tags:
        return []
    scored = []
    tag_set = set(tags)
    for q in questions:
        q_tags = set(q.get("tags", []))
        overlap = len(tag_set & q_tags)
        if overlap > 0:
            scored.append((overlap, q))
    scored.sort(key=lambda x: -x[0])
    return [q for _, q in scored[:n]]

# ── JSONL tailer ───────────────────────────────────────────────────────────────
def find_jsonl():
    for p in JSONL_PATHS:
        if p.exists():
            return p
    return JSONL_PATHS[0]   # default (may not exist yet)

class JsonlTailer:
    def __init__(self):
        self._pos = {}       # path → file position

    def poll(self, path: pathlib.Path) -> list[dict]:
        events = []
        if not path.exists():
            return events
        key = str(path)
        pos  = self._pos.get(key, 0)
        with open(path, "r", errors="replace") as f:
            f.seek(pos)
            for line in f:
                line = line.strip()
                if line:
                    try:
                        events.append(json.loads(line))
                    except json.JSONDecodeError:
                        pass
            self._pos[key] = f.tell()
        return events

# ── Global state ───────────────────────────────────────────────────────────────
connected_clients: set = set()
tag_rules   = load_tag_rules()
questions   = load_questions()
print(f"[viz_server] Loaded {len(tag_rules)} tag rules, {len(questions)} interview questions")

# ── Enrichment ────────────────────────────────────────────────────────────────
def enrich(event: dict) -> dict:
    msg  = event.get("msg", "")
    tags = get_tags_for_msg(msg, tag_rules)
    matched_qs = find_top_questions(tags, questions)
    event["_tags"] = tags
    event["_matched_questions"] = [
        {
            "question": q.get("question", q.get("q", "")),
            "answer":   q.get("answer",   q.get("a", "")),
            "tags":     q.get("tags", []),
            "level":    q.get("level", "engineer"),
            "domain":   q.get("domain", ""),
        }
        for q in matched_qs
    ]
    return event

# ── WebSocket handler ──────────────────────────────────────────────────────────
async def ws_handler(websocket):
    connected_clients.add(websocket)
    print(f"[viz_server] Client connected (total={len(connected_clients)})")
    try:
        # Send a "connected" ping
        await websocket.send(json.dumps({"type": "connected",
                                          "questions_loaded": len(questions),
                                          "tag_rules": len(tag_rules)}))
        await websocket.wait_closed()
    finally:
        connected_clients.discard(websocket)
        print(f"[viz_server] Client disconnected (total={len(connected_clients)})")

async def broadcast(msg: str):
    dead = set()
    for ws in connected_clients:
        try:
            await ws.send(msg)
        except Exception:
            dead.add(ws)
    connected_clients.difference_update(dead)

# ── Poll loop ─────────────────────────────────────────────────────────────────
async def poll_loop():
    tailer   = JsonlTailer()
    jsonl_path = find_jsonl()
    print(f"[viz_server] Watching {jsonl_path} (will wait for file to appear)")
    while True:
        # Re-discover path each tick in case file appears after startup
        p = find_jsonl()
        events = tailer.poll(p)
        for ev in events:
            ev = enrich(ev)
            msg = json.dumps({"type": "event", "data": ev})
            await broadcast(msg)
        await asyncio.sleep(0.25)   # 250ms polling — fast enough for demo

# ── HTTP server (serves viz_ui.html) ──────────────────────────────────────────
class VizHandler(SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path in ("/", "/index.html", "/viz_ui.html"):
            if UI_FILE.exists():
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                content = UI_FILE.read_bytes()
                self.send_header("Content-Length", str(len(content)))
                self.end_headers()
                self.wfile.write(content)
            else:
                self.send_response(404)
                self.end_headers()
                self.wfile.write(b"viz_ui.html not found — check tools/viz_ui.html")
        else:
            super().do_GET()

    def log_message(self, fmt, *args):
        pass  # suppress HTTP access logs

def start_http_server():
    os.chdir(str(pathlib.Path(__file__).parent))   # serve files from tools/
    httpd = HTTPServer(("0.0.0.0", HTTP_PORT), VizHandler)
    print(f"[viz_server] HTTP  listening on http://localhost:{HTTP_PORT}")
    httpd.serve_forever()

# ── Main ───────────────────────────────────────────────────────────────────────
async def main():
    print(f"[viz_server] WebSocket listening on ws://localhost:{WS_PORT}")
    print(f"[viz_server] Open http://localhost:{HTTP_PORT} in your browser")

    # HTTP server in background thread (stdlib — no event loop needed)
    t = threading.Thread(target=start_http_server, daemon=True)
    t.start()

    async with websockets.serve(ws_handler, "0.0.0.0", WS_PORT):
        await poll_loop()   # runs forever

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[viz_server] Stopped.")
