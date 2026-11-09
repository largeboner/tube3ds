#!/bin/sh
# Tube3DS server launcher for macOS / Linux / Raspberry Pi
cd "$(dirname "$0")"
echo "Updating yt-dlp..."
python3 -m pip install -U -r requirements.txt || python3 -m pip install --user -U -r requirements.txt
python3 tube3ds_server.py "$@"
