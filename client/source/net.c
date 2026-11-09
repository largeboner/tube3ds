#include "net.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000
#define CONNECT_TIMEOUT_MS 6000

static u32 *soc_buffer;

int net_init(void)
{
    soc_buffer = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!soc_buffer) return -1;
    Result r = socInit(soc_buffer, SOC_BUFFERSIZE);
    if (R_FAILED(r)) {
        free(soc_buffer);
        soc_buffer = NULL;
        return -2;
    }
    return 0;
}

void net_exit(void)
{
    if (soc_buffer) {
        socExit();
        free(soc_buffer);
        soc_buffer = NULL;
    }
}

int parse_server(const char *s, char *host, size_t hostsz, int *port)
{
    while (*s == ' ') s++;
    if (!strncmp(s, "http://", 7)) s += 7;
    const char *colon = strrchr(s, ':');
    size_t n = colon ? (size_t)(colon - s) : strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '/')) n--;
    if (n == 0 || n >= hostsz) return -1;
    memcpy(host, s, n);
    host[n] = 0;
    *port = colon ? atoi(colon + 1) : 8080;
    if (*port <= 0 || *port > 65535) return -1;
    return 0;
}

static int connect_with_timeout(int fd, struct sockaddr_in *addr)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int r = connect(fd, (struct sockaddr *)addr, sizeof(*addr));
    if (r < 0 && (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EALREADY)) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        r = poll(&pfd, 1, CONNECT_TIMEOUT_MS);
        /* Writable without error flags = connected. A failed connection that slips
           through here is caught by the send() that follows. */
        r = (r > 0 && !(pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) ? 0 : -1;
    }
    if (flags >= 0) fcntl(fd, F_SETFL, flags);
    return r;
}

static int send_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, data, len, 0);
        if (n <= 0) return -1;
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

static int fill(Conn *c)
{
    if (c->pos < c->len) return 1;
    ssize_t n = recv(c->fd, c->buf, CONN_BUF_SIZE, 0);
    if (n <= 0) return (int)n;
    c->pos = 0;
    c->len = (size_t)n;
    return 1;
}

int conn_read_full(Conn *c, void *dst, size_t len)
{
    uint8_t *out = (uint8_t *)dst;
    size_t got = 0;
    while (got < len) {
        int r = fill(c);
        if (r <= 0) return got ? -1 : r;
        size_t take = c->len - c->pos;
        if (take > len - got) take = len - got;
        memcpy(out + got, c->buf + c->pos, take);
        c->pos += take;
        got += take;
    }
    return (int)got;
}

int http_open(Conn *c, const char *host, int port, const char *path)
{
    memset(c, 0, sizeof(*c));
    c->fd = -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u16)port);
    if (!inet_aton(host, &addr.sin_addr)) {
        struct hostent *he = gethostbyname(host);
        if (!he || !he->h_addr_list[0]) return -10;
        memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
    }

    c->buf = (uint8_t *)malloc(CONN_BUF_SIZE);
    if (!c->buf) return -11;

    c->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (c->fd < 0) { conn_close(c); return -12; }

    int rcvbuf = 256 * 1024;
    setsockopt(c->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    if (connect_with_timeout(c->fd, &addr) < 0) { conn_close(c); return -13; }

    char req[1024];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: Tube3DS\r\n\r\n",
                     path, host, port);
    if (n <= 0 || n >= (int)sizeof(req) || send_all(c->fd, req, (size_t)n) < 0) {
        conn_close(c);
        return -14;
    }

    /* Read the header block byte by byte out of our buffer. */
    char line[512];
    size_t ll = 0;
    int first = 1;
    for (;;) {
        char ch;
        if (conn_read_full(c, &ch, 1) != 1) { conn_close(c); return -15; }
        if (ch == '\r') continue;
        if (ch != '\n') {
            if (ll < sizeof(line) - 1) line[ll++] = ch;
            continue;
        }
        line[ll] = 0;
        if (first) {
            const char *sp = strchr(line, ' ');
            c->status = sp ? atoi(sp + 1) : 0;
            first = 0;
        } else if (ll == 0) {
            break; /* end of headers */
        }
        ll = 0;
    }
    return 0;
}

void conn_abort(Conn *c)
{
    if (c->fd >= 0) shutdown(c->fd, SHUT_RDWR);
}

void conn_close(Conn *c)
{
    if (c->fd >= 0) {
        shutdown(c->fd, SHUT_RDWR);
        closesocket(c->fd);
        c->fd = -1;
    }
    free(c->buf);
    c->buf = NULL;
}

int http_get_text(const char *host, int port, const char *path, char **out, size_t max_len)
{
    *out = NULL;
    Conn c;
    int r = http_open(&c, host, port, path);
    if (r < 0) return r;

    size_t cap = 4096, len = 0;
    char *text = (char *)malloc(cap + 1);
    if (!text) { conn_close(&c); return -11; }
    for (;;) {
        int f = fill(&c);
        if (f <= 0) break;
        size_t take = c.len - c.pos;
        if (len + take > max_len) take = max_len - len;
        if (len + take > cap) {
            while (len + take > cap) cap *= 2;
            char *nt = (char *)realloc(text, cap + 1);
            if (!nt) break;
            text = nt;
        }
        memcpy(text + len, c.buf + c.pos, take);
        len += take;
        c.pos = c.len;
        if (len >= max_len) break;
    }
    text[len] = 0;
    int status = c.status;
    conn_close(&c);
    *out = text;
    return status;
}

void url_encode(const char *in, char *out, size_t outsz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < outsz; p++) {
        unsigned char ch = *p;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out[o++] = (char)ch;
        } else if (ch == ' ') {
            out[o++] = '+';
        } else {
            out[o++] = '%';
            out[o++] = hex[ch >> 4];
            out[o++] = hex[ch & 15];
        }
    }
    out[o] = 0;
}
