/*
 * Video player: a network thread pulls packets from the companion server into
 * two queues (JPEG frames and PCM audio). The main loop feeds audio to the DSP
 * and uses the amount of audio played as the clock that decides which video
 * frame to decode and show.
 */
#include "player.h"
#include "net.h"

#include <setjmp.h>
#include <stdio.h>      /* must come before jpeglib.h */
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>

#define SCREEN_W        400
#define SCREEN_H        240
#define VQ_CAP          120             /* video frames buffered */
#define AQ_CAP          200             /* 100 ms audio chunks buffered */
#define MAX_PACKET      (768 * 1024)
#define NUM_WB          24
#define WB_BYTES        20480           /* >= 100 ms of 48 kHz stereo s16 */
#define AUDIO_CH        0
#define PREBUFFER_MS    1500
#define SEEK_DELAY_MS   500
#define NET_STACK       (32 * 1024)

typedef struct { u8 *data; u32 len; u32 pts; } Packet;

typedef struct {
    Packet slot[AQ_CAP > VQ_CAP ? AQ_CAP : VQ_CAP];
    int head, count, cap;
} PQueue;

typedef struct {
    Conn conn;
    Thread thread;
    LightLock lock;
    PQueue vq, aq;
    volatile bool stop, eof, net_error;
    volatile u32 bytes;
    u16 width, height, fps;
    u32 rate, duration, start_ms;
    u8 channels;
    char title[160];
} Stream;

/* ------------------------------------------------------------------ queues */

static void q_init(PQueue *q, int cap) { memset(q, 0, sizeof(*q)); q->cap = cap; }

static Packet *q_front(PQueue *q) { return q->count ? &q->slot[q->head] : NULL; }
static Packet *q_at(PQueue *q, int i) { return &q->slot[(q->head + i) % q->cap]; }

static void q_pop(PQueue *q, Packet *out)
{
    *out = q->slot[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;
}

static void q_clear(PQueue *q)
{
    Packet p;
    while (q->count) { q_pop(q, &p); free(p.data); }
}

/* --------------------------------------------------------------- network */

static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }

static void net_thread(void *arg)
{
    Stream *st = (Stream *)arg;
    while (!st->stop) {
        u8 h[9];
        if (conn_read_full(&st->conn, h, 9) != 9) { if (!st->stop) st->net_error = true; break; }
        u32 len = rd32(h + 1), pts = rd32(h + 5);
        if (h[0] == 'E') break;
        if (len > MAX_PACKET) { st->net_error = true; break; }
        u8 *data = (u8 *)malloc(len ? len : 1);
        if (!data) { st->net_error = true; break; }
        if (len && conn_read_full(&st->conn, data, len) != (int)len) {
            free(data);
            if (!st->stop) st->net_error = true;
            break;
        }
        st->bytes += len + 9;

        PQueue *q = h[0] == 'V' ? &st->vq : h[0] == 'A' ? &st->aq : NULL;
        if (!q) { free(data); continue; }
        for (;;) {   /* wait for room; TCP back-pressure throttles the server */
            LightLock_Lock(&st->lock);
            if (q->count < q->cap) {
                Packet *p = &q->slot[(q->head + q->count) % q->cap];
                p->data = data; p->len = len; p->pts = pts;
                q->count++;
                LightLock_Unlock(&st->lock);
                break;
            }
            LightLock_Unlock(&st->lock);
            if (st->stop) { free(data); break; }
            svcSleepThread(5 * 1000 * 1000);
        }
    }
    st->eof = true;
}

static void read_error_body(Conn *c, char *err, size_t errsz)
{
    size_t n = 0;
    char ch;
    while (n + 1 < errsz && conn_read_full(c, &ch, 1) == 1)
        if (ch >= 32 && ch < 127) err[n++] = ch;
    err[n] = 0;
}

