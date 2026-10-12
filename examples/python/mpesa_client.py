#!/usr/bin/env python3
"""
Take an M-Pesa payment from Python and track it through mpesa_webhook.
Standard library only (Python 3.8+).

    export SERVICE_URL=http://localhost:8080          # where mpesa_webhook runs
    export ADMIN_TOKEN=...                            # same as the server's ADMIN_TOKEN
    export CALLBACK_URL=https://pay.example.com/mpesa/callback/<CALLBACK_SECRET>
    export DARAJA_CONSUMER_KEY=... DARAJA_CONSUMER_SECRET=...
    export DARAJA_SHORTCODE=174379 DARAJA_PASSKEY=...
    # optional: DARAJA_BASE_URL (defaults to the Safaricom sandbox)

    python3 mpesa_client.py 254708374149 10 INV-1001
         phone (2547XXXXXXXX)  amount (whole KES)  your own order reference
"""
import base64
import json
import os
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone

SERVICE_URL = os.environ.get("SERVICE_URL", "http://localhost:8080").rstrip("/")
ADMIN_TOKEN = os.environ.get("ADMIN_TOKEN", "")
CALLBACK_URL = os.environ.get("CALLBACK_URL", "")
DARAJA_BASE = os.environ.get("DARAJA_BASE_URL", "https://sandbox.safaricom.co.ke").rstrip("/")
CONSUMER_KEY = os.environ.get("DARAJA_CONSUMER_KEY", "")
CONSUMER_SECRET = os.environ.get("DARAJA_CONSUMER_SECRET", "")
SHORTCODE = os.environ.get("DARAJA_SHORTCODE", "")
PASSKEY = os.environ.get("DARAJA_PASSKEY", "")


def request(method, url, body=None, headers=None):
    """Tiny JSON-over-HTTP helper. Returns (status_code, parsed_json_or_text)."""
    data = None if body is None else json.dumps(body).encode()
    hdrs = {"Content-Type": "application/json"}
    hdrs.update(headers or {})
    req = urllib.request.Request(url, data=data, method=method, headers=hdrs)
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            raw = resp.read()
            return resp.status, (json.loads(raw) if raw else None)
    except urllib.error.HTTPError as err:
        raw = err.read()
        try:
            return err.code, json.loads(raw)
        except ValueError:
            return err.code, raw.decode(errors="replace")


# ---- Step 1 & 2: talk to Safaricom (Daraja) -------------------------------------------

def daraja_token():
    basic = base64.b64encode(f"{CONSUMER_KEY}:{CONSUMER_SECRET}".encode()).decode()
    status, body = request("GET", f"{DARAJA_BASE}/oauth/v1/generate?grant_type=client_credentials",
                           headers={"Authorization": f"Basic {basic}"})
    if status != 200:
        raise RuntimeError(f"Daraja token request failed ({status}): {body}")
    return body["access_token"]


def stk_push(token, phone, amount, order_ref):
    # Daraja wants East Africa Time (UTC+3, no daylight saving) in YYYYMMDDHHMMSS form.
    timestamp = datetime.now(timezone(timedelta(hours=3))).strftime("%Y%m%d%H%M%S")
    password = base64.b64encode(f"{SHORTCODE}{PASSKEY}{timestamp}".encode()).decode()
    status, body = request("POST", f"{DARAJA_BASE}/mpesa/stkpush/v1/processrequest", {
        "BusinessShortCode": SHORTCODE,
        "Password": password,
        "Timestamp": timestamp,
        "TransactionType": "CustomerPayBillOnline",   # till numbers: "CustomerBuyGoodsOnline"
        "Amount": amount,
        "PartyA": phone,
        "PartyB": SHORTCODE,
        "PhoneNumber": phone,
        "CallBackURL": CALLBACK_URL,
        "AccountReference": order_ref,
        "TransactionDesc": f"Payment {order_ref}",
    }, headers={"Authorization": f"Bearer {token}"})
    if status != 200 or str(body.get("ResponseCode")) != "0":
        raise RuntimeError(f"STK push rejected ({status}): {body}")
    return body["CheckoutRequestID"]


# ---- Step 3 & 4: talk to mpesa_webhook ------------------------------------------------

def service(method, path, body=None):
    return request(method, SERVICE_URL + path, body, headers={"X-Admin-Token": ADMIN_TOKEN})


def register_order(checkout_id, order_ref, amount):
    status, body = service("POST", "/order", {"checkout_request_id": checkout_id,
                                              "order_ref": order_ref, "expected_amount": amount})
    if status not in (200, 201):
        raise RuntimeError(f"could not register order ({status}): {body}")


def wait_for_result(checkout_id, timeout=120):
    """Poll the service until the order leaves 'pending'. Your web app would do this
    from its own status endpoint (or push the result to the browser)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        status, order = service("GET", f"/order/{checkout_id}")
        if status == 200 and order["status"] != "pending":
            return order
        time.sleep(2)
    return None


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    phone, amount, order_ref = sys.argv[1], int(sys.argv[2]), sys.argv[3]

    token = daraja_token()
    checkout_id = stk_push(token, phone, amount, order_ref)
    print(f"STK push sent, CheckoutRequestID={checkout_id}. Ask the customer to enter their PIN...")

    # Register right away. If the callback beats us here, the service still reconciles it.
    register_order(checkout_id, order_ref, amount)

    order = wait_for_result(checkout_id)
    if order is None:
        print("Still pending after 2 minutes; check /orders?status=pending later.")
        print("FINAL STATUS: pending")
        return
    print(json.dumps(order, indent=2))
    print("FINAL STATUS:", order["status"])


if __name__ == "__main__":
    main()
