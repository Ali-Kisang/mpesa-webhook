// mpesa_webhook.c - M-Pesa (Daraja STK Push) callback handler with reconciliation
//                   and a polling fallback for lost callbacks
//
// Build (Debian/Ubuntu: apt install libcjson-dev libsqlite3-dev libssl-dev libcurl4-openssl-dev):
//   gcc -O2 -Wall -o mpesa_webhook mpesa_webhook.c -lcjson -lsqlite3 -lcrypto -lcurl -lpthread -lm
//
// Run:
//   Set these environment variables, then start ./mpesa_webhook:
//     CALLBACK_SECRET, ADMIN_TOKEN
//     DARAJA_ENV (sandbox|production), DARAJA_CONSUMER_KEY, DARAJA_CONSUMER_SECRET,
//     DARAJA_SHORTCODE, DARAJA_PASSKEY
//
// Endpoints:
//   POST /mpesa/callback/<CALLBACK_SECRET>   Daraja posts STK results here
//   POST /order                (X-Admin-Token)  register an expected payment
//   GET  /order/<checkout_id>  (X-Admin-Token)  status of one order and its payment
//   GET  /orders[?status=...]  (X-Admin-Token)  list recent orders, optionally by status
//   GET  /report               (X-Admin-Token)  payment and order totals grouped by status
//
// Fallback: a background thread asks Daraja (STK Push Query) about every order that
// is still pending after a delay, with exponential backoff. If Daraja never answers
// definitively, the order moves to 'needs_review'. A late callback always wins and
// upgrades whatever the poller recorded.
//
// Optional settings: PORT (8080), BIND_ADDR (0.0.0.0), DB_PATH (mpesa.db),
//   POLL_BASE_SECS (30), POLL_MAX (6), POLL_TICK (10), DARAJA_BASE_URL.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <curl/curl.h>
#include <sqlite3.h>
#include <cjson/cJSON.h>

#define DEFAULT_PORT 8080
#define MAX_EVENTS  128
#define BUF_SIZE    16384

typedef struct {
    int    fd;
    size_t len;
    char   buf[BUF_SIZE];
} Conn;

typedef struct {
    char base_url[128], key[128], secret[128], shortcode[32], passkey[256];
    int  base_delay, max_polls, tick;
} Daraja;

/* Each thread opens its own SQLite connection. */
static __thread sqlite3 *db;
static char        callback_path[300];
static const char *admin_token;
static Daraja      dj;

/* ---------- small helpers ---------- */

static void reply(int fd, const char *status, const char *body) {
    char hdr[256];
    size_t bl = body ? strlen(body) : 0;
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n", status, bl);
    (void)!write(fd, hdr, (size_t)n);
    if (bl) (void)!write(fd, body, bl);   /* fine for small bodies; loop for large ones */
}

static int secure_eq(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    return la == lb && CRYPTO_memcmp(a, b, la) == 0;
}

static int get_header(const char *req, const char *hdr_end,
                      const char *name, char *out, size_t outsz) {
    char pat[64];
    snprintf(pat, sizeof pat, "\r\n%s:", name);
    const char *p = strcasestr(req, pat);
    if (!p || p >= hdr_end) return -1;
    p += strlen(pat);
    while (*p == ' ') p++;
    size_t i = 0;
    while (*p && *p != '\r' && i < outsz - 1) out[i++] = *p++;
    out[i] = 0;
    return 0;
}

static const char *jstr(const cJSON *o, const char *key) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(i) ? i->valuestring : NULL;
}

/* Daraja sends ResultCode as a number in callbacks but a string in query responses. */
static int jint_flex(const cJSON *o, const char *key, int *out) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(i)) { *out = i->valueint; return 0; }
    if (cJSON_IsString(i) && i->valuestring[0]) { *out = atoi(i->valuestring); return 0; }
    return -1;
}

static int is_admin(const char *req, const char *hdr_end) {
    char tok[128];
    return get_header(req, hdr_end, "X-Admin-Token", tok, sizeof tok) == 0 &&
           secure_eq(tok, admin_token);
}

/* ---------- database ---------- */

