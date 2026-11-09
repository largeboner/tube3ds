/*
 * Tube3DS - watch YouTube on a homebrew-enabled Nintendo 3DS.
 * Works together with tube3ds_server.py running on a computer on the same Wi-Fi.
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "net.h"
#include "player.h"

#define APP_VERSION     "1.0"
#define CONFIG_DIR      "sdmc:/3ds/tube3ds"
#define CONFIG_PATH     CONFIG_DIR "/config.txt"
#define MAX_RESULTS     50
#define PAGE            13      /* results visible on the top screen */

typedef struct {
    char id[16];
    int  duration;
    char title[152];
    char channel[64];
} Video;

static PrintConsole top, bottom;
static Settings cfg;
static Video results[MAX_RESULTS];
static int nresults, sel, scroll;
static char query[256];
static bool server_ok, audio_ok;
static char message[320];

/* ---------------------------------------------------------------- config */

static void load_config(bool is_new3ds)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 8080;
    cfg.fps = is_new3ds ? 15 : 10;
    cfg.quality = is_new3ds ? 7 : 9;
    cfg.rate = is_new3ds ? 32000 : 22050;
    cfg.channels = 1;

    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "server")) parse_server(v, cfg.host, sizeof(cfg.host), &cfg.port);
        else if (!strcmp(k, "fps")) cfg.fps = atoi(v);
        else if (!strcmp(k, "quality")) cfg.quality = atoi(v);
        else if (!strcmp(k, "audio_rate")) cfg.rate = atoi(v);
        else if (!strcmp(k, "stereo")) cfg.channels = atoi(v) ? 2 : 1;
    }
    fclose(f);
    if (cfg.fps < 1 || cfg.fps > 30) cfg.fps = 12;
    if (cfg.quality < 2 || cfg.quality > 31) cfg.quality = 8;
    if (cfg.rate < 8000 || cfg.rate > 48000) cfg.rate = 32000;
}

static void save_config(void)
{
    mkdir("sdmc:/3ds", 0777);
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "# Tube3DS settings\n");
    fprintf(f, "server=%s:%d\n", cfg.host, cfg.port);
    fprintf(f, "# Frames per second (1-30). Lower it if video stutters.\n");
    fprintf(f, "fps=%d\n", cfg.fps);
    fprintf(f, "# JPEG quality: 2 = best/biggest, 31 = worst/smallest.\n");
    fprintf(f, "quality=%d\n", cfg.quality);
    fprintf(f, "audio_rate=%d\n", cfg.rate);
    fprintf(f, "stereo=%d\n", cfg.channels == 2);
    fclose(f);
}

/* -------------------------------------------------------------------- UI */

static bool keyboard(const char *hint, const char *initial, char *out, size_t outsz)
{
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, (int)outsz - 1);
    swkbdSetHintText(&kb, hint);
    if (initial && *initial) swkbdSetInitialText(&kb, initial);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    return swkbdInputText(&kb, out, outsz) == SWKBD_BUTTON_CONFIRM;
}

