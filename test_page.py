import subprocess
import sys

print("Testing yt-dlp search pagination...")

cmd = [
    "yt-dlp",
    "--no-config",
    "--quiet",
    "--no-warnings",
    "--encoding", "utf-8",
    "--flat-playlist",
    "--dump-json",
    "ytsearch30:trending",
    "--playlist-start", "16",
    "--playlist-end", "30"
]

try:
    print(f"Running command: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, check=True)
    lines = result.stdout.strip().split('\n')
    valid_lines = [l for l in lines if l.strip()]
    print(f"Success! Output has {len(valid_lines)} items.")
    for i, line in enumerate(valid_lines[:3]):
        try:
            import json
            j = json.loads(line)
            print(f"  Item {i+16}: {j.get('title')} by {j.get('channel') or j.get('uploader')}")
        except Exception as e:
            print(f"  Item {i+16} parse error: {e}")
except Exception as e:
    print(f"Error executing yt-dlp pagination: {e}")
    if hasattr(e, 'stderr') and e.stderr:
        print(f"Stderr: {e.stderr}")