static sqlite3 *open_db(void) {
    sqlite3 *h;
    const char *path = getenv("DB_PATH");
    if (sqlite3_open(path && *path ? path : "mpesa.db", &h) != SQLITE_OK) return NULL;
    sqlite3_busy_timeout(h, 5000);
    sqlite3_exec(h, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    return h;
}

static void init_schema(void) {
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS orders("
        " checkout_id TEXT PRIMARY KEY, order_ref TEXT, expected_cents INTEGER,"
        " status TEXT DEFAULT 'pending', created_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        " polls INTEGER DEFAULT 0, last_poll INTEGER DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS payments("
        " dedupe_key TEXT PRIMARY KEY, checkout_id TEXT, receipt TEXT,"
        " amount_cents INTEGER, phone TEXT, result_code INTEGER, status TEXT,"
        " payload TEXT, received_at TEXT DEFAULT CURRENT_TIMESTAMP,"
        " source TEXT DEFAULT 'callback');"
        "CREATE INDEX IF NOT EXISTS idx_pay_checkout ON payments(checkout_id);",
        NULL, NULL, NULL);
    /* Migrate databases created by the previous version; errors (column exists) are fine. */
    sqlite3_exec(db, "ALTER TABLE orders ADD COLUMN polls INTEGER DEFAULT 0", NULL, NULL, NULL);
    sqlite3_exec(db, "ALTER TABLE orders ADD COLUMN last_poll INTEGER DEFAULT 0", NULL, NULL, NULL);
    sqlite3_exec(db, "ALTER TABLE payments ADD COLUMN source TEXT DEFAULT 'callback'", NULL, NULL, NULL);
}

static void exec1(const char *sql, const char *arg) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) {
        fprintf(stderr, "sql error: %s\n", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_text(s, 1, arg, -1, SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

/* Match successful payments for a checkout ID against its order.
 * Safe to call repeatedly; handles callbacks that arrive before the order,
 * and upgrades orders the poller marked paid_unverified once a callback brings the amount. */
static void reconcile(const char *checkout) {
    /* Callback-sourced payments carry an amount: compare it. */
    exec1("UPDATE payments SET status = CASE WHEN amount_cents = "
          "(SELECT expected_cents FROM orders WHERE checkout_id = payments.checkout_id) "
          "THEN 'matched' ELSE 'mismatch' END "
          "WHERE checkout_id = ?1 AND result_code = 0 AND source = 'callback' "
          "AND status IN ('pending','unmatched') "
          "AND EXISTS (SELECT 1 FROM orders WHERE checkout_id = ?1)", checkout);

    /* Poll-sourced payments confirm success but carry no amount. */
    exec1("UPDATE payments SET status = 'confirmed_unverified' "
          "WHERE checkout_id = ?1 AND result_code = 0 AND source = 'query' "
          "AND status IN ('pending','unmatched') "
          "AND EXISTS (SELECT 1 FROM orders WHERE checkout_id = ?1)", checkout);

    exec1("UPDATE orders SET status = (SELECT CASE p.status "
          "WHEN 'matched' THEN 'paid' WHEN 'confirmed_unverified' THEN 'paid_unverified' "
          "ELSE 'amount_mismatch' END FROM payments p "
          "WHERE p.checkout_id = orders.checkout_id AND p.result_code = 0 "
          "ORDER BY p.rowid DESC LIMIT 1) "
          "WHERE checkout_id = ?1 "
          "AND status IN ('pending','needs_review','paid_unverified','failed') "
          "AND EXISTS (SELECT 1 FROM payments p WHERE p.checkout_id = orders.checkout_id "
          "AND p.result_code = 0 AND p.status IN ('matched','mismatch','confirmed_unverified'))",
          checkout);

    /* Successful payments with no registered order yet. */
    exec1("UPDATE payments SET status = 'unmatched' "
          "WHERE checkout_id = ?1 AND result_code = 0 AND status = 'pending'", checkout);
}

static int success_exists(const char *checkout) {
    sqlite3_stmt *s;
    int found = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM payments WHERE checkout_id = ? AND result_code = 0",
                           -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, checkout, -1, SQLITE_STATIC);
        found = sqlite3_step(s) == SQLITE_ROW;
        sqlite3_finalize(s);
    }
    return found;
}

/* If the poller already recorded a success for this checkout, fill in the callback's
 * receipt, amount and phone instead of inserting a second row. Returns 1 if upgraded. */
static int upgrade_query_row(const char *key, const char *checkout, const char *receipt,
                             long long cents, const char *phone, const char *payload) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
        "UPDATE payments SET dedupe_key=?, receipt=?, amount_cents=?, phone=?, payload=?, "
        "source='callback', status='pending' "
        "WHERE checkout_id=? AND source='query' AND result_code=0", -1, &s, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text (s, 1, key,      -1, SQLITE_STATIC);
    sqlite3_bind_text (s, 2, receipt,  -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 3, cents);
    sqlite3_bind_text (s, 4, phone,    -1, SQLITE_STATIC);
    sqlite3_bind_text (s, 5, payload,  -1, SQLITE_STATIC);
    sqlite3_bind_text (s, 6, checkout, -1, SQLITE_STATIC);
    sqlite3_step(s);
    int changed = sqlite3_changes(db);
    sqlite3_finalize(s);
    return changed > 0;
}

/* Returns 1 if newly stored, 0 if duplicate, -1 on error.
 * source is "callback" or "query" (the polling fallback). */
static int record_payment(const char *key, const char *checkout, const char *receipt,
                          long long cents, const char *phone, int rc,
                          const char *payload, const char *source) {
    int from_query = strcmp(source, "query") == 0;
    sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);

    if (rc == 0 && from_query && success_exists(checkout)) {      /* callback got there first */
        sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
        return 0;
    }
    if (rc == 0 && !from_query && upgrade_query_row(key, checkout, receipt, cents, phone, payload)) {
        reconcile(checkout);
        sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
        return 1;
    }

    sqlite3_stmt *s;
    const char *sql =
        "INSERT OR IGNORE INTO payments"
        "(dedupe_key, checkout_id, receipt, amount_cents, phone, result_code, status, payload, source) "
        "VALUES(?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    sqlite3_bind_text (s, 1, key,      -1, SQLITE_STATIC);
    sqlite3_bind_text (s, 2, checkout, -1, SQLITE_STATIC);
    sqlite3_bind_text (s, 3, receipt,  -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 4, cents);
    sqlite3_bind_text (s, 5, phone,    -1, SQLITE_STATIC);
    sqlite3_bind_int  (s, 6, rc);
    sqlite3_bind_text (s, 7, rc == 0 ? "pending" : "failed", -1, SQLITE_STATIC);
    sqlite3_bind_text (s, 8, payload,  -1, SQLITE_STATIC);
    sqlite3_bind_text (s, 9, source,   -1, SQLITE_STATIC);
    int step = sqlite3_step(s);
    int changed = sqlite3_changes(db);
    sqlite3_finalize(s);

    if (step != SQLITE_DONE) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    if (changed) {
        if (rc == 0) reconcile(checkout);
        else exec1("UPDATE orders SET status='failed' "
                   "WHERE checkout_id = ?1 AND status IN ('pending','needs_review')", checkout);
    }
    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    return changed ? 1 : 0;
}