static int stream_open(Stream *st, const Settings *s, const char *id, u32 start_s,
                       char *err, size_t errsz)
{
    memset(st, 0, sizeof(*st));
    st->conn.fd = -1;
    q_init(&st->vq, VQ_CAP);
    q_init(&st->aq, AQ_CAP);
    LightLock_Init(&st->lock);

    char path[256];
    snprintf(path, sizeof(path), "/stream?id=%s&start=%lu&fps=%d&q=%d&rate=%d&ch=%d",
             id, (unsigned long)start_s, s->fps, s->quality, s->rate, s->channels);
    int r = http_open(&st->conn, s->host, s->port, path);
    if (r < 0) {
        snprintf(err, errsz, "Could not reach the server at %s:%d (error %d).", s->host, s->port, r);
        return -1;
    }
    if (st->conn.status != 200) {
        char body[200];
        read_error_body(&st->conn, body, sizeof(body));
        snprintf(err, errsz, "Server error %d: %s", st->conn.status, body);
        conn_close(&st->conn);
        return -1;
    }

    u8 magic[5], h[9], payload[15 + 200];
    if (conn_read_full(&st->conn, magic, 5) != 5 || memcmp(magic, "T3DS\x01", 5) ||
        conn_read_full(&st->conn, h, 9) != 9 || h[0] != 'H' ||
        rd32(h + 1) < 15 || rd32(h + 1) > sizeof(payload) ||
        conn_read_full(&st->conn, payload, rd32(h + 1)) != (int)rd32(h + 1)) {
        snprintf(err, errsz, "Server sent an unexpected reply. Are the app and server the same version?");
        conn_close(&st->conn);
        return -1;
    }
    u32 plen = rd32(h + 1);
    st->start_ms = rd32(h + 5);
    st->width = rd16(payload);
    st->height = rd16(payload + 2);
    st->fps = rd16(payload + 4);
    st->rate = rd32(payload + 6);
    st->channels = payload[10];
    st->duration = rd32(payload + 11);
    size_t tl = plen - 15;
    if (tl >= sizeof(st->title)) tl = sizeof(st->title) - 1;
    memcpy(st->title, payload + 15, tl);
    st->title[tl] = 0;
    if (!st->fps) st->fps = 15;
    if (st->rate < 8000 || st->rate > 48000) st->rate = 32000;
    if (st->channels < 1 || st->channels > 2) st->channels = 1;

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    st->thread = threadCreate(net_thread, st, NET_STACK, prio - 1, -2, false);
    if (!st->thread) {
        snprintf(err, errsz, "Could not start the network thread.");
        conn_close(&st->conn);
        return -1;
    }
    return 0;
}

static void stream_close(Stream *st)
{
    st->stop = true;
    conn_abort(&st->conn);
    if (st->thread) {
        threadJoin(st->thread, U64_MAX);
        threadFree(st->thread);
        st->thread = NULL;
    }
    conn_close(&st->conn);
    q_clear(&st->vq);
    q_clear(&st->aq);
}

/* ----------------------------------------------------------------- audio */

static ndspWaveBuf wb[NUM_WB];
static u8 *wb_mem;
static int wb_head, wb_count;
static u64 done_samples;
static u32 audio_rate;
static u8 audio_channels;

