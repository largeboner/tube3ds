# Tube3DS — YouTube on a modded 3DS

Search YouTube and watch videos with sound on a homebrew-enabled Nintendo 3DS.

## How it works

A 3DS can't talk to YouTube directly. Its CPU is too slow for YouTube's
encryption, JavaScript checks and video formats. So Tube3DS has two parts:

```
 3DS (Tube3DS app)  <--- home Wi-Fi --->  your PC (tube3ds_server.py)  <--->  YouTube
   shows 400x240 JPEG frames               yt-dlp finds the video,
   plays PCM audio                         ffmpeg shrinks it for the 3DS
```

The PC (or a Raspberry Pi) does the hard work. The 3DS only has to show
small pictures and play plain audio.

## What you need

- A 3DS / 2DS / New 3DS with custom firmware (Luma3DS) and the Homebrew Launcher.
- **For sound:** a DSP firmware dump. Run the **DSP1** homebrew once; it creates
  `sd:/3ds/dspfirm.cdc`. Without it, video still plays, just silently.
- A computer on the **same Wi-Fi network** with:
  - Python 3.9 or newer — https://www.python.org
  - ffmpeg — https://ffmpeg.org/download.html (it must work when you type `ffmpeg` in a terminal)

---

## 1. Start the server on your computer

**Windows:** double-click `server/start_server_windows.bat`.

**macOS / Linux / Raspberry Pi:**

```sh
cd server
sh start_server.sh
```

It prints a line like:

```
On your 3DS, enter this server address:  192.168.1.20:8080
```

Write that address down. If Windows asks about the firewall, click **Allow**
(private networks).

## 2. Install the 3DS app with a QR code (FBI)

On your 3DS, open **FBI → Remote Install → Scan QR Code** and scan this:

![Scan with FBI to install Tube3DS](https://github.com/largeboner/tube3ds/releases/latest/download/tube3ds-qr.png)

FBI downloads and installs **Tube3DS** to your HOME Menu. The QR code always
points to the newest build
(`https://github.com/largeboner/tube3ds/releases/latest/download/tube3ds.cia`),
so scanning it again later updates the app.

GitHub builds a new release automatically every time something is pushed to
`main` (see the **Actions** tab). If the QR code above is missing, the first
build hasn't finished yet, or it failed.

**Prefer the Homebrew Launcher?** Each release also has `tube3ds.3dsx`.
Copy it to `sd:/3ds/` and open it from the Homebrew Launcher.

**Building it yourself instead**

1. Install devkitPro: https://devkitpro.org/wiki/Getting_Started
2. Install the 3DS libraries:
   `sudo dkp-pacman -S 3ds-dev 3ds-libjpeg-turbo`
   (On Windows, run `pacman -S 3ds-dev 3ds-libjpeg-turbo` in the devkitPro MSYS2 shell.)
3. Run `make` in `client/` to get the `.3dsx`. To get the `.cia` as well,
   put [makerom](https://github.com/3DSGuy/Project_CTR/releases) and
   [bannertool](https://github.com/Epicpkmn11/bannertool/releases) on your
   PATH and run `make cia`.

## 3. Run it

1. Open **Tube3DS** from the HOME Menu.
2. On first launch, type in the server address from step 1.
3. Press **Y** to search. You can also paste a YouTube video or playlist link.

## Controls

| Where       | Button        | Action                         |
|-------------|---------------|--------------------------------|
| List        | Y             | Search                         |
|             | A             | Play the selected video        |
|             | Up / Down     | Move through results           |
|             | L / R         | Page up / down                 |
|             | SELECT        | Change the server address      |
|             | START         | Quit                           |
| Video       | A             | Pause / resume                 |
|             | Left / Right  | Seek 10 seconds                |
|             | L / R         | Seek 1 minute                  |
|             | X             | Turn the bottom screen light on/off (saves battery) |
|             | B             | Back to the list               |

## Settings

Settings are saved in `sd:/3ds/tube3ds/config.txt`:

```
server=192.168.1.20:8080
fps=15          # frames per second, 1-30
quality=7       # JPEG quality: 2 = best/largest, 31 = worst/smallest
audio_rate=32000
stereo=0
```

The defaults are 15 fps on a New 3DS and 10 fps on an original 3DS.

## Troubleshooting

- **"Can't reach the server"**: check that the server window is still open,
  that both devices are on the same Wi-Fi, and that your firewall allows port 8080.
  Some guest networks block devices from seeing each other ("AP isolation").
- **The 3DS won't connect to Wi-Fi at all**: the 3DS only supports 2.4 GHz
  networks with WPA2 (not WPA3).
- **Search or playback suddenly fails with a server error**: YouTube changes
  things often. Close the server and run the start script again, which updates
  yt-dlp. You can also run `pip install -U yt-dlp` yourself.
- **Video is choppy, or "Buffer" stays near 0**: the 3DS's Wi-Fi is slow. Lower
  `fps` (try 10) or raise `quality` (try 10–12) in `config.txt`. The bottom
  screen shows the network speed to help you tune it.
- **No sound**: run DSP1 once (see "What you need").
- **Age-restricted videos**: export your browser cookies to `cookies.txt` and start
  the server with `python3 tube3ds_server.py --cookies cookies.txt`.

### Testing without YouTube

`python3 tube3ds_server.py --local some_video.mp4` serves that file for every
search and play request. Use it to check the 3DS side on its own.

## Files

```
client/            3DS app (C, libctru, libjpeg-turbo)
  cia/               banner, sound and makerom settings for the .cia
  source/main.c      menus, search, settings
  source/player.c    streaming, JPEG decoding, audio, A/V sync
  source/net.c       small HTTP client
server/            companion server (Python)
  tube3ds_server.py
.github/workflows/ automatic GitHub build: .3dsx, .cia and the FBI QR code
```

### Stream format (for anyone extending it)

`GET /stream` returns the bytes `T3DS\x01`, followed by a series of packets.
Each packet is `type:1  length:u32le  pts_ms:u32le  payload`:

- `H` is the header: `width:u16 height:u16 fps:u16 rate:u32 channels:u8 duration_s:u32 title`
- `V` is one baseline JPEG frame (400x240)
- `A` is 100 ms of signed 16-bit little-endian PCM
- `E` marks the end of the stream