/* ---------- polling fallback (Daraja STK Push Query) ---------- */

typedef struct { char *p; size_t n; } Buf;

static size_t write_cb(void *data, size_t sz, size_t nm, void *ud) {
    Buf *b = ud;
    size_t add = sz * nm;
    if (b->n + add > 65536) return 0;
    char *np = realloc(b->p, b->n + add + 1);
    if (!np) return 0;
    b->p = np;
    memcpy(b->p + b->n, data, add);
    b->n += add;
    b->p[b->n] = 0;
    return add;
}

/* Returns the HTTP status, or -1 on transport failure. Caller frees out->p. */
static long http_json(const char *url, const char *userpwd, const char *bearer,
                      const char *post_body, Buf *out) {
    CURL *h = curl_easy_init();
    if (!h) return -1;
    struct curl_slist *hdrs = NULL;
    char auth[600];

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    if (userpwd) curl_easy_setopt(h, CURLOPT_USERPWD, userpwd);
    if (bearer) {
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", bearer);
        hdrs = curl_slist_append(hdrs, auth);
    }
    if (post_body) {
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, post_body);
    }
    if (hdrs) curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

    long code = -1;
    if (curl_easy_perform(h) == CURLE_OK) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);
    return code;
}

static char   token[512];
static time_t token_exp;

static int get_token(void) {
    if (token[0] && time(NULL) < token_exp - 60) return 0;
    char url[256], up[300];
    snprintf(url, sizeof url, "%s/oauth/v1/generate?grant_type=client_credentials", dj.base_url);
    snprintf(up, sizeof up, "%s:%s", dj.key, dj.secret);

    Buf b = {0};
    long code = http_json(url, up, NULL, NULL, &b);
    int ok = -1;
    if (code == 200 && b.p) {
        cJSON *root = cJSON_Parse(b.p);
        const char *at = jstr(root, "access_token");
        int ttl = 3500;
        jint_flex(root, "expires_in", &ttl);
        if (at && strlen(at) < sizeof token) {
            snprintf(token, sizeof token, "%s", at);
            token_exp = time(NULL) + ttl;
            ok = 0;
        }
        cJSON_Delete(root);
    } else {
        fprintf(stderr, "poller: token request failed (http %ld)\n", code);
    }
    free(b.p);
    return ok;
}

