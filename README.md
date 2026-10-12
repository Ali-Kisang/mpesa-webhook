# mpesa-webhook

A small, fast payment-callback service for **M-Pesa (Safaricom Daraja STK Push)**, written in C.

It sits next to your application (which can be written in **any language**) and does the part that
usually goes wrong: **making sure every payment is recorded exactly once, even when Safaricom's
callback is late, duplicated, out of order, or never arrives.**

- Receives Safaricom's callbacks and records them safely (no double-crediting)
- **Falls back to asking Safaricom directly** when a callback is lost (STK Push Query polling)
- Matches each payment to the order you expected, and flags wrong amounts
- Gives your app a simple HTTP API: "was order X paid?"
- Single binary, SQLite storage, one thread for HTTP (`epoll`) and one for polling; uses little memory

> **Status: starter-grade, not battle-tested in production.** Read [Tested vs not tested](#tested-vs-not-tested)
> and [Limitations](#limitations) before putting real money through it.

---

## Contents

1. [The problem it solves](#the-problem-it-solves)
2. [How it works](#how-it-works)
3. [Quick start (no Safaricom account needed)](#quick-start-no-safaricom-account-needed)
4. [Requirements and building](#requirements-and-building)
5. [Configuration](#configuration)
6. [Going live with Safaricom](#going-live-with-safaricom)
7. [HTTP API reference](#http-api-reference)
8. [Order and payment statuses](#order-and-payment-statuses)
9. [Using it from other languages](#using-it-from-other-languages)
10. [Deployment](#deployment)
11. [Operations](#operations)
12. [Testing](#testing)
13. [Security notes](#security-notes)
14. [Limitations](#limitations)
15. [Troubleshooting](#troubleshooting)
16. [Project layout](#project-layout)

---

## The problem it solves

When a customer pays with STK Push, Safaricom reports the result by sending an HTTP request (a
*callback*) to a URL you provided. Many systems treat that callback as the **only** source of truth, so:

| What goes wrong | Typical result |
|---|---|
| Your server is down or slow when Safaricom calls | The payment succeeded but your system never knows |
| The callback is delivered twice | Customer is credited twice |
| The callback arrives before your app has saved the order | Payment can't be matched and is lost |
| Customer paid a different amount than expected | Goes unnoticed |

This service handles each case:

| Situation | What mpesa-webhook does |
|---|---|
| Callback lost | The poller asks Safaricom about the order and records the answer |
| Duplicate callback | Ignored (deduplicated on the M-Pesa receipt number) |
| Callback before order | Kept as `unmatched`, then matched automatically when the order is registered |
| Wrong amount | Order marked `amount_mismatch` |
| Nobody answers at all | Order becomes `needs_review` so a human looks at it |

---

## How it works

```
 Your app (any language)                mpesa-webhook                      Safaricom (Daraja)
 ───────────────────────                ─────────────                      ──────────────────
 1. POST STK push  ─────────────────────────────────────────────────────────────►
                   ◄───────────────────────────────────────────── CheckoutRequestID
 2. POST /order  ──────────────────────►  stores "expected payment"
    (checkout id, your ref, amount)
                                                                  customer enters PIN
                        ┌──────────────────────────────────────────────┐
                        │ 3a. callback  ──►  POST /mpesa/callback/<secret>
                        │     matched against the order, status updated
                        │
                        │ 3b. if no callback arrives:
                        │     poller ── STK Push Query ─────────────────►
                        │            ◄───────────────────────────────────  result
                        └──────────────────────────────────────────────┘
 4. GET /order/<checkout id>  ─────────►  { status: "paid", payment: {...} }
```

### Polling timeline (defaults)

An order that is still `pending` is queried at roughly **0:30, 1:30, 3:30, 7:30, 15:30 and 31:30**
after it was registered (30 s, then the wait doubles each time). If Safaricom has not given a final answer
by then, the order becomes `needs_review` and the server logs an `ALERT` line. A callback that arrives
later still resolves the order automatically. Tune this with `POLL_BASE_SECS` and `POLL_MAX`.

Safaricom's *query* response says only whether the payment succeeded or failed. It does **not** include the
receipt number or amount. So a payment confirmed by polling is recorded as `paid_unverified` and is upgraded
to `paid` (or `amount_mismatch`) if the real callback arrives later.

---

## Quick start (no Safaricom account needed)

This runs the whole system locally against a **mock Daraja server** included in `test/`.

```bash
# 1. Build (see "Requirements" below for the packages you need)
make

# 2. Terminal A: start the mock Safaricom
python3 test/mock_daraja.py 9099

# 3. Terminal B: start the service pointed at the mock
export CALLBACK_SECRET=demo-callback-secret-1234
export ADMIN_TOKEN=demo-admin-token-1234567
DARAJA_BASE_URL=http://127.0.0.1:9099 \
DARAJA_CONSUMER_KEY=k DARAJA_CONSUMER_SECRET=s \
DARAJA_SHORTCODE=174379 DARAJA_PASSKEY=pk \
POLL_BASE_SECS=5 POLL_MAX=3 ./mpesa_webhook

# 4. Terminal C: make "payments" with the Python example
export SERVICE_URL=http://127.0.0.1:8080
export ADMIN_TOKEN=demo-admin-token-1234567
export CALLBACK_URL=http://127.0.0.1:8080/mpesa/callback/demo-callback-secret-1234
export DARAJA_BASE_URL=http://127.0.0.1:9099
export DARAJA_CONSUMER_KEY=k DARAJA_CONSUMER_SECRET=s DARAJA_SHORTCODE=174379 DARAJA_PASSKEY=pk

python3 examples/python/mpesa_client.py 254700000001 25 DEMO-1   # callback delivered -> paid
python3 examples/python/mpesa_client.py 254700000002 25 DEMO-2   # callback LOST -> paid_unverified (via polling)
python3 examples/python/mpesa_client.py 254700000003 25 DEMO-3   # customer cancels -> failed
python3 examples/python/mpesa_client.py 254700000004 25 DEMO-4   # Safaricom never answers -> needs_review (about 40 s with these demo settings)

curl -s localhost:8080/report -H "X-Admin-Token: $ADMIN_TOKEN"
```

The mock decides the outcome from the **last digit of the phone number** (see the top of `test/mock_daraja.py`).

---

## Requirements and building

**Platform:** Linux (uses `epoll`). On macOS or Windows, run it in Docker or a Linux VM.

**Libraries** (development headers):

| Library | Debian / Ubuntu package | Fedora / RHEL package (names may vary) |
|---|---|---|
| cJSON | `libcjson-dev` | `cjson-devel` |
| SQLite 3 | `libsqlite3-dev` | `sqlite-devel` |
| OpenSSL | `libssl-dev` | `openssl-devel` |
| libcurl | `libcurl4-openssl-dev` | `libcurl-devel` |

Plus `gcc` and `make`. Python 3 is only needed for the tests and the mock server.

```bash
sudo apt install gcc make libcjson-dev libsqlite3-dev libssl-dev libcurl4-openssl-dev
make                # builds ./mpesa_webhook
sudo make install   # optional: copies it to /usr/local/bin
```

Or without `make`:

```bash
gcc -O2 -Wall -o mpesa_webhook src/mpesa_webhook.c -lcjson -lsqlite3 -lcrypto -lcurl -lpthread -lm
```

Or with Docker (see [Deployment](#deployment)).

---

## Configuration

Everything is configured with environment variables. Copy `.env.example` as a starting point.

### Required

| Variable | Meaning |
|---|---|
| `CALLBACK_SECRET` | Secret part of your callback URL: `https://YOUR_DOMAIN/mpesa/callback/<CALLBACK_SECRET>`. Daraja does not sign callbacks, so this secret is what stops strangers posting fake payments. Use 16+ random characters (`openssl rand -hex 24`). |
| `ADMIN_TOKEN` | Token your own app sends in the `X-Admin-Token` header to use the `/order*` and `/report` endpoints. |

The server refuses to start if you leave the placeholder values from `.env.example`, and warns if either
secret is shorter than 16 characters.

### Polling fallback (all four needed, otherwise polling is disabled)

| Variable | Meaning |
|---|---|
| `DARAJA_CONSUMER_KEY` | From your app on the Daraja portal |
| `DARAJA_CONSUMER_SECRET` | From your app on the Daraja portal |
| `DARAJA_SHORTCODE` | Your paybill/till/shortcode (the sandbox uses a test one) |
| `DARAJA_PASSKEY` | The Lipa Na M-Pesa passkey for that shortcode |
| `DARAJA_ENV` | `sandbox` (default) or `production` |
| `DARAJA_BASE_URL` | Overrides the URL entirely (used by tests and the mock) |

Empty values count as "not set". Without them the server still receives callbacks; it just logs
`WARNING: ... polling fallback DISABLED`.

### Optional

| Variable | Default | Meaning |
|---|---|---|
| `PORT` | `8080` | TCP port to listen on |
| `BIND_ADDR` | `0.0.0.0` | Address to bind. Use `127.0.0.1` when a reverse proxy runs on the same machine |
| `DB_PATH` | `mpesa.db` | SQLite file (created automatically) |
| `POLL_BASE_SECS` | `30` | Seconds before the first poll; doubles after each poll |
| `POLL_MAX` | `6` | Polls before an order becomes `needs_review` (1 to 12) |
| `POLL_TICK` | `10` | How often the poller wakes up to look for due orders |

---

## Going live with Safaricom

1. **Create a Daraja app** at <https://developer.safaricom.co.ke>, enable *Lipa Na M-Pesa Online*, and note the
   consumer key, consumer secret, shortcode and passkey. Start in the **sandbox**.
2. **Get a public HTTPS URL** for the callback. Safaricom must be able to reach it from the internet
   (`localhost` will not work; for development use a tunnel such as ngrok or cloudflared). The
   [Docker Compose setup](#docker-compose-https-included) gets a certificate for you automatically.
3. **Start the service** with your secrets and Daraja credentials.
4. **Send the STK push from your app** with `CallBackURL` set to
   `https://YOUR_DOMAIN/mpesa/callback/<CALLBACK_SECRET>` (the language examples show the full request).
5. **Immediately register the order**: `POST /order` with the `CheckoutRequestID` Safaricom returned.
   Registering after the push is fine; if the callback beats you, it is matched when you register.
6. **Show the customer a "waiting for payment" state** and read the result from `GET /order/<id>`.
7. When everything works in the sandbox, switch `DARAJA_ENV=production`, use your production credentials,
   and run a small real payment end to end before you launch.

> **Verify against Safaricom's own documentation.** I wrote the Daraja request and response formats from
> public SDKs and examples, not from Safaricom's official pages, and could not call the real sandbox when
> building this. Check the STK Push and STK Push Query pages for current field names, the full list of
> result codes (see `terminal_fail` in `src/mpesa_webhook.c`), till-number vs paybill `TransactionType`, and the
> list of Safaricom callback IP addresses.

---

## HTTP API reference

All endpoints except the callback require the header `X-Admin-Token: <ADMIN_TOKEN>` and return JSON.
Never call them from a browser or mobile app (the token would leak). Call them from your server.

### `POST /mpesa/callback/<CALLBACK_SECRET>`

For Safaricom only. Parses the STK callback (`Body.stkCallback`), records the payment, reconciles it.

| Response | When |
|---|---|
| `200 {"ResultCode":0,"ResultDesc":"Accepted"}` | Recorded (or already recorded; duplicates are also `200` so Safaricom stops retrying) |
| `400` | Body is not valid JSON or is not an STK callback |
| `404` | Wrong secret in the path |

Successful payments are deduplicated on `MpesaReceiptNumber`; failed or cancelled ones on `CheckoutRequestID`.

### `POST /order`

Registers the payment you are expecting. Call it right after the STK push.

```bash
curl -X POST http://localhost:8080/order \
  -H "X-Admin-Token: $ADMIN_TOKEN" \
  -d '{"checkout_request_id":"ws_CO_191220191020363925","order_ref":"INV-1001","expected_amount":1500}'
```

| Field | Type | Meaning |
|---|---|---|
| `checkout_request_id` | string | `CheckoutRequestID` from Safaricom's STK push response |
| `order_ref` | string | Your own reference (invoice number, cart id, ...) |
| `expected_amount` | number | Amount in KES |

Returns `201 {"ok":true}`. Registering the same `checkout_request_id` twice is harmless (ignored).
`400` if a field is missing, `401` if the token is wrong.

### `GET /order/<checkout_request_id>`

The endpoint your app polls to learn whether an order was paid.

```json
{
  "checkout_request_id": "ws_CO_191220191020363925",
  "order_ref": "INV-1001",
  "expected_amount": 1500,
  "status": "paid",
  "polls": 0,
  "created_at": "2026-10-12 08:15:02",
  "payment": {
    "receipt": "QJA1AAA111",
    "amount": 1500,
    "phone": "254700000000",
    "source": "callback",
    "status": "matched",
    "result_code": 0
  }
}
```

`payment` is `null` until something has been recorded. `source` is `callback` or `query` (found by polling).
`404` if the order is unknown; `400` if the id contains characters other than letters, digits, `_` and `-`.

### `GET /orders` and `GET /orders?status=<status>`

Up to the 100 most recent orders, newest first, optionally filtered. Typical use:
`/orders?status=needs_review` for an alerting job.

```json
[{"checkout_request_id":"ws_CO_...","order_ref":"INV-1001","expected_amount":1500,
  "status":"needs_review","created_at":"2026-10-12 08:15:02"}]
```

### `GET /report`

Counts and totals by status, for dashboards and daily reports.

```json
{"payments":[{"status":"matched","count":42,"total":63500},{"status":"failed","count":3,"total":0}],
 "orders":[{"status":"paid","count":42},{"status":"failed","count":3},{"status":"needs_review","count":1}]}
```

---

## Order and payment statuses

### Order status (`GET /order/...` -> `status`)

| Status | Meaning | What your app should do |
|---|---|---|
| `pending` | Registered, no answer yet | Show "waiting for payment" and keep checking |
| `paid` | Callback received, amount matches | Fulfil the order |
| `paid_unverified` | Safaricom confirmed success when polled, but no callback yet, so the amount is unverified | Safe to fulfil (an STK push amount is fixed by your request, so risk is low). It upgrades to `paid` when the callback arrives. Include it in your reconciliation checks |
| `amount_mismatch` | Payment succeeded but the amount differs from `expected_amount` | **Do not auto-fulfil.** Investigate. With STK push this usually means you registered the wrong expected amount |
| `failed` | Customer cancelled, wrong PIN, timeout, or insufficient funds | Offer to try again (a new STK push creates a new `CheckoutRequestID`) |
| `needs_review` | Neither callback nor query gave an answer within the polling window | A human should check the M-Pesa portal or statement. A late callback can still resolve it automatically |

### Payment status (inside `payment.status`, and `/report`)

| Status | Meaning |
|---|---|
| `matched` | Callback payment, amount equals expected |
| `mismatch` | Callback payment, amount differs |
| `confirmed_unverified` | Found by polling (no amount available) |
| `unmatched` | Successful payment with no registered order (yet) |
| `failed` | Cancelled / failed result |
| `pending` | Transient, only while a record is being processed |

---

## Using it from other languages

**You do not need to write any C to use this.** mpesa-webhook is a separate process that speaks plain HTTP + JSON.
Your application, in any language, only has to make **four HTTP calls**:

| # | Call | Goes to | When |
|---|---|---|---|
| 1 | Get an OAuth token, then **STK push** | Safaricom | When the customer clicks "Pay" |
| 2 | `POST /order` | mpesa-webhook | Immediately after the push succeeds |
| 3 | `GET /order/<checkout id>` | mpesa-webhook | Repeatedly, until the status is no longer `pending` |
| 4 | `GET /orders?status=needs_review` | mpesa-webhook | From a scheduled job, to alert a human |

Safaricom's callback goes straight to mpesa-webhook, never to your app. Your app does not need to
expose a callback endpoint at all.

### Ready-made examples

Each example below does steps 1 to 3 end to end, uses **no third-party packages**, and was run
successfully against the included mock (see [Tested vs not tested](#tested-vs-not-tested)).

| Language | File | Runtime | Run |
|---|---|---|---|
| Python | `examples/python/mpesa_client.py` | Python 3.8+ | `python3 mpesa_client.py 2547XXXXXXXX 10 INV-1` |
| Node.js | `examples/node/mpesa_client.js` | Node 18+ (built-in `fetch`) | `node mpesa_client.js 2547XXXXXXXX 10 INV-1` |
| PHP | `examples/php/mpesa_client.php` | PHP 7.4+ with `php-curl` | `php mpesa_client.php 2547XXXXXXXX 10 INV-1` |
| Go | `examples/go/main.go` | Go 1.21+ | `go run . 2547XXXXXXXX 10 INV-1` |
| Java | `examples/java/MpesaClient.java` | Java 11+ | `java MpesaClient.java 2547XXXXXXXX 10 INV-1` |
| curl / shell | `examples/curl/demo.sh` | bash + curl | `ADMIN_TOKEN=... ./demo.sh` |

They all read the same environment variables:

```bash
export SERVICE_URL=http://localhost:8080            # where mpesa-webhook runs
export ADMIN_TOKEN=...                              # same as the server's ADMIN_TOKEN
export CALLBACK_URL=https://pay.example.com/mpesa/callback/<CALLBACK_SECRET>
export DARAJA_CONSUMER_KEY=...  DARAJA_CONSUMER_SECRET=...
export DARAJA_SHORTCODE=...     DARAJA_PASSKEY=...
# optional: DARAJA_BASE_URL (defaults to the Safaricom sandbox)
```

### Any other language (C#, Ruby, Kotlin, Rust, Dart, ...)

Copy the structure of any example. The service contract is just this:

```
POST   {SERVICE_URL}/order                     header X-Admin-Token: ...   body {"checkout_request_id","order_ref","expected_amount"}
GET    {SERVICE_URL}/order/{checkout_request_id}   header X-Admin-Token: ...   -> {"status": "...", "payment": {...}}
```

Port the five small functions you see in every example: `request` (HTTP helper), `daraja_token`, `stk_push`,
`register_order`, `wait_for_result`.

### Integration patterns for a real web app

**Checkout page.** Never put `ADMIN_TOKEN` in browser or mobile code. Instead:

```
Browser  ──"Pay"──►  Your backend  ──STK push + POST /order──►  Safaricom / mpesa-webhook
Browser  ◄─{status}─ Your backend  ◄─GET /order/<id>──────────  mpesa-webhook     (browser repeats every 2-3 s)
```

Your backend exposes something like `GET /api/payments/<your order id>/status`, looks up the stored
`CheckoutRequestID`, calls mpesa-webhook, and returns just the status to the browser. You can also push the
result with Server-Sent Events or WebSockets instead of polling from the browser.

**Fulfilment rule of thumb:**

```
paid, paid_unverified     -> deliver goods / activate service
pending                   -> keep waiting
failed                    -> let the customer retry
amount_mismatch, needs_review -> hold, alert a human
```

**Nightly or hourly alert job** (cron example; needs `jq` and a working `mail` command, or swap in an SMS API):

```bash
#!/usr/bin/env bash
n=$(curl -s "$SERVICE_URL/orders?status=needs_review" -H "X-Admin-Token: $ADMIN_TOKEN" | jq length)
[ "$n" -gt 0 ] && echo "$n M-Pesa order(s) need review" | mail -s "M-Pesa alert" you@example.com
```

**Where to run your app relative to the service.** Easiest is the same machine or the same Docker network
(`http://app:8080` inside Compose). If your app is elsewhere, reach the admin endpoints over a private
network, VPN or SSH tunnel; do not expose them publicly.

---

## Deployment

### Docker Compose (HTTPS included)

Runs the service plus [Caddy](https://caddyserver.com), which obtains and renews an HTTPS certificate
automatically. Only `/mpesa/callback/*` is reachable from the internet.

```bash
cp .env.example .env          # fill in secrets, Daraja credentials, and DOMAIN
# point your domain's DNS A record at the server, open ports 80 and 443
docker compose up -d --build
docker compose logs -f app
```

Your callback URL is then `https://<DOMAIN>/mpesa/callback/<CALLBACK_SECRET>`. The SQLite database lives in the
`mpesa-data` volume. Your own app reaches the admin API at `http://app:8080` from inside the Compose network.

### systemd on a Linux server

See the instructions at the top of `deploy/mpesa-webhook.service`: build, `make install`, create a service
user, put your settings in `/etc/mpesa-webhook.env` (mode 600), enable the unit. It binds to `127.0.0.1`, so put
nginx or Caddy in front for HTTPS.

### Behind nginx

Expose only the callback path, and keep the rest internal:

```nginx
location /mpesa/callback/ {
    proxy_pass http://127.0.0.1:8080;
}
# do NOT proxy /order, /orders or /report to the internet
```

Note that nginx writes request paths (which contain your callback secret) to its access log by default.
Disable or mask the log for that location.

---

## Operations

**Logs** go to stderr (journald under systemd, `docker compose logs` under Docker). Look for:

| Line | Meaning |
|---|---|
| `stored checkout=... rc=0 receipt=...` | A callback was recorded |
| `duplicate checkout=...` | A repeat callback was ignored (normal) |
| `poller: ... confirmed by query` | A lost callback was recovered by polling |
| `ALERT: N order(s) moved to needs_review` | **Needs a human.** Check the M-Pesa portal |
| `poller: token request failed` | Wrong Daraja credentials or Safaricom unreachable |

**Monitoring.** Alert on `needs_review` (see the cron example above), and put an uptime check on
`GET https://<DOMAIN>/` (any response means the proxy is up) or on a synthetic order flow.

**Inspecting the database:**

```bash
sqlite3 mpesa.db "select order_ref, status, polls, created_at from orders order by rowid desc limit 20;"
sqlite3 mpesa.db "select checkout_id, receipt, amount_cents/100.0, source, status from payments order by rowid desc limit 20;"
```

**Backups.** The database is a single file in WAL mode. Use SQLite's online backup, not `cp`:

```bash
sqlite3 mpesa.db ".backup '/backups/mpesa-$(date +%F).db'"
```

For continuous replication consider [Litestream](https://litestream.io).

**Privacy.** The database stores customers' phone numbers. Restrict file permissions, include it in your
data-protection policy (see Kenya's Data Protection Act), and decide how long you keep records.

**Upgrading.** The server adds new columns to an existing database automatically on startup.

---

## Testing

```bash
make test        # 26 end-to-end checks against the mock Daraja (about 15 seconds)
make test-asan   # the same checks on a build with AddressSanitizer + UndefinedBehaviorSanitizer
bash test/test_examples.sh   # runs every language example that is installed on your machine
```

`make test` starts a mock Safaricom and the real binary on free ports, then verifies: authentication, matching
and mismatched amounts, duplicate callbacks, callbacks that arrive before the order, cancellations, polling
recovery, `needs_review`, late callbacks upgrading polled results, no double-counting, and that every request
to the mock carried valid credentials and a valid password.

### Tested vs not tested

**Tested** (Ubuntu 24.04, GCC 13.3):
- The 26 end-to-end checks above, including under AddressSanitizer / UBSan
- All five language examples against the mock, in both scenarios (callback delivered, callback lost):
  Python 3.12, Node 22, PHP 8.3, Go 1.22, Java 21
- The `curl` demo script; the generic HMAC webhook in `extras/`
- `docker-compose.yml` parses as valid YAML

**Not tested:**
- **The real Safaricom sandbox or production.** All Daraja behaviour was tested against my mock, which
  reproduces what public documentation and SDKs describe (string `ResultCode` in query responses, the
  "still being processed" error, token expiry as a string). Real Daraja may differ.
- The Dockerfile build, the Caddy configuration and the systemd unit (they were written carefully but not run)
- Behaviour under load, or after weeks of uptime. No benchmark has been done yet.
- Fuzzing the HTTP parser

---

## Security notes

- **Callback authentication is a secret URL**, because Daraja does not sign callbacks. Keep `CALLBACK_SECRET`
  long and random, serve the callback over HTTPS, and keep it out of logs. For stronger protection, also
  restrict the callback path to Safaricom's published IP addresses at your firewall or proxy (see
  `deploy/Caddyfile`). Obtain the list from Safaricom's documentation.
- **Admin endpoints are protected only by `ADMIN_TOKEN`.** Keep them off the public internet.
  Token and secret comparisons are constant-time.
- **TLS is not built in.** Always put a reverse proxy in front (Caddy, nginx).
- **Amounts are stored as integer cents** to avoid floating-point rounding errors.
- **All SQL uses bound parameters**; JSON is parsed with cJSON, not by hand.
- **The service holds your Daraja secrets in memory and environment variables.** Do not commit `.env`. It is
  in `.gitignore`.
- The HTTP handling is deliberately minimal (single read buffer of 16 KB, `Connection: close`, no
  keep-alive). That is fine for Safaricom's small callbacks but means it should not be exposed as a general web
  server. Run `make test-asan` after any change, and consider fuzzing before heavy exposure.

---

## Limitations

- **Only STK Push (Lipa Na M-Pesa Online) is handled.** Customer-initiated payments to a paybill or till
  (C2B confirmation/validation URLs) and B2C/B2B use different payloads and are **not** supported.
- **Single instance, local SQLite.** The service itself is a single point of failure: use a process
  supervisor (Docker restart policy or systemd), monitor it, and keep backups. Polling protects you from
  *lost callbacks*. While the server is down, Safaricom's callbacks to it are lost too, and polling cannot run.
  Pending orders live in SQLite, so after a restart they are polled again, but I have not verified how long
  Safaricom keeps answering STK queries for old transactions, so a long outage may still need manual
  reconciliation against your M-Pesa statement.
- **Linux only** (`epoll`).
- `GET /orders` returns at most the latest 100 orders; there is no pagination or date filter yet.
- Polling uses Safaricom's STK Query only for orders registered through `POST /order`. A payment for which
  you never registered an order is stored as `unmatched`.
- No rate limiting, no per-client API keys, no web dashboard.
- Responses are written with a single `write()`; very large responses on a slow connection could be cut short.

---

## Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| Build fails with `fatal error: ...h: No such file or directory` | A development package is missing; see [Requirements](#requirements-and-building) |
| `Set CALLBACK_SECRET and ADMIN_TOKEN` | Both are required environment variables |
| `Refusing to start with the placeholder secrets` | Replace the values from `.env.example` (`openssl rand -hex 24`) |
| `WARNING: DARAJA_* variables not set, polling fallback DISABLED` | Set all four `DARAJA_*` variables (empty counts as unset) |
| `poller: token request failed (http 400/401)` | Wrong consumer key/secret, or sandbox keys used with `DARAJA_ENV=production` (or the reverse) |
| `poller: token request failed (http -1)` | The server cannot reach Safaricom (DNS, firewall, no outbound internet) |
| Callbacks never arrive | The callback URL must be public HTTPS; check `CallBackURL` in your push, the secret in the path, firewall/proxy, and DNS. Use a tunnel during development |
| Orders sit at `pending` for a long time | Normal until the first poll (30 s by default). Check logs for `poller:` lines. Polling needs the `DARAJA_*` variables |
| `401` from the API | Missing or wrong `X-Admin-Token` header |
| `bind: Address already in use` | Another process uses the port; set `PORT` |
| An order is `paid_unverified` for long | The callback never arrived. The payment *is* confirmed by Safaricom; reconcile the amount against your M-Pesa statement |
| `amount_mismatch` | `expected_amount` registered with `/order` differs from what Safaricom reports. Check your amount handling (cents vs shillings) |
| Phone number format rejected by Safaricom | Use international format `2547XXXXXXXX` with no `+` |

---

## Project layout

```
mpesa-webhook/
├── README.md                     this file
├── Makefile                      make | make debug | make test | make test-asan | make install
├── Dockerfile                    two-stage build, runs as a non-root user
├── docker-compose.yml            service + Caddy (automatic HTTPS)
├── .env.example                  configuration template
├── src/
│   └── mpesa_webhook.c           the whole service (one file)
├── examples/
│   ├── python/mpesa_client.py    STK push + order registration + status polling
│   ├── node/mpesa_client.js
│   ├── php/mpesa_client.php
│   ├── go/{main.go,go.mod}
│   ├── java/MpesaClient.java
│   └── curl/demo.sh              the API contract in plain curl
├── deploy/
│   ├── Caddyfile                 public callback only, everything else 404
│   └── mpesa-webhook.service     systemd unit with hardening
├── test/
│   ├── mock_daraja.py            mock Safaricom (also runnable stand-alone for demos)
│   ├── test_e2e.py               26 end-to-end checks
│   └── test_examples.sh          runs every installed language example
└── extras/
    └── generic_hmac_webhook.c    simpler webhook for providers that sign requests with HMAC-SHA256
                                  (e.g. Stripe-style); not used by the M-Pesa service
```

### Code map (`src/mpesa_webhook.c`)

| Section | What it does |
|---|---|
| `database` | Schema, `reconcile()`, `record_payment()` (idempotent, transactional) |
| `polling fallback` | OAuth token cache, STK Push Query, backoff, `needs_review` |
| `endpoint handlers` | Callback, `/order`, `/order/<id>`, `/orders`, `/report` |
| `request routing` | Minimal HTTP parsing and dispatch |
| `event loop` | Single-threaded `epoll` server |

The HTTP server runs on the main thread; the poller runs on a second thread with its own SQLite connection
(WAL mode, 5 s busy timeout), and every write goes through a transaction.

---

## Disclaimer and licence

This is example-quality software that handles money. Review it, test it with the Safaricom sandbox, and run
your own security review before using it in production. No licence file is included; add one that suits
your project (for example MIT or Apache-2.0) before sharing or selling it.

Not affiliated with or endorsed by Safaricom. "M-Pesa" and "Daraja" are trademarks of their respective owners.
