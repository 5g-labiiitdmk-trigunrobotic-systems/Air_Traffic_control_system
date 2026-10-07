"""
IIITDM KURNOOL — DRONE ATC LAUNCHER
=====================================
Run this on any laptop: python start.py

It will:
  1. Start localhost.run SSH tunnel (localhost:5000 → internet)
  2. Capture the public HTTPS URL
  3. Push that URL to a GitHub Gist (ESP reads this on boot)
  4. Start Flask (app.py)

ONE-TIME SETUP:
  1. pip install flask requests
  2. Create a GitHub Personal Access Token:
       github.com → Settings → Developer Settings →
       Personal Access Tokens (classic) → Generate
       Scope: check "gist"
  3. Go to gist.github.com → New gist
       Filename: drone_server.txt   Content: placeholder
       Click "Create secret gist"
       Copy the GIST_ID from the URL
  4. Fill GITHUB_TOKEN and GIST_ID below and save
  5. Run: python start.py
     It will print a permanent raw URL — paste that into
     drone2.ino as GIST_RAW_URL, then reflash the ESP once.

EVERY TIME AFTER THAT:
  Just run: python start.py
  The ESP discovers the new tunnel URL automatically.
"""

import subprocess
import threading
import time
import re
import sys
import os
import socket
import json
import urllib.request
import urllib.error

# Fix windows console unicode errors
if sys.stdout.encoding != 'utf-8':
    sys.stdout.reconfigure(encoding='utf-8')

# ── !! FILL THESE IN ONCE !! ─────────────────────────────────────────────────
GIST_ID       = ""   # fill in your Gist ID
GITHUB_TOKEN  = ""   # fill in ghp_xxxx token
GIST_FILENAME = "drone_server.txt"
SERVER_PORT   = 5000
# ─────────────────────────────────────────────────────────────────────────────


# localhost.run — no account, no password, works on Windows/Linux/Mac
TUNNEL_CMD = [
    "ssh",
    "-o", "StrictHostKeyChecking=no",
    "-o", "ServerAliveInterval=30",
    "-R", "80:127.0.0.1:5000",
    "nokey@localhost.run"
]

tunnel_url = None


def start_tunnel():
    global tunnel_url
    print("[TUNNEL] Starting localhost.run tunnel...")
    proc = subprocess.Popen(
        TUNNEL_CMD,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,   # prevents password prompt hang
        text=True
    )
    # localhost.run URL format: https://xxxx.lhr.life
    pattern = re.compile(r'https://[a-z0-9\-]+\.lhr\.life', re.IGNORECASE)

    def read_output():
        global tunnel_url
        for line in proc.stdout:
            line = line.strip()
            if line:
                print(f"[TUNNEL] {line}")
            m = pattern.search(line)
            if m and not tunnel_url:
                tunnel_url = m.group(0)
                print(f"\n[TUNNEL] ✓ Public URL: {tunnel_url}\n")

    t = threading.Thread(target=read_output, daemon=True)
    t.start()
    return proc


def save_tunnel_url(url):
    """Stdlib-only local record of the public tunnel URL (no Gist push here
    anymore — the Gist now always holds the LAN IP via update_gist() below,
    which is what the drones actually read)."""
    try:
        with open("current_tunnel_url.txt", "w") as f:
            f.write(url + "\n")
        print(f"[TUNNEL]   Saved to current_tunnel_url.txt: {url}")
    except Exception as e:
        print(f"[TUNNEL]   Could not save current_tunnel_url.txt: {e}")


def get_local_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        try:
            return socket.gethostbyname(socket.gethostname())
        except Exception:
            return "127.0.0.1"


def update_gist(ip):
    if not GIST_ID or not GITHUB_TOKEN:
        print("[GIST] ⚠  GIST_ID / GITHUB_TOKEN not set — skipping Gist update.")
        return
    url = f"http://{ip}:{SERVER_PORT}"
    try:
        body = json.dumps({"files": {GIST_FILENAME: {"content": url}}}).encode("utf-8")
        req = urllib.request.Request(
            f"https://api.github.com/gists/{GIST_ID}",
            data=body,
            method="PATCH",
            headers={
                "Authorization": f"token {GITHUB_TOKEN}",
                "Accept": "application/vnd.github.v3+json",
                "Content-Type": "application/json",
            },
        )
        with urllib.request.urlopen(req, timeout=10) as resp:
            if resp.status == 200:
                print(f"[GIST] Updated → {url}")
            else:
                print(f"[GIST] Failed: {resp.status} — {resp.read()[:200]}")
    except urllib.error.HTTPError as e:
        print(f"[GIST] Failed: {e.code} — {e.reason}")
    except Exception as e:
        print(f"[GIST] Failed: {e}")


def start_flask():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    app_path   = os.path.join(script_dir, "app.py")
    print("[FLASK] Starting app.py ...\n")
    subprocess.run([sys.executable, app_path])


if __name__ == "__main__":
    tunnel_proc = start_tunnel()

    print("[INFO] Waiting for tunnel URL (up to 30s)...")
    for _ in range(30):
        if tunnel_url:
            break
        time.sleep(1)

    if not tunnel_url:
        print("[WARN] Could not capture tunnel URL. Flask starting anyway.")
    else:
        save_tunnel_url(tunnel_url)

    ip = get_local_ip()
    update_gist(ip)
    print(f"[SERVER] Running on http://{ip}:{SERVER_PORT}")
    print(f"[SERVER] Pilot portal: http://{ip}:{SERVER_PORT}/pilot")
    print(f"[SERVER] ATC Dashboard: http://{ip}:{SERVER_PORT}")

    start_flask()