typedef enum { Q_PENDING, Q_SUCCESS, Q_FAILED, Q_ERROR } QResult;

/* Result codes that mean the payment definitively did not happen.
 * Anything else (including unknown codes) is treated as "still pending" so we never
 * mark a payment failed just because we asked too early. Extend from Safaricom's docs. */
static const int terminal_fail[] = { 1, 1001, 1019, 1025, 1032, 1037, 2001 };

static QResult query_stk(const char *checkout, int *code_out) {
    if (get_token() != 0) return Q_ERROR;

    time_t now = time(NULL);
    struct tm tm;
    char ts[16], raw[400], pw[600], url[256];
    now += 3 * 3600;                              /* East Africa Time (UTC+3, no DST) */
    gmtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y%m%d%H%M%S", &tm);
    int n = snprintf(raw, sizeof raw, "%s%s%s", dj.shortcode, dj.passkey, ts);
    EVP_EncodeBlock((unsigned char *)pw, (unsigned char *)raw, n);

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "BusinessShortCode", dj.shortcode);
    cJSON_AddStringToObject(req, "Password", pw);
    cJSON_AddStringToObject(req, "Timestamp", ts);
    cJSON_AddStringToObject(req, "CheckoutRequestID", checkout);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    snprintf(url, sizeof url, "%s/mpesa/stkpushquery/v1/query", dj.base_url);
    Buf b = {0};
    long http = http_json(url, NULL, token, body, &b);
    free(body);

    QResult res = Q_ERROR;
    if (http == 401) token[0] = 0;                /* force a fresh token next time */
    if (http > 0 && b.p) {
        cJSON *root = cJSON_Parse(b.p);
        int rc;
        if (jint_flex(root, "ResultCode", &rc) == 0) {
            *code_out = rc;
            res = Q_PENDING;
            if (rc == 0) res = Q_SUCCESS;
            else for (size_t i = 0; i < sizeof terminal_fail / sizeof *terminal_fail; i++)
                if (terminal_fail[i] == rc) res = Q_FAILED;
        } else {
            /* Daraja answers with this error while the transaction is still in progress. */
            const char *ec = jstr(root, "errorCode");
            if (ec && !strcmp(ec, "500.001.1001")) res = Q_PENDING;
            else fprintf(stderr, "poller: unexpected reply for %s (http %ld): %.200s\n",
                         checkout, http, b.p);
        }
        cJSON_Delete(root);
    } else {
        fprintf(stderr, "poller: query failed for %s (http %ld)\n", checkout, http);
    }
    free(b.p);
    return res;
}

static void bump_poll(const char *checkout, int add) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
        "UPDATE orders SET polls = polls + ?2, last_poll = CAST(strftime('%s','now') AS INTEGER) "
        "WHERE checkout_id = ?1", -1, &s, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(s, 1, checkout, -1, SQLITE_STATIC);
    sqlite3_bind_int (s, 2, add);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

