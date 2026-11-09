#pragma once
#include <3ds.h>
#include <stddef.h>

typedef struct {
    char host[128];
    int  port;
    int  fps;        /* video frames per second requested from the server */
    int  quality;    /* JPEG quality, 2 (best) .. 31 (worst) */
    int  rate;       /* audio sample rate */
    int  channels;   /* 1 = mono, 2 = stereo */
} Settings;

/* Plays one video until the user backs out or it ends.
   Returns 0 normally, negative with a message in err on failure. */
int play_video(const Settings *s, const char *video_id, PrintConsole *top, PrintConsole *bottom,
               bool audio_ok, char *err, size_t errsz);
