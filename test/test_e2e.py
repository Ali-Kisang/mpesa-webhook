#!/usr/bin/env python3
"""
End-to-end tests for mpesa_webhook against a mock Daraja server (no internet needed).

    make test                                  # normal build
    make test-asan                             # sanitizer build
    MPESA_BIN=/path/to/binary python3 test/test_e2e.py

Exit code is 0 only if every check passes.
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mock_daraja import MockDaraja  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
BIN = os.environ.get("MPESA_BIN") or str(ROOT / "mpesa_webhook")
CB_SECRET, ADMIN = "cbsecret", "admintoken"

results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond)))
    print(("  PASS  " if cond else "  FAIL  ") + name + ("" if cond else "   -> " + str(detail)))


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for(fn, timeout=25, step=0.3):
    end = time.time() + timeout
    while time.time() < end:
        try:
            v = fn()
            if v:
                return v
        except Exception:
            pass
        time.sleep(step)
    return None


class Api:
    def __init__(self, port):
        self.base = "http://127.0.0.1:%d" % port

    def call(self, method, path, body=None, admin=True, raw=None):
        data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
        req = urllib.request.Request(self.base + path, data=data, method=method)
        if admin:
            req.add_header("X-Admin-Token", ADMIN)
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                txt = r.read().decode()
                return r.status, (json.loads(txt) if txt else None)
        except urllib.error.HTTPError as e:
            txt = e.read().decode()
            try:
                return e.code, json.loads(txt) if txt else None
            except ValueError:
                return e.code, txt

    def order(self, cid, ref, amount):
        return self.call("POST", "/order", {"checkout_request_id": cid, "order_ref": ref,
                                            "expected_amount": amount})

    def get_order(self, cid):
        return self.call("GET", "/order/" + cid)[1]

    def callback(self, cid, amount, receipt, rc=0):
        if rc == 0:
            body = {"Body": {"stkCallback": {"CheckoutRequestID": cid, "ResultCode": 0,
                    "ResultDesc": "ok", "CallbackMetadata": {"Item": [
                        {"Name": "Amount", "Value": amount},
                        {"Name": "MpesaReceiptNumber", "Value": receipt},
                        {"Name": "PhoneNumber", "Value": 254700000000}]}}}}
        else:
            body = {"Body": {"stkCallback": {"CheckoutRequestID": cid, "ResultCode": rc,
                                             "ResultDesc": "cancelled"}}}
        return self.call("POST", "/mpesa/callback/" + CB_SECRET, body, admin=False)


def main():
    if not os.path.exists(BIN):
        print("Binary not found: %s (run `make` first)" % BIN)
        return 2

    mock_port, port = free_port(), free_port()
    mock = MockDaraja(port=mock_port, callback_delay=1.0).start()
    tmp = tempfile.mkdtemp()
    env = dict(os.environ,
               CALLBACK_SECRET=CB_SECRET, ADMIN_TOKEN=ADMIN, PORT=str(port), BIND_ADDR="127.0.0.1",
               DB_PATH=os.path.join(tmp, "test.db"),
               DARAJA_BASE_URL="http://127.0.0.1:%d" % mock_port,
               DARAJA_CONSUMER_KEY="k", DARAJA_CONSUMER_SECRET="s",
               DARAJA_SHORTCODE="174379", DARAJA_PASSKEY="pk",
               POLL_BASE_SECS="1", POLL_MAX="3", POLL_TICK="1")
    log = open(os.path.join(tmp, "server.log"), "w")
    proc = subprocess.Popen([BIN], env=env, stderr=log, stdout=log)
    api = Api(port)

    try:
        if not wait_for(lambda: api.call("GET", "/report")[0] == 200, timeout=10):
            print("server did not start; log:\n" + open(os.path.join(tmp, "server.log")).read())
            return 2

        print("\n[security]")
        check("wrong callback secret -> 404",
              api.call("POST", "/mpesa/callback/nope", {}, admin=False)[0] == 404)
        check("/report without admin token -> 401", api.call("GET", "/report", admin=False)[0] == 401)
        check("/order without admin token -> 401",
              api.call("POST", "/order", {}, admin=False)[0] == 401)
        check("/order/<id> without admin token -> 401",
              api.call("GET", "/order/x", admin=False)[0] == 401)
        check("bad characters in order id -> 400", api.call("GET", "/order/a%20b")[0] == 400)
        check("unknown order -> 404", api.call("GET", "/order/ws_CO_unknown")[0] == 404)
        check("invalid JSON callback -> 400",
              api.call("POST", "/mpesa/callback/" + CB_SECRET, raw=b"not json", admin=False)[0] == 400)

        print("\n[callback flow]")
        api.order("t_norm", "INV-1", 1500)
        st, _ = api.callback("t_norm", 1500, "RCPT0001")
        o = api.get_order("t_norm")
        check("matching callback -> order paid", o["status"] == "paid", o)
        check("payment details exposed", o["payment"]["receipt"] == "RCPT0001"
              and o["payment"]["amount"] == 1500 and o["payment"]["status"] == "matched", o)
        api.callback("t_norm", 1500, "RCPT0001")
        rep = api.call("GET", "/report")[1]
        matched = [r for r in rep["payments"] if r["status"] == "matched"]
        check("duplicate callback stored once", matched and matched[0]["count"] == 1, rep)

        api.order("t_mis", "INV-2", 1000)
        api.callback("t_mis", 800, "RCPT0002")
        check("wrong amount -> amount_mismatch", api.get_order("t_mis")["status"] == "amount_mismatch")

        api.callback("t_early", 200, "RCPT0003")           # callback before order exists
        rep = api.call("GET", "/report")[1]
        check("callback before order is kept as unmatched",
              any(r["status"] == "unmatched" and r["count"] == 1 for r in rep["payments"]), rep)
        api.order("t_early", "INV-3", 200)
        check("order registered after callback -> paid", api.get_order("t_early")["status"] == "paid")

        api.order("t_cancel", "INV-4", 300)
        api.callback("t_cancel", 0, "", rc=1032)
        check("cancelled callback -> failed", api.get_order("t_cancel")["status"] == "failed")

        print("\n[polling fallback] (takes ~10s)")
        c_ok = mock.register_checkout("success")      # callback lost, payment succeeded
        c_cancel = mock.register_checkout("cancel")   # callback lost, customer cancelled
        c_pend = mock.register_checkout("pending")    # Daraja never answers
        c_cb = mock.register_checkout("success")      # callback arrives normally
        for cid, ref, amt in [(c_ok, "P-OK", 2500), (c_cancel, "P-CANCEL", 100),
                              (c_pend, "P-PEND", 700), (c_cb, "P-CB", 50)]:
            api.order(cid, ref, amt)
        api.callback(c_cb, 50, "RCPTCB01")

        check("lost callback, paid -> paid_unverified",
              wait_for(lambda: api.get_order(c_ok)["status"] == "paid_unverified"),
              api.get_order(c_ok))
        check("lost callback, cancelled -> failed",
              wait_for(lambda: api.get_order(c_cancel)["status"] == "failed"), api.get_order(c_cancel))
        check("never answered -> needs_review",
              wait_for(lambda: api.get_order(c_pend)["status"] == "needs_review"), api.get_order(c_pend))
        listing = api.call("GET", "/orders?status=needs_review")[1]
        check("/orders?status=needs_review lists it",
              any(r["checkout_request_id"] == c_pend for r in listing), listing)
        check("order paid by callback was never polled", mock.queries.get(c_cb, 0) == 0, mock.queries)
        check("pending order polled exactly POLL_MAX times", mock.queries.get(c_pend) == 3, mock.queries)

        print("\n[late callbacks win]")
        api.callback(c_ok, 2500, "RCPTOK01")
        o = api.get_order(c_ok)
        check("late callback upgrades paid_unverified -> paid", o["status"] == "paid", o)
        check("late callback fills in receipt and amount",
              o["payment"]["receipt"] == "RCPTOK01" and o["payment"]["amount"] == 2500
              and o["payment"]["source"] == "callback", o)
        api.callback(c_pend, 700, "RCPTPD01")
        check("late callback rescues needs_review -> paid", api.get_order(c_pend)["status"] == "paid")
        rep = api.call("GET", "/report")[1]
        total_payments = sum(r["count"] for r in rep["payments"])
        # t_norm, t_mis, t_early, t_cancel, c_ok, c_cancel, c_pend, c_cb = 8 rows, no doubles
        check("no double-counted payments", total_payments == 8, rep)

        print("\n[mock sanity]")
        check("every request to Daraja had valid auth and password", mock.bad_requests == 0,
              mock.bad_requests)
        check("OAuth token cached (requested once)", mock.token_requests == 1, mock.token_requests)

    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        mock.stop()
        log.close()

    failed = [n for n, ok in results if not ok]
    print("\n%d checks, %d failed" % (len(results), len(failed)))
    if failed:
        print("server log:\n" + open(os.path.join(tmp, "server.log")).read())
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