static void poll_cycle(void) {
    typedef struct { char id[96]; } Due;
    Due due[20];
    int nd = 0;

    /* An order is due when time since its last poll (or creation) >= base_delay * 2^polls. */
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
        "SELECT checkout_id FROM orders WHERE status = 'pending' AND polls < ?1 "
        "AND CAST(strftime('%s','now') AS INTEGER) - "
        "    MAX(last_poll, CAST(strftime('%s', created_at) AS INTEGER)) >= (?2 << polls) "
        "LIMIT 20", -1, &s, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(s, 1, dj.max_polls);
    sqlite3_bind_int(s, 2, dj.base_delay);
    while (nd < 20 && sqlite3_step(s) == SQLITE_ROW)
        snprintf(due[nd++].id, sizeof due[0].id, "%s", (const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);

    for (int i = 0; i < nd; i++) {
        const char *id = due[i].id;
        int code = 0;
        QResult r = query_stk(id, &code);
        static const char *payload = "{\"source\":\"stkpushquery\"}";

        if (r == Q_SUCCESS) {
            char key[128];
            snprintf(key, sizeof key, "Q:%.95s", id);
            int rr = record_payment(key, id, "", 0, "", 0, payload, "query");
            fprintf(stderr, "poller: %s confirmed by query (%s)\n", id, rr ? "recorded" : "already known");
        } else if (r == Q_FAILED) {
            record_payment(id, id, "", 0, "", code, payload, "query");
            fprintf(stderr, "poller: %s failed by query (code %d)\n", id, code);
        } else if (r == Q_PENDING) {
            bump_poll(id, 1);
        } else {
            bump_poll(id, 0);                     /* error: back off without using up an attempt */
        }
        usleep(200000);                           /* be gentle with Daraja */
    }

    /* Out of attempts: hand over to a human. */
    sqlite3_stmt *u;
    if (sqlite3_prepare_v2(db,
        "UPDATE orders SET status = 'needs_review' WHERE status = 'pending' AND polls >= ?1",
        -1, &u, NULL) == SQLITE_OK) {
        sqlite3_bind_int(u, 1, dj.max_polls);
        sqlite3_step(u);
        if (sqlite3_changes(db) > 0)
            fprintf(stderr, "ALERT: %d order(s) moved to needs_review (no answer from Daraja)\n",
                    sqlite3_changes(db));
        sqlite3_finalize(u);
    }
}

static void *poller_main(void *arg) {
    (void)arg;
    db = open_db();
    if (!db) { fprintf(stderr, "poller: cannot open db\n"); return NULL; }
    fprintf(stderr, "poller: running (base %ds, max %d polls, tick %ds)\n",
            dj.base_delay, dj.max_polls, dj.tick);
    for (;;) {
        poll_cycle();
        sleep((unsigned)dj.tick);
    }
    return NULL;
}

static int env_int(const char *name, int def) {
    const char *v = getenv(name);
    return v && *v ? atoi(v) : def;
}

static int load_daraja_config(void) {
    const char *key = getenv("DARAJA_CONSUMER_KEY"), *sec = getenv("DARAJA_CONSUMER_SECRET");
    const char *sc = getenv("DARAJA_SHORTCODE"), *pk = getenv("DARAJA_PASSKEY");
    if (!key || !sec || !sc || !pk || !*key || !*sec || !*sc || !*pk) return -1;   /* empty = unset */

    const char *base = getenv("DARAJA_BASE_URL");
    const char *envn = getenv("DARAJA_ENV");
    if (!base) base = (envn && !strcmp(envn, "production")) ? "https://api.safaricom.co.ke"
                                                            : "https://sandbox.safaricom.co.ke";
    snprintf(dj.base_url, sizeof dj.base_url, "%s", base);
    snprintf(dj.key, sizeof dj.key, "%s", key);
    snprintf(dj.secret, sizeof dj.secret, "%s", sec);
    snprintf(dj.shortcode, sizeof dj.shortcode, "%s", sc);
    snprintf(dj.passkey, sizeof dj.passkey, "%s", pk);
    dj.base_delay = env_int("POLL_BASE_SECS", 30);
    dj.max_polls  = env_int("POLL_MAX", 6);
    dj.tick       = env_int("POLL_TICK", 10);
    if (dj.base_delay < 1) dj.base_delay = 1;
    if (dj.max_polls < 1 || dj.max_polls > 12) dj.max_polls = 6;
    if (dj.tick < 1) dj.tick = 1;
    return 0;
}

/* ---------- endpoint handlers ---------- */

static void handle_callback(int fd, const char *body) {
    cJSON *root = cJSON_Parse(body);
    if (!root) { reply(fd, "400 Bad Request", "{\"error\":\"invalid json\"}"); return; }

    const cJSON *cb = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(root, "Body"), "stkCallback");
    const char *checkout = jstr(cb, "CheckoutRequestID");
    int rc;

    if (!checkout || jint_flex(cb, "ResultCode", &rc) != 0) {
        cJSON_Delete(root);
        reply(fd, "400 Bad Request", "{\"error\":\"unexpected payload\"}");
        return;
    }

    char receipt[64] = "", phone[32] = "";       /* rc 0 = success; anything else = failed/cancelled */
    long long cents = 0;

    if (rc == 0) {
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(cb, "CallbackMetadata"), "Item");
        const cJSON *it;
        cJSON_ArrayForEach(it, items) {
            const char  *name = jstr(it, "Name");
            const cJSON *v    = cJSON_GetObjectItemCaseSensitive(it, "Value");
            if (!name || !v) continue;
            if (!strcmp(name, "Amount") && cJSON_IsNumber(v))
                cents = llround(v->valuedouble * 100.0);
            else if (!strcmp(name, "MpesaReceiptNumber") && cJSON_IsString(v))
                snprintf(receipt, sizeof receipt, "%s", v->valuestring);
            else if (!strcmp(name, "PhoneNumber")) {
                if (cJSON_IsNumber(v)) snprintf(phone, sizeof phone, "%.0f", v->valuedouble);
                else if (cJSON_IsString(v)) snprintf(phone, sizeof phone, "%s", v->valuestring);
            }
        }
        if (!*receipt) {
            cJSON_Delete(root);
            reply(fd, "400 Bad Request", "{\"error\":\"missing receipt\"}");
            return;
        }
    }

    /* Success: dedupe on the M-Pesa receipt. Failure: dedupe on the checkout ID. */
    const char *key = rc == 0 ? receipt : checkout;
    int r = record_payment(key, checkout, receipt, cents, phone, rc, body, "callback");
    fprintf(stderr, "%s checkout=%s rc=%d receipt=%s cents=%lld\n",
            r < 0 ? "ERROR" : r ? "stored" : "duplicate", checkout, rc, receipt, cents);
    cJSON_Delete(root);

    if (r < 0) reply(fd, "500 Internal Server Error", "{\"error\":\"db\"}");
    else       reply(fd, "200 OK", "{\"ResultCode\":0,\"ResultDesc\":\"Accepted\"}");
}

