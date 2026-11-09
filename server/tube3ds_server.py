#!/usr/bin/env python3
"""
Tube3DS companion server.

The 3DS is far too slow to talk to YouTube directly (modern TLS, JavaScript
signature ciphers, VP9/AV1 video...). This server does the heavy lifting on a
PC / Raspberry Pi on the same Wi-Fi network:

  * /info                    -> "tube3ds 1"  (used by the 3DS to test the connection)
  * /search?q=QUERY          -> one result per line: id \t seconds \t title \t channel
  * /stream?id=ID&start=SEC&fps=N&q=N&rate=N&ch=N
                             -> a simple packet stream of 400x240 JPEG frames
                                and raw 16-bit PCM audio the 3DS can play.

Requirements:  Python 3.9+,  pip install -U yt-dlp,  ffmpeg on your PATH.
Run:           python3 tube3ds_server.py            (listens on port 8080)
"""

import argparse
import itertools
import queue
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import unicodedata
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import yt_dlp
except ImportError:  # pragma: no cover
    yt_dlp = None

PROTOCOL_VERSION = 1
MAGIC = b"T3DS" + bytes([PROTOCOL_VERSION])
WIDTH, HEIGHT = 400, 240           # 3DS top screen
AUDIO_CHUNK_MS = 100               # one audio packet = 100 ms of PCM
VIDEO_ID_RE = re.compile(r"^[A-Za-z0-9_-]{11}$")
# Low resolution is plenty for a 400x240 screen and keeps downloads small.
FORMAT = "bv*[height<=360]+ba/b[height<=360]/bv*[height<=480]+ba/b"

ARGS = None
_resolve_cache = {}                # video id -> (time, info, vfmt, afmt)
_resolve_lock = threading.Lock()


def log(*a):
    print(time.strftime("[%H:%M:%S]"), *a, flush=True)


def ascii_text(s, maxlen=200):
    """The 3DS text console only has ASCII glyphs."""
    s = unicodedata.normalize("NFKD", str(s or ""))
    s = s.encode("ascii", "ignore").decode("ascii")
    s = re.sub(r"\s+", " ", s).strip()
    return s[:maxlen] or "?"


# --------------------------------------------------------------------------
# YouTube access (yt-dlp)
# --------------------------------------------------------------------------

def ydl_opts(**extra):
    opts = {"quiet": True, "no_warnings": True, "skip_download": True, "noplaylist": False}
    if ARGS and ARGS.cookies:
        opts["cookiefile"] = ARGS.cookies
    opts.update(extra)
    return opts


def search(q, limit):
    q = q.strip()
    if not q:
        return []
    if re.match(r"^(https?://|www\.|youtu\.be/|youtube\.com/)", q, re.I):
        target = q if q.lower().startswith("http") else "https://" + q
    else:
        target = "ytsearch%d:%s" % (limit, q)
    with yt_dlp.YoutubeDL(ydl_opts(extract_flat="in_playlist")) as ydl:
        info = ydl.extract_info(target, download=False)
    entries = info.get("entries") if info.get("_type") == "playlist" else [info]
    out = []
    for e in itertools.islice(entries or [], limit):
        if not e:
            continue
        vid = e.get("id") or ""
        if not VIDEO_ID_RE.match(vid):
            continue  # skip channels / playlists / shorts shelves etc.
        out.append({
            "id": vid,
            "duration": int(e.get("duration") or 0),
            "title": ascii_text(e.get("title"), 150),
            "channel": ascii_text(e.get("channel") or e.get("uploader") or "", 60),
        })
    return out


def resolve(video_id):
    """Find direct media URLs for a video (cached so seeking is quick)."""
    with _resolve_lock:
        hit = _resolve_cache.get(video_id)
        if hit and time.time() - hit[0] < 1800:
            return hit[1:]
    with yt_dlp.YoutubeDL(ydl_opts(format=FORMAT)) as ydl:
        info = ydl.extract_info("https://www.youtube.com/watch?v=" + video_id, download=False)
    vfmt = afmt = None
    for f in info.get("requested_formats") or [info]:
        if vfmt is None and f.get("vcodec") not in (None, "none"):
            vfmt = f
        if afmt is None and f.get("acodec") not in (None, "none"):
            afmt = f
    if vfmt is None:
        raise RuntimeError("no playable video format found")
    with _resolve_lock:
        _resolve_cache[video_id] = (time.time(), info, vfmt, afmt)
    return info, vfmt, afmt


def ffmpeg_input(fmt, start):
    """ffmpeg input arguments for a yt-dlp format dict (or a local file path)."""
    if isinstance(fmt, str):
        return ["-ss", "%.3f" % start, "-i", fmt]
    args = []
    headers = fmt.get("http_headers") or {}
    if headers:
        args += ["-headers", "".join("%s: %s\r\n" % kv for kv in headers.items())]
    if start > 0:
        args += ["-ss", "%.3f" % start]
    return args + ["-i", fmt["url"]]


