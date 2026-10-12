#!/usr/bin/env node
/*
 * Take an M-Pesa payment from Node.js and track it through mpesa_webhook.
 * Needs Node 18+ (built-in fetch). No npm packages.
 *
 *   export SERVICE_URL=http://localhost:8080
 *   export ADMIN_TOKEN=...
 *   export CALLBACK_URL=https://pay.example.com/mpesa/callback/<CALLBACK_SECRET>
 *   export DARAJA_CONSUMER_KEY=... DARAJA_CONSUMER_SECRET=...
 *   export DARAJA_SHORTCODE=174379 DARAJA_PASSKEY=...
 *   # optional: DARAJA_BASE_URL (defaults to the Safaricom sandbox)
 *
 *   node mpesa_client.js 254708374149 10 INV-1001
 *        phone (2547XXXXXXXX)  amount (whole KES)  your own order reference
 */
"use strict";

const env = (name, def = "") => process.env[name] || def;
const SERVICE_URL = env("SERVICE_URL", "http://localhost:8080").replace(/\/+$/, "");
const ADMIN_TOKEN = env("ADMIN_TOKEN");
const CALLBACK_URL = env("CALLBACK_URL");
const DARAJA_BASE = env("DARAJA_BASE_URL", "https://sandbox.safaricom.co.ke").replace(/\/+$/, "");
const CONSUMER_KEY = env("DARAJA_CONSUMER_KEY");
const CONSUMER_SECRET = env("DARAJA_CONSUMER_SECRET");
const SHORTCODE = env("DARAJA_SHORTCODE");
const PASSKEY = env("DARAJA_PASSKEY");

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** Tiny JSON-over-HTTP helper. Resolves to { status, body }. */
async function request(method, url, body, headers = {}) {
  const res = await fetch(url, {
    method,
    headers: { "Content-Type": "application/json", ...headers },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const text = await res.text();
  let parsed = null;
  try { parsed = text ? JSON.parse(text) : null; } catch { parsed = text; }
  return { status: res.status, body: parsed };
}

// ---- Step 1 & 2: talk to Safaricom (Daraja) ------------------------------------------

async function darajaToken() {
  const basic = Buffer.from(`${CONSUMER_KEY}:${CONSUMER_SECRET}`).toString("base64");
  const { status, body } = await request(
    "GET", `${DARAJA_BASE}/oauth/v1/generate?grant_type=client_credentials`,
    undefined, { Authorization: `Basic ${basic}` });
  if (status !== 200) throw new Error(`Daraja token request failed (${status}): ${JSON.stringify(body)}`);
  return body.access_token;
}

async function stkPush(token, phone, amount, orderRef) {
  // Daraja wants East Africa Time (UTC+3, no daylight saving) as YYYYMMDDHHMMSS.
  const timestamp = new Date(Date.now() + 3 * 3600 * 1000)
    .toISOString().replace(/[-:T]/g, "").slice(0, 14);
  const password = Buffer.from(`${SHORTCODE}${PASSKEY}${timestamp}`).toString("base64");
  const { status, body } = await request("POST", `${DARAJA_BASE}/mpesa/stkpush/v1/processrequest`, {
    BusinessShortCode: SHORTCODE,
    Password: password,
    Timestamp: timestamp,
    TransactionType: "CustomerPayBillOnline",   // till numbers: "CustomerBuyGoodsOnline"
    Amount: amount,
    PartyA: phone,
    PartyB: SHORTCODE,
    PhoneNumber: phone,
    CallBackURL: CALLBACK_URL,
    AccountReference: orderRef,
    TransactionDesc: `Payment ${orderRef}`,
  }, { Authorization: `Bearer ${token}` });
  if (status !== 200 || String(body.ResponseCode) !== "0")
    throw new Error(`STK push rejected (${status}): ${JSON.stringify(body)}`);
  return body.CheckoutRequestID;
}

// ---- Step 3 & 4: talk to mpesa_webhook ------------------------------------------------

const service = (method, path, body) =>
  request(method, SERVICE_URL + path, body, { "X-Admin-Token": ADMIN_TOKEN });

async function registerOrder(checkoutId, orderRef, amount) {
  const { status, body } = await service("POST", "/order", {
    checkout_request_id: checkoutId, order_ref: orderRef, expected_amount: amount });
  if (status !== 200 && status !== 201)
    throw new Error(`could not register order (${status}): ${JSON.stringify(body)}`);
}

/** Poll until the order leaves 'pending'. A real web app would expose this through its
 *  own status endpoint, or push the result to the browser (SSE / WebSocket). */
async function waitForResult(checkoutId, timeoutMs = 120000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const { status, body } = await service("GET", `/order/${checkoutId}`);
    if (status === 200 && body.status !== "pending") return body;
    await sleep(2000);
  }
  return null;
}

async function main() {
  const [phone, amountArg, orderRef] = process.argv.slice(2);
  if (!phone || !amountArg || !orderRef) {
    console.error("usage: node mpesa_client.js <phone 2547XXXXXXXX> <amount KES> <order_ref>");
    process.exit(1);
  }
  const amount = parseInt(amountArg, 10);

  const token = await darajaToken();
  const checkoutId = await stkPush(token, phone, amount, orderRef);
  console.log(`STK push sent, CheckoutRequestID=${checkoutId}. Ask the customer to enter their PIN...`);

  // Register right away. If the callback beats us here, the service still reconciles it.
  await registerOrder(checkoutId, orderRef, amount);

  const order = await waitForResult(checkoutId);
  if (!order) {
    console.log("Still pending after 2 minutes; check /orders?status=pending later.");
    console.log("FINAL STATUS: pending");
    return;
  }
  console.log(JSON.stringify(order, null, 2));
  console.log("FINAL STATUS:", order.status);
}

main().catch((err) => { console.error(err.message); process.exit(1); });