static void handle_order(int fd, const char *body) {
    cJSON *root = cJSON_Parse(body);
    const char  *checkout = jstr(root, "checkout_request_id");
    const char  *ref      = jstr(root, "order_ref");
    const cJSON *amt      = cJSON_GetObjectItemCaseSensitive(root, "expected_amount");

    if (!checkout || !ref || !cJSON_IsNumber(amt)) {
        cJSON_Delete(root);
        reply(fd, "400 Bad Request",
              "{\"error\":\"need checkout_request_id, order_ref, expected_amount\"}");
        return;
    }

    sqlite3_stmt *s;
    sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO orders(checkout_id, order_ref, expected_cents) VALUES(?,?,?)",
        -1, &s, NULL);
    sqlite3_bind_text (s, 1, checkout, -1, SQLITE_STATIC);
    sqlite3_bind_text (s, 2, ref,      -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 3, llround(amt->valuedouble * 100.0));
    sqlite3_step(s);
    sqlite3_finalize(s);

    reconcile(checkout);   /* in case the callback beat the order registration */
    cJSON_Delete(root);
    reply(fd, "201 Created", "{\"ok\":true}");
}

static cJSON *summary(const char *sql, int money) {
    cJSON *arr = cJSON_CreateArray();
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return arr;
    while (sqlite3_step(s) == SQLITE_ROW) {
        cJSON *row = cJSON_CreateObject();
        cJSON_AddStringToObject(row, "status", (const char *)sqlite3_column_text(s, 0));
        cJSON_AddNumberToObject(row, "count",  sqlite3_column_int64(s, 1));
        if (money) cJSON_AddNumberToObject(row, "total", sqlite3_column_int64(s, 2) / 100.0);
        cJSON_AddItemToArray(arr, row);
    }
    sqlite3_finalize(s);
    return arr;
}

static void handle_report(int fd) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "payments", summary(
        "SELECT status, COUNT(*), COALESCE(SUM(amount_cents),0) FROM payments GROUP BY status", 1));
    cJSON_AddItemToObject(root, "orders", summary(
        "SELECT status, COUNT(*), 0 FROM orders GROUP BY status", 0));
    char *out = cJSON_PrintUnformatted(root);
    reply(fd, "200 OK", out);
    free(out);
    cJSON_Delete(root);
}

static const char *col_txt(sqlite3_stmt *s, int i) {
    const unsigned char *t = sqlite3_column_text(s, i);
    return t ? (const char *)t : "";
}

