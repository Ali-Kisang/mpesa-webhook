#!/usr/bin/env python3
"""
A small mock of the Daraja endpoints that mpesa_webhook talks to:
  GET  /oauth/v1/generate                 (access token)
  POST /mpesa/stkpush/v1/processrequest   (STK Push)
  POST /mpesa/stkpushquery/v1/query       (STK Push Query)

It lets you run and test the whole system WITHOUT Safaricom credentials.

Behaviour is chosen by the LAST DIGIT of the phone number used in the STK push:
  ...1  customer pays, callback delivered to your CallBackURL after ~2s
  ...2  customer pays, callback is LOST (the poller must find the payment)
  ...3  customer cancels, callback is LOST (poller finds the cancellation)
  ...4  Daraja never gives a final answer (order ends up 'needs_review')
  ...5  customer cancels, callback delivered
  other  same as ...1

Run standalone:   python3 test/mock_daraja.py [port]
Import in tests:  from mock_daraja import MockDaraja
"""
import base64
import json
import sys
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class MockDaraja:
    def __init__(self, port=9099, key="k", secret="s", shortcode="174379", passkey="pk",
                 callback_delay=2.0, verbose=False):
        self.port = port
        self.key, self.secret = key, secret
        self.shortcode, self.passkey = shortcode, passkey
        self.callback_delay = callback_delay
        self.verbose = verbose
        self.checkouts = {}          # checkout id -> outcome
        self.queries = {}            # checkout id -> number of query calls
        self.token_requests = 0
        self.bad_requests = 0        # requests with wrong auth or password
        self._n = 0
        self._lock = threading.Lock()
        self._server = None

    # ---- test helpers ----
    def register_checkout(self, outcome):
        """outcome: success | cancel | pending. Returns a new CheckoutRequestID."""
        with self._lock:
            self._n += 1
            cid = "ws_CO_mock_%04d" % self._n
            self.checkouts[cid] = outcome
        return cid

    def start(self):
        mock = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                if mock.verbose:
                    sys.stderr.write("[mock] " + (args[0] % args[1:]) + "\n")

            def _send(self, code, obj):
                body = json.dumps(obj).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                want = "Basic " + base64.b64encode(
                    ("%s:%s" % (mock.key, mock.secret)).encode()).decode()
                mock.token_requests += 1
                if self.headers.get("Authorization") != want:
                    mock.bad_requests += 1
                    return self._send(400, {"errorMessage": "Bad credentials"})
                self._send(200, {"access_token": "tok123", "expires_in": "3599"})

            def do_POST(self):
                n = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(n) or b"{}")
                if self.headers.get("Authorization") != "Bearer tok123":
                    mock.bad_requests += 1
                    return self._send(401, {"errorMessage": "Invalid Access Token"})
                try:
                    pw = base64.b64decode(body["Password"]).decode()
                except Exception:
                    pw = ""
                if pw != mock.shortcode + mock.passkey + body.get("Timestamp", ""):
                    mock.bad_requests += 1
                    return self._send(400, {"errorMessage": "Bad Password"})

                if self.path.endswith("/stkpush/v1/processrequest"):
                    return self._stk_push(body)
                if self.path.endswith("/stkpushquery/v1/query"):
                    return self._stk_query(body)
                self._send(404, {"errorMessage": "unknown endpoint"})

            def _stk_push(self, body):
                phone = str(body.get("PhoneNumber", ""))
                digit = phone[-1:] 
                outcome = {"2": "success", "3": "cancel", "4": "pending", "5": "cancel"}.get(digit, "success")
                cid = mock.register_checkout(outcome)
                if digit in ("1", "5") or digit not in ("2", "3", "4"):
                    threading.Timer(mock.callback_delay, mock._deliver_callback,
                                    args=(cid, outcome, body)).start()
                self._send(200, {
                    "MerchantRequestID": "mrid-" + cid[-4:],
                    "CheckoutRequestID": cid,
                    "ResponseCode": "0",
                    "ResponseDescription": "Success. Request accepted for processing",
                    "CustomerMessage": "Success. Request accepted for processing"})

            def _stk_query(self, body):
                cid = body.get("CheckoutRequestID", "")
                mock.queries[cid] = mock.queries.get(cid, 0) + 1
                outcome = mock.checkouts.get(cid)
                if outcome is None:                       # ids not created through the mock
                    outcome = "success" if "OK" in cid else "cancel" if "CANCEL" in cid else "pending"
                if outcome == "success":
                    return self._send(200, {"ResponseCode": "0", "CheckoutRequestID": cid,
                        "ResultCode": "0", "ResultDesc": "The service request is processed successfully."})
                if outcome == "cancel":
                    return self._send(200, {"ResponseCode": "0", "CheckoutRequestID": cid,
                        "ResultCode": "1032", "ResultDesc": "Request cancelled by user"})
                self._send(500, {"requestId": "x", "errorCode": "500.001.1001",
                                 "errorMessage": "The transaction is being processed"})

        self._server = ThreadingHTTPServer(("127.0.0.1", self.port), Handler)
        threading.Thread(target=self._server.serve_forever, daemon=True).start()
        return self

    def stop(self):
        if self._server:
            self._server.shutdown()

    def _deliver_callback(self, cid, outcome, push_body):
        url = push_body.get("CallBackURL")
        if not url:
            return
        if outcome == "success":
            cb = {"Body": {"stkCallback": {
                "MerchantRequestID": "mrid-" + cid[-4:], "CheckoutRequestID": cid,
                "ResultCode": 0, "ResultDesc": "The service request is processed successfully.",
                "CallbackMetadata": {"Item": [
                    {"Name": "Amount", "Value": push_body.get("Amount")},
                    {"Name": "MpesaReceiptNumber", "Value": "MOCK" + cid[-4:]},
                    {"Name": "TransactionDate", "Value": int(time.strftime("%Y%m%d%H%M%S"))},
                    {"Name": "PhoneNumber", "Value": int(push_body.get("PhoneNumber"))}]}}}}
        else:
            cb = {"Body": {"stkCallback": {
                "MerchantRequestID": "mrid-" + cid[-4:], "CheckoutRequestID": cid,
                "ResultCode": 1032, "ResultDesc": "Request cancelled by user"}}}
        try:
            req = urllib.request.Request(url, data=json.dumps(cb).encode(),
                                         headers={"Content-Type": "application/json"})
            urllib.request.urlopen(req, timeout=5).read()
        except Exception as e:                            # callback target unreachable
            sys.stderr.write("[mock] callback delivery failed: %s\n" % e)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9099
    MockDaraja(port=port, verbose=True).start()
    print("Mock Daraja listening on http://127.0.0.1:%d  (key=k secret=s shortcode=174379 passkey=pk)" % port)
    print(__doc__.split("Behaviour")[1].split("Run standalone")[0])
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