static bool audio_open(u32 rate, u8 channels)
{
    if (!wb_mem) wb_mem = (u8 *)linearAlloc(NUM_WB * WB_BYTES);
    if (!wb_mem) return false;
    audio_rate = rate;
    audio_channels = channels;
    ndspChnReset(AUDIO_CH);
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnSetInterp(AUDIO_CH, NDSP_INTERP_LINEAR);
    ndspChnSetRate(AUDIO_CH, (float)rate);
    ndspChnSetFormat(AUDIO_CH, channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    float mix[12] = { 1.0f, 1.0f };
    ndspChnSetMix(AUDIO_CH, mix);
    memset(wb, 0, sizeof(wb));
    for (int i = 0; i < NUM_WB; i++) wb[i].data_vaddr = wb_mem + i * WB_BYTES;
    wb_head = wb_count = 0;
    done_samples = 0;
    return true;
}

static void audio_reset(void)
{
    ndspChnWaveBufClear(AUDIO_CH);
    ndspChnSetPaused(AUDIO_CH, false);
    for (int i = 0; i < NUM_WB; i++) wb[i].status = NDSP_WBUF_FREE;
    wb_head = wb_count = 0;
    done_samples = 0;
}

static void audio_close(void)
{
    ndspChnWaveBufClear(AUDIO_CH);
    ndspChnReset(AUDIO_CH);
    if (wb_mem) { linearFree(wb_mem); wb_mem = NULL; }
}

/* Counts the buffers the DSP has finished playing. */
static void audio_reap(void)
{
    while (wb_count > 0 && wb[wb_head].status == NDSP_WBUF_DONE) {
        done_samples += wb[wb_head].nsamples;
        wb[wb_head].status = NDSP_WBUF_FREE;
        wb_head = (wb_head + 1) % NUM_WB;
        wb_count--;
    }
}

static void audio_feed(Stream *st)
{
    while (wb_count < NUM_WB) {
        Packet p;
        LightLock_Lock(&st->lock);
        if (!st->aq.count) { LightLock_Unlock(&st->lock); break; }
        q_pop(&st->aq, &p);
        LightLock_Unlock(&st->lock);

        u32 len = p.len > WB_BYTES ? WB_BYTES : p.len;
        u32 frame = 2u * audio_channels;
        len -= len % frame;
        if (len) {
            ndspWaveBuf *b = &wb[(wb_head + wb_count) % NUM_WB];
            memcpy((void *)b->data_vaddr, p.data, len);
            b->nsamples = len / frame;
            b->looping = false;
            DSP_FlushDataCache(b->data_vaddr, len);
            ndspChnWaveBufAdd(AUDIO_CH, b);
            wb_count++;
        }
        free(p.data);
    }
}

static u32 audio_clock_ms(u32 start_ms)
{
    u64 samples = done_samples + (wb_count > 0 ? ndspChnGetSamplePos(AUDIO_CH) : 0);
    return start_ms + (u32)(samples * 1000 / audio_rate);
}

static u32 audio_buffered_ms(Stream *st)
{
    u32 ms = 0;
    LightLock_Lock(&st->lock);
    for (int i = 0; i < st->aq.count; i++) {
        Packet *p = q_at(&st->aq, i);
        ms += (u32)((u64)p->len * 1000 / (2u * st->channels * st->rate));
    }
    LightLock_Unlock(&st->lock);
    for (int i = 0; i < wb_count; i++) ms += wb[(wb_head + i) % NUM_WB].nsamples * 1000 / audio_rate;
    return ms;
}

/* ----------------------------------------------------------------- video */

typedef struct { struct jpeg_error_mgr pub; jmp_buf jb; } JErr;

static void jerr_exit(j_common_ptr ci) { longjmp(((JErr *)ci->err)->jb, 1); }
static void jerr_silent(j_common_ptr ci) { (void)ci; }

static struct jpeg_decompress_struct dinfo;
static JErr jerr;
static u16 *rowbuf;   /* 16 rows of up to 1024 pixels */

static void video_open(void)
{
    dinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jerr_exit;
    jerr.pub.output_message = jerr_silent;
    jpeg_create_decompress(&dinfo);
    rowbuf = (u16 *)malloc(16 * 1024 * sizeof(u16));

    gfxSetDoubleBuffering(GFX_TOP, true);
    for (int i = 0; i < 2; i++) {
        memset(gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL), 0, SCREEN_W * SCREEN_H * 2);
        gfxFlushBuffers();
        gfxSwapBuffers();
    }
}

static void video_close(void)
{
    jpeg_destroy_decompress(&dinfo);
    free(rowbuf);
    rowbuf = NULL;
}

/* Decodes a JPEG straight into the (rotated, 240x400 column-major) top framebuffer. */
static bool decode_frame(const u8 *data, u32 len)
{
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    if (!rowbuf) return false;
    if (setjmp(jerr.jb)) {
        jpeg_abort_decompress(&dinfo);
        return false;
    }
    jpeg_mem_src(&dinfo, (unsigned char *)data, len);
    jpeg_read_header(&dinfo, TRUE);
    dinfo.out_color_space = JCS_RGB565;
    dinfo.dct_method = JDCT_IFAST;
    dinfo.do_fancy_upsampling = FALSE;
    dinfo.do_block_smoothing = FALSE;
    dinfo.dither_mode = JDITHER_NONE;
    jpeg_start_decompress(&dinfo);

    if (dinfo.output_width > 1024) { jpeg_abort_decompress(&dinfo); return false; }
    int w = dinfo.output_width < SCREEN_W ? (int)dinfo.output_width : SCREEN_W;
    int h = dinfo.output_height < SCREEN_H ? (int)dinfo.output_height : SCREEN_H;
    int xoff = (SCREEN_W - w) / 2, yoff = (SCREEN_H - h) / 2;
    if (w < SCREEN_W || h < SCREEN_H) memset(fb, 0, SCREEN_W * SCREEN_H * 2);

    JSAMPROW rows[16];
    for (int i = 0; i < 16; i++) rows[i] = (JSAMPROW)(rowbuf + i * 1024);
    while (dinfo.output_scanline < dinfo.output_height) {
        int y0 = (int)dinfo.output_scanline;
        int n = (int)jpeg_read_scanlines(&dinfo, rows, 16);
        for (int i = 0; i < n; i++) {
            int y = y0 + i;
            if (y >= h) continue;
            const u16 *src = (const u16 *)rows[i];
            u16 *dst = fb + xoff * SCREEN_H + (SCREEN_H - 1 - (y + yoff));
            for (int x = 0; x < w; x++, dst += SCREEN_H) *dst = src[x];
        }
    }
    jpeg_finish_decompress(&dinfo);
    return true;
}