static void fmt_duration(char *out, size_t n, int sec)
{
    if (sec <= 0) out[0] = 0;
    else if (sec >= 3600) snprintf(out, n, "%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
    else snprintf(out, n, "%d:%02d", sec / 60, sec % 60);
}

static void draw_top(void)
{
    consoleSelect(&top);
    consoleClear();
    if (query[0]) printf("\x1b[1;1H\x1b[36m Tube3DS\x1b[0m  results for \"%.28s\"", query);
    else printf("\x1b[1;1H\x1b[36m Tube3DS\x1b[0m  v" APP_VERSION);

    if (nresults == 0) {
        printf("\x1b[12;1H");
        printf("        Press \x1b[33mY\x1b[0m to search YouTube.\n\n");
        printf("   You can also type a YouTube link or\n");
        printf("   playlist link instead of search words.\n");
        return;
    }
    for (int i = 0; i < PAGE && scroll + i < nresults; i++) {
        int idx = scroll + i;
        Video *v = &results[idx];
        char dur[16];
        fmt_duration(dur, sizeof(dur), v->duration);
        bool s = idx == sel;
        printf("\x1b[%d;1H%s %-48.48s\x1b[0m", 3 + i * 2, s ? "\x1b[7m" : "", v->title);
        printf("\x1b[%d;1H%s   %-36.36s %9s \x1b[0m", 4 + i * 2, s ? "\x1b[7m" : "\x1b[36m",
               v->channel, dur);
    }
    printf("\x1b[30;1H %d of %d", sel + 1, nresults);
}

static void draw_bottom(void)
{
    consoleSelect(&bottom);
    consoleClear();
    printf("\x1b[1;1H\x1b[36mTube3DS\x1b[0m - YouTube for your 3DS\n");
    printf("Server: %.24s:%d %s\x1b[0m\n", cfg.host[0] ? cfg.host : "(not set)", cfg.port,
           server_ok ? "\x1b[32m[OK]" : "\x1b[31m[offline]");
    if (!audio_ok) printf("\x1b[31mNo sound: run DSP1 to dump DSP firmware\x1b[0m\n");

    if (nresults > 0) {
        Video *v = &results[sel];
        char dur[16];
        fmt_duration(dur, sizeof(dur), v->duration);
        printf("\x1b[5;1H\x1b[33m%.120s\x1b[0m\n", v->title);
        printf("\x1b[9;1H%.38s\n%s\n", v->channel, dur);
    }

    printf("\x1b[13;1H");
    printf(" Y       search\n");
    printf(" A       play selected video\n");
    printf(" Up/Down choose      L/R  page\n");
    printf(" SELECT  change server address\n");
    printf(" START   quit\n");

    if (message[0]) printf("\x1b[21;1H\x1b[31m%.360s\x1b[0m", message);
}

static void redraw(void)
{
    draw_top();
    draw_bottom();
}

static void status(const char *text)
{
    consoleSelect(&bottom);
    printf("\x1b[21;1H\x1b[0J\x1b[33m%s\x1b[0m", text);
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

/* --------------------------------------------------------------- network */

static bool test_server(void)
{
    if (!cfg.host[0]) return false;
    char *text = NULL;
    int st = http_get_text(cfg.host, cfg.port, "/info", &text, 256);
    bool ok = st == 200 && text && !strncmp(text, "tube3ds", 7);
    free(text);
    return ok;
}

static void ask_server(void)
{
    char current[160] = "", entered[160];
    if (cfg.host[0]) snprintf(current, sizeof(current), "%s:%d", cfg.host, cfg.port);
    if (!keyboard("Server address, e.g. 192.168.1.20:8080", current, entered, sizeof(entered)))
        return;
    char host[128];
    int port;
    if (parse_server(entered, host, sizeof(host), &port) < 0) {
        snprintf(message, sizeof(message), "That address doesn't look right. Use IP:PORT.");
        return;
    }
    strcpy(cfg.host, host);
    cfg.port = port;
    save_config();
    status("Checking server...");
    server_ok = test_server();
    snprintf(message, sizeof(message), server_ok ? "" :
             "Can't reach the server. Is it running, and are both on the same Wi-Fi?");
}

static void parse_results(char *text)
{
    nresults = sel = scroll = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line && nresults < MAX_RESULTS;
         line = strtok_r(NULL, "\n", &save)) {
        char *f[4] = { 0 };
        int nf = 0;
        char *p = line;
        while (nf < 4) {
            f[nf++] = p;
            char *tab = strchr(p, '\t');
            if (!tab) break;
            *tab = 0;
            p = tab + 1;
        }
        if (nf < 3 || strlen(f[0]) != 11) continue;
        Video *v = &results[nresults++];
        snprintf(v->id, sizeof(v->id), "%s", f[0]);
        v->duration = atoi(f[1]);
        snprintf(v->title, sizeof(v->title), "%s", f[2]);
        snprintf(v->channel, sizeof(v->channel), "%s", nf > 3 ? f[3] : "");
    }
}

static void do_search(void)
{
    char q[256];
    if (!keyboard("Search YouTube (or paste a link)", query, q, sizeof(q))) return;
    snprintf(query, sizeof(query), "%s", q);

    status("Searching...");
    char enc[768], path[900];
    url_encode(q, enc, sizeof(enc));
    snprintf(path, sizeof(path), "/search?n=%d&q=%s", MAX_RESULTS, enc);
    char *text = NULL;
    int st = http_get_text(cfg.host, cfg.port, path, &text, 128 * 1024);
    message[0] = 0;
    if (st < 0) {
        server_ok = false;
        snprintf(message, sizeof(message), "Can't reach the server (error %d). Check that it's running and the address is right (SELECT).", st);
    } else if (st != 200) {
        snprintf(message, sizeof(message), "Search failed: %.200s", text ? text : "");
    } else {
        server_ok = true;
        parse_results(text);
        if (!nresults) snprintf(message, sizeof(message), "No results.");
    }
    free(text);
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, &top);
    consoleInit(GFX_BOTTOM, &bottom);

    bool is_new = false;
    APT_CheckNew3DS(&is_new);
    if (is_new) osSetSpeedupEnable(true);
    gspLcdInit();
    audio_ok = R_SUCCEEDED(ndspInit());
    hidSetRepeatParameters(20, 6);

    if (net_init() != 0) {
        consoleSelect(&bottom);
        printf("\x1b[31mCould not start networking.\x1b[0m\n\nPress START to exit.");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
        goto out;
    }

    load_config(is_new);
    if (!cfg.host[0]) {
        consoleSelect(&top);
        printf("\x1b[10;1H  Welcome to Tube3DS!\n\n  Start tube3ds_server.py on your computer.\n"
               "  It prints the address to type in here.\n\n  Press A to continue.");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & (KEY_A | KEY_START)) break;
            gspWaitForVBlank();
        }
        ask_server();
    } else {
        status("Checking server...");
        server_ok = test_server();
        if (!server_ok)
            snprintf(message, sizeof(message), "Server %s:%d isn't answering. Start it, or press SELECT to change the address.", cfg.host, cfg.port);
    }
    redraw();

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown(), kRep = hidKeysDownRepeat();
        bool changed = false;

        if (kDown & KEY_START) break;
        if (kDown & KEY_SELECT) { ask_server(); changed = true; }
        if (kDown & KEY_Y) { do_search(); changed = true; }

        if (nresults > 0) {
            int old = sel;
            if (kRep & KEY_DOWN) sel++;
            if (kRep & KEY_UP) sel--;
            if (kRep & KEY_R) sel += PAGE;
            if (kRep & KEY_L) sel -= PAGE;
            if (sel >= nresults) sel = nresults - 1;
            if (sel < 0) sel = 0;
            if (sel < scroll) scroll = sel;
            if (sel >= scroll + PAGE) scroll = sel - PAGE + 1;
            if (sel != old) changed = true;

            if (kDown & KEY_A) {
                char err[300];
                if (play_video(&cfg, results[sel].id, &top, &bottom, audio_ok, err, sizeof(err)) < 0)
                    snprintf(message, sizeof(message), "%s", err);
                else
                    message[0] = 0;
                changed = true;
            }
        }

        if (changed) redraw();
        gspWaitForVBlank();
    }

out:
    if (audio_ok) ndspExit();
    net_exit();
    gspLcdExit();
    gfxExit();
    return 0;
}