static void handle_order_get(int fd, const char *id) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
        "SELECT order_ref, expected_cents, status, polls, created_at "
        "FROM orders WHERE checkout_id = ?", -1, &s, NULL) != SQLITE_OK) {
        reply(fd, "500 Internal Server Error", "{\"error\":\"db\"}");
        return;
    }
    sqlite3_bind_text(s, 1, id, -1, SQLITE_STATIC);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        reply(fd, "404 Not Found", "{\"error\":\"order not found\"}");
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "checkout_request_id", id);
    cJSON_AddStringToObject(o, "order_ref", col_txt(s, 0));
    cJSON_AddNumberToObject(o, "expected_amount", sqlite3_column_int64(s, 1) / 100.0);
    cJSON_AddStringToObject(o, "status", col_txt(s, 2));
    cJSON_AddNumberToObject(o, "polls", sqlite3_column_int(s, 3));
    cJSON_AddStringToObject(o, "created_at", col_txt(s, 4));
    sqlite3_finalize(s);

    /* Most recent payment record for this checkout. */
    sqlite3_stmt *p;
    cJSON *pay = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT receipt, amount_cents, phone, source, status, result_code "
        "FROM payments WHERE checkout_id = ? ORDER BY rowid DESC LIMIT 1",
        -1, &p, NULL) == SQLITE_OK) {
        sqlite3_bind_text(p, 1, id, -1, SQLITE_STATIC);
        if (sqlite3_step(p) == SQLITE_ROW) {
            pay = cJSON_CreateObject();
            cJSON_AddStringToObject(pay, "receipt", col_txt(p, 0));
            cJSON_AddNumberToObject(pay, "amount", sqlite3_column_int64(p, 1) / 100.0);
            cJSON_AddStringToObject(pay, "phone", col_txt(p, 2));
            cJSON_AddStringToObject(pay, "source", col_txt(p, 3));
            cJSON_AddStringToObject(pay, "status", col_txt(p, 4));
            cJSON_AddNumberToObject(pay, "result_code", sqlite3_column_int(p, 5));
        }
        sqlite3_finalize(p);
    }
    if (pay) cJSON_AddItemToObject(o, "payment", pay);
    else     cJSON_AddNullToObject(o, "payment");

    char *out = cJSON_PrintUnformatted(o);
    reply(fd, "200 OK", out);
    free(out);
    cJSON_Delete(o);
}

static void handle_orders_list(int fd, const char *status) {
    sqlite3_stmt *s;
    const char *sql = status[0]
        ? "SELECT checkout_id, order_ref, expected_cents, status, created_at FROM orders "
          "WHERE status = ?1 ORDER BY rowid DESC LIMIT 100"
        : "SELECT checkout_id, order_ref, expected_cents, status, created_at FROM orders "
          "ORDER BY rowid DESC LIMIT 100";
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) {
        reply(fd, "500 Internal Server Error", "{\"error\":\"db\"}");
        return;
    }
    if (status[0]) sqlite3_bind_text(s, 1, status, -1, SQLITE_STATIC);

    cJSON *arr = cJSON_CreateArray();
    while (sqlite3_step(s) == SQLITE_ROW) {
        cJSON *row = cJSON_CreateObject();
        cJSON_AddStringToObject(row, "checkout_request_id", col_txt(s, 0));
        cJSON_AddStringToObject(row, "order_ref", col_txt(s, 1));
        cJSON_AddNumberToObject(row, "expected_amount", sqlite3_column_int64(s, 2) / 100.0);
        cJSON_AddStringToObject(row, "status", col_txt(s, 3));
        cJSON_AddStringToObject(row, "created_at", col_txt(s, 4));
        cJSON_AddItemToArray(arr, row);
    }
    sqlite3_finalize(s);
    char *out = cJSON_PrintUnformatted(arr);
    reply(fd, "200 OK", out);
    free(out);
    cJSON_Delete(arr);
}

/* ---------- request routing ---------- */