/* ---------------------------------------------------------------- status */

static void fmt_time(char *out, size_t n, u32 sec)
{
    if (sec >= 3600) snprintf(out, n, "%lu:%02lu:%02lu", (unsigned long)(sec / 3600),
                              (unsigned long)(sec / 60 % 60), (unsigned long)(sec % 60));
    else snprintf(out, n, "%02lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
}

enum { ST_BUFFERING, ST_PLAYING };

static void draw_status(Stream *st, u32 clock_ms, int state, bool paused, int pending_seek_ms,
                        u32 buffered_ms, u32 kbps, u32 dropped, bool audio_ok)
{
    char a[16], b[16];
    printf("\x1b[1;1H\x1b[36mTube3DS\x1b[0m - now playing\n\n");
    char t[81];
    snprintf(t, sizeof(t), "%-80.80s", st->title);
    printf("%.40s%.40s", t, t + 40);

    fmt_time(a, sizeof(a), (pending_seek_ms >= 0 ? (u32)pending_seek_ms : clock_ms) / 1000);
    fmt_time(b, sizeof(b), st->duration);
    const char *tag = pending_seek_ms >= 0 ? "\x1b[33m[SEEK]     " :
                      paused ? "\x1b[33m[PAUSED]   " :
                      state == ST_BUFFERING ? "\x1b[33m[LOADING]  " : "\x1b[32m[PLAYING]  ";
    printf("\x1b[6;1H %s / %s  %s\x1b[0m\n", a, st->duration ? b : "live", tag);

    char bar[39];
    int filled = 0;
    if (st->duration) {
        u32 pos = pending_seek_ms >= 0 ? (u32)pending_seek_ms : clock_ms;
        filled = (int)((u64)pos * 38 / ((u64)st->duration * 1000));
        if (filled > 38) filled = 38;
    }
    for (int i = 0; i < 38; i++) bar[i] = i < filled ? '=' : '-';
    bar[38] = 0;
    printf("\x1b[7;1H %s\n", bar);
    printf("\x1b[9;1H Buffer %4lu.%lus   Net %4lu KB/s   \n", (unsigned long)(buffered_ms / 1000),
           (unsigned long)(buffered_ms / 100 % 10), (unsigned long)kbps);
    printf(" Dropped frames: %-6lu            \n", (unsigned long)dropped);
    if (!audio_ok) printf("\x1b[31m No sound: DSP firmware missing\x1b[0m\n");
}

static void draw_help(void)
{
    printf("\x1b[19;1H\x1b[36m Controls\x1b[0m\n");
    printf("  A        pause / resume\n");
    printf("  Left/Right  seek 10 seconds\n");
    printf("  L / R    seek 1 minute\n");
    printf("  X        bottom screen light on/off\n");
    printf("  B        back to the list\n");
}

/* ------------------------------------------------------------------ main */

int play_video(const Settings *s, const char *video_id, PrintConsole *top, PrintConsole *bottom,
               bool audio_ok, char *err, size_t errsz)
{
    static Stream st;  /* large: keep it off the stack */
    err[0] = 0;

    consoleSelect(bottom);
    consoleClear();
    printf("\x1b[1;1HConnecting to server...\n\nThe server is fetching the video\nfrom YouTube. This can take a few\nseconds.\n");

    if (stream_open(&st, s, video_id, 0, err, errsz) < 0) return -1;
    if (audio_ok && !audio_open(st.rate, st.channels)) audio_ok = false;
    video_open();

    consoleSelect(bottom);
    consoleClear();
    draw_help();

    int state = ST_BUFFERING;
    bool paused = false, shown_first = false, backlight = true;
    u32 clock_ms = st.start_ms, dropped = 0, kbps = 0, last_bytes = 0;
    int pending_seek_ms = -1;
    u64 seek_time = 0, last_time = osGetTime(), rate_time = last_time;
    int frame_no = 0, result = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u64 now = osGetTime();
        u32 dt = (u32)(now - last_time);
        last_time = now;

        if (kDown & KEY_B) break;
        if (kDown & KEY_A) {
            paused = !paused;
            if (audio_ok) ndspChnSetPaused(AUDIO_CH, paused);
        }
        if (kDown & KEY_X) {
            backlight = !backlight;
            if (backlight) GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
            else GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM);
        }
        int delta = 0;
        if (kDown & KEY_LEFT)  delta -= 10000;
        if (kDown & KEY_RIGHT) delta += 10000;
        if (kDown & KEY_L)     delta -= 60000;
        if (kDown & KEY_R)     delta += 60000;
        if (delta && st.duration) {
            int target = (pending_seek_ms >= 0 ? pending_seek_ms : (int)clock_ms) + delta;
            int maxms = (int)st.duration * 1000 - 3000;
            if (target > maxms) target = maxms;
            if (target < 0) target = 0;
            pending_seek_ms = target;
            seek_time = now;
        }

        /* Restart the stream at the new position once the user stops pressing. */
        if (pending_seek_ms >= 0 && now - seek_time > SEEK_DELAY_MS) {
            stream_close(&st);
            if (audio_ok) audio_reset();
            if (stream_open(&st, s, video_id, (u32)pending_seek_ms / 1000, err, errsz) < 0) {
                result = -1;
                break;
            }
            clock_ms = st.start_ms;
            state = ST_BUFFERING;
            shown_first = false;
            pending_seek_ms = -1;
            paused = false;
            last_bytes = 0;
        }

        if (st.net_error && st.eof && !st.vq.count && !st.aq.count && (!audio_ok || !wb_count)) {
            snprintf(err, errsz, "Lost the connection to the server.");
            result = -1;
            break;
        }

        /* --- audio + clock --- */
        if (audio_ok) audio_reap();
        u32 buffered = audio_ok ? audio_buffered_ms(&st) : (u32)st.vq.count * 1000 / st.fps;
        if (state == ST_BUFFERING && (st.eof || buffered >= PREBUFFER_MS)) {
            state = ST_PLAYING;
            last_time = now;
            dt = 0;
        }
        if (state == ST_PLAYING) {
            if (audio_ok) audio_feed(&st);
            bool audio_empty = !audio_ok || (wb_count == 0 && st.aq.count == 0);
            if (st.eof && audio_empty && st.vq.count == 0) break;       /* finished */
            if (!st.eof && audio_ok && audio_empty) state = ST_BUFFERING; /* ran dry */
            if (!st.eof && !audio_ok && st.vq.count == 0) state = ST_BUFFERING;
        }
        if (state == ST_PLAYING && !paused) {
            if (audio_ok && !(st.eof && wb_count == 0 && st.aq.count == 0)) {
                u32 ac = audio_clock_ms(st.start_ms);
                if (ac > clock_ms) clock_ms = ac;
            } else {
                clock_ms += dt;   /* no audio driving the clock */
            }
        }

        /* --- video --- */
        Packet frame = { 0 };
        LightLock_Lock(&st.lock);
        if (!shown_first) {
            if (st.vq.count) { q_pop(&st.vq, &frame); shown_first = true; }
        } else if (state == ST_PLAYING) {
            while (st.vq.count >= 2 && q_at(&st.vq, 1)->pts <= clock_ms) {
                Packet old;
                q_pop(&st.vq, &old);
                free(old.data);
                dropped++;
            }
            Packet *f = q_front(&st.vq);
            if (f && f->pts <= clock_ms) q_pop(&st.vq, &frame);
        }
        LightLock_Unlock(&st.lock);

        bool drew = false;
        if (frame.data) {
            drew = decode_frame(frame.data, frame.len);
            free(frame.data);
        }

        /* --- status --- */
        if (now - rate_time >= 1000) {
            u32 b = st.bytes;
            kbps = (u32)((u64)(b - last_bytes) * 1000 / 1024 / (now - rate_time));
            last_bytes = b;
            rate_time = now;
        }
        if (backlight && (frame_no++ % 15) == 0) {
            consoleSelect(bottom);
            draw_status(&st, clock_ms, state, paused, pending_seek_ms, buffered, kbps, dropped, audio_ok);
        }

        if (drew) {
            gfxFlushBuffers();
            gfxSwapBuffers();
        }
        gspWaitForVBlank();
    }

    stream_close(&st);
    if (audio_ok) audio_close();
    video_close();
    if (!backlight) GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
    consoleInit(GFX_TOP, top);
    consoleSelect(bottom);
    consoleClear();
    return result;
}
