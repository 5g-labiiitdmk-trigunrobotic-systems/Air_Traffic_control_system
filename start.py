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
import requests

# ── !! FILL THESE IN ONCE !! ─────────────────────────────────────────────────
GITHUB_TOKEN = "PASTE_YOUR_NEW_TOKEN_HERE"
GIST_ID      = "71775a92c0a67d79d6974ad1e7fd83e8"
# ─────────────────────────────────────────────────────────────────────────────

GIST_FILE = "drone_server.txt"

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


def update_gist(url):
    if "PASTE_YOUR" in GITHUB_TOKEN:
        print("[GIST] ⚠  GitHub token not set — skipping Gist update.")
        print(f"[GIST]    Current tunnel URL: {url}")
        with open("current_tunnel_url.txt", "w") as f:
            f.write(url + "\n")
        print("[GIST]    Saved to current_tunnel_url.txt")
        return

    headers = {
        "Authorization": f"Bearer {GITHUB_TOKEN}",   # fine-grained tokens need Bearer
        "Accept": "application/vnd.github.v3+json"
    }
    try:
        r = requests.patch(
            f"https://api.github.com/gists/{GIST_ID}",
            json={"files": {GIST_FILE: {"content": url}}},
            headers=headers,
            timeout=10
        )
        if r.status_code == 200:
            owner = r.json()["owner"]["login"]
            perm  = f"https://gist.githubusercontent.com/{owner}/{GIST_ID}/raw/{GIST_FILE}"
            print(f"[GIST] ✓ Published successfully.")
            print(f"[GIST]   Permanent raw URL for ESP:")
            print(f"[GIST]   {perm}")
            with open("current_tunnel_url.txt", "w") as f:
                f.write(f"Tunnel URL : {url}\nGist raw   : {perm}\n")
        else:
            print(f"[GIST] ✗ Failed: {r.status_code} — {r.text[:200]}")
    except Exception as e:
        print(f"[GIST] ✗ Error: {e}")


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
        update_gist(tunnel_url)

    start_flask()