# --------------------------------------------------------------------------
# Stream production
# --------------------------------------------------------------------------

class Reader(threading.Thread):
    """Reads packets out of one ffmpeg pipe into a bounded queue."""

    def __init__(self, proc, kind, stop, chunk_bytes=0):
        super().__init__(daemon=True)
        self.proc, self.kind, self.stop, self.chunk_bytes = proc, kind, stop, chunk_bytes
        self.q = queue.Queue(maxsize=48)

    def put(self, item):
        while not self.stop.is_set():
            try:
                self.q.put(item, timeout=0.25)
                return True
            except queue.Full:
                pass
        return False

    def run(self):
        pipe = self.proc.stdout
        try:
            if self.kind == "V":
                self.read_jpegs(pipe)
            else:
                self.read_pcm(pipe)
        except (OSError, ValueError):
            pass
        self.put(None)  # end marker

    def read_jpegs(self, pipe):
        buf = b""
        while not self.stop.is_set():
            data = pipe.read1(65536) if hasattr(pipe, "read1") else pipe.read(65536)
            if not data:
                return
            buf += data
            while True:
                start = buf.find(b"\xff\xd8")
                if start < 0:
                    buf = b""
                    break
                end = buf.find(b"\xff\xd9", start + 2)
                if end < 0:
                    buf = buf[start:]
                    break
                if not self.put(buf[start:end + 2]):
                    return
                buf = buf[end + 2:]

    def read_pcm(self, pipe):
        while not self.stop.is_set():
            data = b""
            while len(data) < self.chunk_bytes:
                part = pipe.read(self.chunk_bytes - len(data))
                if not part:
                    break
                data += part
            if data:
                data = data[: len(data) - (len(data) % 4)] if len(data) % 4 else data
                if data and not self.put(data):
                    return
            if len(data) < self.chunk_bytes:
                return


def packet(kind, pts_ms, payload=b""):
    return kind.encode() + struct.pack("<II", len(payload), max(0, int(pts_ms))) + payload