/* Returns 1 when a full request was handled, 0 if more data is needed. */
static int handle_request(Conn *c) {
    c->buf[c->len] = 0;
    char *hdr_end = strstr(c->buf, "\r\n\r\n");
    if (!hdr_end) return 0;

    char *body = hdr_end + 4;
    size_t content_len = 0;
    char *cl = strcasestr(c->buf, "\r\nContent-Length:");
    if (cl && cl < body) content_len = (size_t)atol(cl + 17);

    size_t have = c->len - (size_t)(body - c->buf);
    if (have < content_len) return 0;
    body[content_len] = 0;

    char method[8], path[300];
    if (sscanf(c->buf, "%7s %299s", method, path) != 2) {
        reply(c->fd, "400 Bad Request", "{}");
        return 1;
    }

    if (!strcmp(method, "POST") && secure_eq(path, callback_path)) {
        handle_callback(c->fd, body);
    } else if (!strcmp(method, "POST") && !strcmp(path, "/order")) {
        if (!is_admin(c->buf, hdr_end)) reply(c->fd, "401 Unauthorized", "{}");
        else handle_order(c->fd, body);
    } else if (!strcmp(method, "GET") && !strcmp(path, "/report")) {
        if (!is_admin(c->buf, hdr_end)) reply(c->fd, "401 Unauthorized", "{}");
        else handle_report(c->fd);
    } else if (!strcmp(method, "GET") && !strncmp(path, "/order/", 7)) {
        const char *id = path + 7;
        int valid = *id != 0;
        for (const char *q = id; *q; q++)
            if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
                  (*q >= '0' && *q <= '9') || *q == '_' || *q == '-')) valid = 0;
        if (!is_admin(c->buf, hdr_end)) reply(c->fd, "401 Unauthorized", "{}");
        else if (!valid) reply(c->fd, "400 Bad Request", "{\"error\":\"bad id\"}");
        else handle_order_get(c->fd, id);
    } else if (!strcmp(method, "GET") && (!strcmp(path, "/orders") || !strncmp(path, "/orders?", 8))) {
        if (!is_admin(c->buf, hdr_end)) {
            reply(c->fd, "401 Unauthorized", "{}");
        } else {
            char st[32] = "";
            const char *q = strstr(path, "status=");
            if (q) {
                q += 7;
                size_t i = 0;
                while (*q && *q != '&' && i < sizeof st - 1) {
                    if ((*q >= 'a' && *q <= 'z') || *q == '_') st[i++] = *q;
                    q++;
                }
                st[i] = 0;
            }
            handle_orders_list(c->fd, st);
        }
    } else {
        reply(c->fd, "404 Not Found", "{}");
    }
    return 1;
}

/* ---------- event loop ---------- */

static void set_nonblock(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static void close_conn(int ep, Conn *c) {
    epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    free(c);
}

int main(void) {
    const char *cb_secret = getenv("CALLBACK_SECRET");
    admin_token = getenv("ADMIN_TOKEN");
    if (!cb_secret || !*cb_secret || !admin_token || !*admin_token) {
        fprintf(stderr, "Set CALLBACK_SECRET and ADMIN_TOKEN\n");
        return 1;
    }
    if (!strcmp(cb_secret, "change-me") || !strcmp(admin_token, "change-me-too")) {
        fprintf(stderr, "Refusing to start with the placeholder secrets from .env.example. "
                        "Generate real ones, e.g. `openssl rand -hex 24`.\n");
        return 1;
    }
    if (strlen(cb_secret) < 16 || strlen(admin_token) < 16)
        fprintf(stderr, "WARNING: CALLBACK_SECRET and ADMIN_TOKEN should be at least 16 random characters\n");
    snprintf(callback_path, sizeof callback_path, "/mpesa/callback/%s", cb_secret);
    signal(SIGPIPE, SIG_IGN);

    db = open_db();
    if (!db) { fprintf(stderr, "db open failed\n"); return 1; }
    init_schema();

    if (load_daraja_config() == 0) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        pthread_t th;
        if (pthread_create(&th, NULL, poller_main, NULL) == 0) pthread_detach(th);
    } else {
        fprintf(stderr, "WARNING: DARAJA_* variables not set, polling fallback DISABLED\n");
    }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    int port = env_int("PORT", DEFAULT_PORT);
    const char *bind_addr = getenv("BIND_ADDR");
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (inet_pton(AF_INET, bind_addr && *bind_addr ? bind_addr : "0.0.0.0", &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid BIND_ADDR\n");
        return 1;
    }
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    listen(lfd, 512);
    set_nonblock(lfd);

    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = NULL };
    epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev);

    fprintf(stderr, "listening on %s:%d\n", bind_addr && *bind_addr ? bind_addr : "0.0.0.0", port);
    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int n = epoll_wait(ep, events, MAX_EVENTS, -1);
        for (int i = 0; i < n; i++) {
            Conn *c = events[i].data.ptr;

            if (c == NULL) {
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
            for (;;) {
                ssize_t r = read(c->fd, c->buf + c->len, BUF_SIZE - 1 - c->len);
                if (r > 0) { c->len += (size_t)r; continue; }
                if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) done = 1;
                break;
            }
            if (c->len >= BUF_SIZE - 1) { reply(c->fd, "413 Payload Too Large", "{}"); done = 1; }
            else if (c->len > 0 && handle_request(c)) done = 1;

            if (done || (events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)))
                close_conn(ep, c);
        }
    }
}
