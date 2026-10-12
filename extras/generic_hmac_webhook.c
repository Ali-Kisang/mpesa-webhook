// webhook.c - minimal payment webhook handler (starter)
//
// Build:  gcc -O2 -Wall -o webhook webhook.c -lssl -lcrypto -lsqlite3
// Run:    WEBHOOK_SECRET=mysecret ./webhook
// Test:
//   BODY='{"id":"evt_001","amount":"1500","phone":"254700000000"}'
//   SIG=$(printf '%s' "$BODY" | openssl dgst -sha256 -hmac mysecret | awk '{print $2}')
//   curl -i -X POST localhost:8080/webhook -H "X-Signature: $SIG" -d "$BODY"
//   (send it twice: the second call is accepted but not stored again)

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <sqlite3.h>

#define PORT        8080
#define MAX_EVENTS  128
#define BUF_SIZE    16384

typedef struct {
    int    fd;
    size_t len;
    char   buf[BUF_SIZE];
} Conn;

static sqlite3    *db;
static const char *secret;

/* ---------- helpers ---------- */

static void reply(int fd, const char *status) {
    char out[128];
    int n = snprintf(out, sizeof out,
        "HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
    (void)!write(fd, out, n);
}

/* Naive JSON string/number extractor. Replace with cJSON for production. */
static int json_get(const char *json, const char *key, char *out, size_t outsz) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p == '"') p++;
    size_t i = 0;
    while (*p && *p != '"' && *p != ',' && *p != '}' && i < outsz - 1)
        out[i++] = *p++;
    out[i] = 0;
    return i ? 0 : -1;
}

static int verify_signature(const char *body, size_t body_len, const char *sig_hex) {
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int  mac_len = 0;
    HMAC(EVP_sha256(), secret, (int)strlen(secret),
         (const unsigned char *)body, body_len, mac, &mac_len);

    char hex[EVP_MAX_MD_SIZE * 2 + 1];
    for (unsigned i = 0; i < mac_len; i++) sprintf(hex + i * 2, "%02x", mac[i]);

    if (strlen(sig_hex) != mac_len * 2) return 0;
    return CRYPTO_memcmp(hex, sig_hex, mac_len * 2) == 0;   /* constant time */
}

/* Returns 1 if newly stored, 0 if duplicate, -1 on error. */
static int store_payment(const char *event_id, const char *amount, const char *payload) {
    sqlite3_stmt *st;
    const char *sql =
        "INSERT OR IGNORE INTO payments(event_id, amount, payload) VALUES(?,?,?)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, event_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, amount,   -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, payload,  -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    int changed = sqlite3_changes(db);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? changed : -1;
}

/* ---------- request handling ---------- */

/* Returns 1 when a full request was handled, 0 if more data is needed. */
static int handle_request(Conn *c) {
    c->buf[c->len] = 0;
    char *hdr_end = strstr(c->buf, "\r\n\r\n");
    if (!hdr_end) return 0;

    char *body = hdr_end + 4;
    size_t content_len = 0;
    char *cl = strcasestr(c->buf, "Content-Length:");
    if (cl && cl < body) content_len = (size_t)atol(cl + 15);

    size_t have = c->len - (size_t)(body - c->buf);
    if (have < content_len) return 0;                 /* wait for rest of body */
    body[content_len] = 0;

    if (strncmp(c->buf, "POST /webhook", 13) != 0) { reply(c->fd, "404 Not Found"); return 1; }

    char sig[130] = {0};
    char *sh = strcasestr(c->buf, "X-Signature:");
    if (!sh || sh > body) { reply(c->fd, "401 Unauthorized"); return 1; }
    sh += 12;
    while (*sh == ' ') sh++;
    size_t i = 0;
    while (*sh && *sh != '\r' && i < sizeof sig - 1) sig[i++] = *sh++;

    if (!verify_signature(body, content_len, sig)) {
        reply(c->fd, "401 Unauthorized");
        return 1;
    }

    char event_id[64], amount[32];
    if (json_get(body, "id", event_id, sizeof event_id) < 0 ||
        json_get(body, "amount", amount, sizeof amount) < 0) {
        reply(c->fd, "400 Bad Request");
        return 1;
    }

    int r = store_payment(event_id, amount, body);
    if (r < 0) { reply(c->fd, "500 Internal Server Error"); return 1; }

    /* Always 200 on duplicates so the provider stops retrying. */
    reply(c->fd, "200 OK");
    fprintf(stderr, "%s event=%s amount=%s\n", r ? "stored" : "duplicate", event_id, amount);
    return 1;
}

static void close_conn(int ep, Conn *c) {
    epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    free(c);
}

/* ---------- main ---------- */

static void set_nonblock(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

int main(void) {
    secret = getenv("WEBHOOK_SECRET");
    if (!secret || !*secret) { fprintf(stderr, "Set WEBHOOK_SECRET\n"); return 1; }
    signal(SIGPIPE, SIG_IGN);

    if (sqlite3_open("payments.db", &db) != SQLITE_OK) { fprintf(stderr, "db open failed\n"); return 1; }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS payments("
        " event_id TEXT PRIMARY KEY, amount TEXT, payload TEXT,"
        " received_at TEXT DEFAULT CURRENT_TIMESTAMP);", NULL, NULL, NULL);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = { .sin_family = AF_INET,
                                .sin_port = htons(PORT),
                                .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    listen(lfd, 512);
    set_nonblock(lfd);

    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = NULL };  /* NULL = listener */
    epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev);

    fprintf(stderr, "listening on :%d\n", PORT);
    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int n = epoll_wait(ep, events, MAX_EVENTS, -1);
        for (int i = 0; i < n; i++) {
            Conn *c = events[i].data.ptr;

            if (c == NULL) {                              /* new connections */
                for (;;) {
                    int fd = accept(lfd, NULL, NULL);
                    if (fd < 0) break;
                    set_nonblock(fd);
                    Conn *nc = calloc(1, sizeof *nc);
                    nc->fd = fd;
                    struct epoll_event cev = { .events = EPOLLIN | EPOLLRDHUP, .data.ptr = nc };
                    epoll_ctl(ep, EPOLL_CTL_ADD, fd, &cev);
                }
                continue;
            }

            int done = 0;
            for (;;) {                                    /* read until EAGAIN */
                ssize_t r = read(c->fd, c->buf + c->len, BUF_SIZE - 1 - c->len);
                if (r > 0) { c->len += (size_t)r; continue; }
                if (r == 0) { done = 1; }
                else if (errno != EAGAIN && errno != EWOULDBLOCK) { done = 1; }
                break;
            }
            if (c->len >= BUF_SIZE - 1) { reply(c->fd, "413 Payload Too Large"); done = 1; }
            else if (c->len > 0 && handle_request(c)) done = 1;

            if (done || (events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)))
                close_conn(ep, c);
        }
    }
}