def produce_stream(write, video_id, start, fps, quality, rate, channels):
    if ARGS.local:
        title, duration = "Local test: " + ARGS.local, probe_duration(ARGS.local)
        vsrc = asrc = ARGS.local
    else:
        info, vsrc, asrc = resolve(video_id)
        title, duration = ascii_text(info.get("title"), 150), int(info.get("duration") or 0)
        if info.get("is_live"):
            start = 0

    vf = ("fps={fps},scale={w}:{h}:force_original_aspect_ratio=decrease:flags=bilinear,"
          "pad={w}:{h}:(ow-iw)/2:(oh-ih)/2").format(fps=fps, w=WIDTH, h=HEIGHT)
    vcmd = ([ARGS.ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin"]
            + ffmpeg_input(vsrc, start)
            + ["-an", "-sn", "-vf", vf, "-pix_fmt", "yuvj420p",
               "-c:v", "mjpeg", "-q:v", str(quality), "-f", "mjpeg", "pipe:1"])
    if asrc is not None:
        ain = ffmpeg_input(asrc, start)
    else:  # video has no sound: send silence so the 3DS clock still runs
        ain = ["-f", "lavfi", "-i", "anullsrc=r=%d:cl=mono" % rate]
        if duration:
            ain += ["-t", str(max(1, duration - start))]
    acmd = ([ARGS.ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin"] + ain
            + ["-vn", "-sn", "-ac", str(channels), "-ar", str(rate), "-f", "s16le", "pipe:1"])

    stop = threading.Event()
    procs = []
    try:
        vproc = subprocess.Popen(vcmd, stdout=subprocess.PIPE, stdin=subprocess.DEVNULL)
        procs.append(vproc)
        aproc = subprocess.Popen(acmd, stdout=subprocess.PIPE, stdin=subprocess.DEVNULL)
        procs.append(aproc)
        chunk = rate * channels * 2 * AUDIO_CHUNK_MS // 1000
        vr, ar = Reader(vproc, "V", stop), Reader(aproc, "A", stop, chunk)
        vr.start()
        ar.start()

        start_ms = int(start * 1000)
        header = struct.pack("<HHHIBI", WIDTH, HEIGHT, fps, rate, channels, duration)
        write(MAGIC + packet("H", start_ms, header + title.encode("ascii")))

        vnext = anext = None
        vdone = adone = False
        vcount = 0
        asamples = 0
        while not (vdone and adone):
            if vnext is None and not vdone:
                vnext = vr.q.get()
                vdone = vnext is None
            if anext is None and not adone:
                anext = ar.q.get()
                adone = anext is None
            vpts = start_ms + vcount * 1000 // fps
            apts = start_ms + asamples * 1000 // rate
            # emit whichever packet comes first in time (audio slightly favoured)
            if anext is not None and (vnext is None or apts <= vpts):
                write(packet("A", apts, anext))
                asamples += len(anext) // (2 * channels)
                anext = None
            elif vnext is not None:
                write(packet("V", vpts, vnext))
                vcount += 1
                vnext = None
        write(packet("E", 0))
    finally:
        stop.set()
        for p in procs:
            try:
                p.kill()
                p.wait(timeout=2)
            except Exception:
                pass


def probe_duration(path):
    try:
        out = subprocess.run(
            [ARGS.ffmpeg.replace("ffmpeg", "ffprobe"), "-v", "error", "-show_entries",
             "format=duration", "-of", "default=nw=1:nk=1", path],
            capture_output=True, text=True, timeout=20).stdout
        return int(float(out.strip()))
    except Exception:
        return 0


# --------------------------------------------------------------------------
# HTTP
# --------------------------------------------------------------------------

def clamp(v, lo, hi, default):
    try:
        return max(lo, min(hi, int(float(v))))
    except (TypeError, ValueError):
        return default


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    server_version = "Tube3DS/1"

    def log_message(self, fmt, *args):
        pass

    def send_text(self, code, text):
        body = text.encode("ascii", "replace")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        qs = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}
        try:
            if url.path == "/info":
                self.send_text(200, "tube3ds %d\n" % PROTOCOL_VERSION)
            elif url.path == "/search":
                self.do_search(qs)
            elif url.path == "/stream":
                self.do_stream(qs)
            else:
                self.send_text(404, "not found")
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            log("client disconnected")
        except Exception as e:  # report the problem to the 3DS
            log("error:", repr(e))
            try:
                self.send_text(502, ascii_text(str(e), 300))
            except Exception:
                pass

    def do_search(self, qs):
        q = qs.get("q", "")
        limit = clamp(qs.get("n"), 1, 50, 30)
        log("search %r from %s" % (q, self.client_address[0]))
        if ARGS.local:
            results = [{"id": "localtest01", "duration": probe_duration(ARGS.local),
                        "title": "Local test video", "channel": "tube3ds"}]
        else:
            results = search(q, limit)
        lines = ["%s\t%d\t%s\t%s" % (r["id"], r["duration"], r["title"], r["channel"])
                 for r in results]
        self.send_text(200, "\n".join(lines) + "\n")

    def do_stream(self, qs):
        vid = qs.get("id", "")
        if not VIDEO_ID_RE.match(vid):
            return self.send_text(400, "bad video id")
        start = clamp(qs.get("start"), 0, 24 * 3600, 0)
        fps = clamp(qs.get("fps"), 1, 30, 15)
        quality = clamp(qs.get("q"), 2, 31, 7)
        rate = clamp(qs.get("rate"), 8000, 48000, 32000)
        channels = clamp(qs.get("ch"), 1, 2, 1)
        log("play %s from %ds (%d fps, q%d, %d Hz x%d) for %s"
            % (vid, start, fps, quality, rate, channels, self.client_address[0]))

        sent = {"headers": False}

        def write(data):
            if not sent["headers"]:
                self.send_response(200)
                self.send_header("Content-Type", "application/x-tube3ds")
                self.end_headers()
                sent["headers"] = True
            self.wfile.write(data)

        try:
            produce_stream(write, vid, start, fps, quality, rate, channels)
            log("finished %s" % vid)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            log("stopped %s" % vid)
        except Exception:
            if sent["headers"]:
                raise BrokenPipeError()  # too late for an error page
            raise


def lan_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))  # no packet is actually sent
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "this-computer's-ip"


def main():
    global ARGS
    ap = argparse.ArgumentParser(description="Tube3DS companion server")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--ffmpeg", default="ffmpeg", help="path to ffmpeg")
    ap.add_argument("--cookies", help="cookies.txt for yt-dlp (helps with age-gated videos)")
    ap.add_argument("--local", metavar="FILE", help="serve FILE for every request (testing without YouTube)")
    ARGS = ap.parse_args()

    if not shutil.which(ARGS.ffmpeg):
        sys.exit("ffmpeg not found. Install it and make sure it's on your PATH.")
    if yt_dlp is None and not ARGS.local:
        sys.exit("yt-dlp missing. Run:  pip install -U yt-dlp")

    srv = ThreadingHTTPServer((ARGS.host, ARGS.port), Handler)
    srv.daemon_threads = True
    print("Tube3DS server running.")
    print("On your 3DS, enter this server address:  %s:%d" % (lan_ip(), ARGS.port))
    print("(Ctrl+C to stop)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
