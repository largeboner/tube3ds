#pragma once
#include <stddef.h>
#include <stdint.h>

#define CONN_BUF_SIZE (64 * 1024)

typedef struct {
    int fd;
    int status;                 /* HTTP status code */
    uint8_t *buf;
    size_t pos, len;
} Conn;

/* Socket service setup; returns 0 on success. */
int  net_init(void);
void net_exit(void);

/* Opens an HTTP/1.0 GET and reads the response headers.
   Returns 0 on success (check c->status), negative on network failure. */
int  http_open(Conn *c, const char *host, int port, const char *path);
/* Reads exactly len bytes. Returns len, 0 on clean EOF, negative on error. */
int  conn_read_full(Conn *c, void *dst, size_t len);
/* Interrupt a blocking read from another thread. */
void conn_abort(Conn *c);
void conn_close(Conn *c);

/* Fetches a whole (small) response body as a NUL-terminated string.
   *out must be freed by the caller. Returns HTTP status or negative error. */
int  http_get_text(const char *host, int port, const char *path, char **out, size_t max_len);

/* Percent-encodes a string for use in a query string. */
void url_encode(const char *in, char *out, size_t outsz);

/* Splits "host:port" (port defaults to 8080). Returns 0 on success. */
int  parse_server(const char *s, char *host, size_t hostsz, int *port);
