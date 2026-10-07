# Drone ATC System

## Quick Start

### One-time setup
1. Create a GitHub Gist at https://gist.github.com
   - Filename: `drone_server.txt`
   - Content: `http://0.0.0.0:5000` (placeholder)
2. Get the Raw URL from the Gist → paste into `drone2.ino`'s `GIST_RAW_URL`
3. Create a GitHub token at https://github.com/settings/tokens/new
   - Scope: `gist` only
   - Paste into `start.py`'s `GITHUB_TOKEN` and `GIST_ID`

### Every time you run
1. Connect laptop to `Atc1` hotspot
2. Run: `python start.py`
   → Auto-updates Gist with current IP
   → Flask starts on port 5000
3. Open `http://localhost:5000` → login: `admin` / `iiitdmk@ece`
4. Power on drones → they auto-fetch IP from Gist → connect

### Drone IDs
- Drone 1: `"Drone-1"` in `drone2.ino`
- Drone 2: `"Drone-2"` in `drone2.ino` (flash separately)
