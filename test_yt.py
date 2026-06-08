import subprocess
import json

try:
    result = subprocess.run(
        ["yt-dlp", "--flat-playlist", "-J", "https://www.youtube.com/feed/trending"],
        capture_output=True,
        text=True,
        check=True
    )
    data = json.loads(result.stdout)
    entries = data.get("entries", [])
    print(f"Got {len(entries)} entries.")
    if entries:
        print(f"First entry: {entries[0].get('title')} by {entries[0].get('channel', 'Unknown')}")
except Exception as e:
    print(f"Error: {e}")
