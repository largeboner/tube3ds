@echo off
cd /d "%~dp0"
echo Updating yt-dlp...
python -m pip install -U -r requirements.txt
echo.
python tube3ds_server.py
pause
