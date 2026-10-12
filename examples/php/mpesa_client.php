<?php
/*
 * Take an M-Pesa payment from PHP and track it through mpesa_webhook.
 * Needs PHP 7.4+ with the curl extension (php-curl). No Composer packages.
 *
 *   export SERVICE_URL=http://localhost:8080
 *   export ADMIN_TOKEN=...
 *   export CALLBACK_URL=https://pay.example.com/mpesa/callback/<CALLBACK_SECRET>
 *   export DARAJA_CONSUMER_KEY=... DARAJA_CONSUMER_SECRET=...
 *   export DARAJA_SHORTCODE=174379 DARAJA_PASSKEY=...
 *   # optional: DARAJA_BASE_URL (defaults to the Safaricom sandbox)
 *
 *   php mpesa_client.php 254708374149 10 INV-1001
 *       phone (2547XXXXXXXX)  amount (whole KES)  your own order reference
 *
 * In a framework (Laravel, WordPress, ...) you would call these same functions from a
 * controller or a queued job instead of the command line.
 */

function env(string $name, string $default = ""): string {
    $v = getenv($name);
    return ($v === false || $v === "") ? $default : $v;
}

$SERVICE_URL = rtrim(env("SERVICE_URL", "http://localhost:8080"), "/");
$ADMIN_TOKEN = env("ADMIN_TOKEN");
$CALLBACK_URL = env("CALLBACK_URL");
$DARAJA_BASE = rtrim(env("DARAJA_BASE_URL", "https://sandbox.safaricom.co.ke"), "/");
$CONSUMER_KEY = env("DARAJA_CONSUMER_KEY");
$CONSUMER_SECRET = env("DARAJA_CONSUMER_SECRET");
$SHORTCODE = env("DARAJA_SHORTCODE");
$PASSKEY = env("DARAJA_PASSKEY");

/** Tiny JSON-over-HTTP helper. Returns [status, decoded_json_or_text]. */
function request(string $method, string $url, ?array $body = null, array $headers = []): array {
    $ch = curl_init($url);
    curl_setopt_array($ch, [
        CURLOPT_CUSTOMREQUEST => $method,
        CURLOPT_RETURNTRANSFER => true,
        CURLOPT_TIMEOUT => 30,
        CURLOPT_HTTPHEADER => array_merge(["Content-Type: application/json"], $headers),
    ]);
    if ($body !== null) {
        curl_setopt($ch, CURLOPT_POSTFIELDS, json_encode($body));
    }
    $raw = curl_exec($ch);
    if ($raw === false) {
        throw new RuntimeException("HTTP error: " . curl_error($ch));
    }
    $status = curl_getinfo($ch, CURLINFO_RESPONSE_CODE);
    curl_close($ch);
    $json = json_decode($raw, true);
    return [$status, $json === null ? $raw : $json];
}

// ---- Step 1 & 2: talk to Safaricom (Daraja) ------------------------------------------

function daraja_token(): string {
    global $DARAJA_BASE, $CONSUMER_KEY, $CONSUMER_SECRET;
    $basic = base64_encode("$CONSUMER_KEY:$CONSUMER_SECRET");
    [$status, $body] = request("GET", "$DARAJA_BASE/oauth/v1/generate?grant_type=client_credentials",
        null, ["Authorization: Basic $basic"]);
    if ($status !== 200) {
        throw new RuntimeException("Daraja token request failed ($status): " . json_encode($body));
    }
    return $body["access_token"];
}

function stk_push(string $token, string $phone, int $amount, string $orderRef): string {
    global $DARAJA_BASE, $SHORTCODE, $PASSKEY, $CALLBACK_URL;
    // Daraja wants East Africa Time (UTC+3, no daylight saving) as YYYYMMDDHHMMSS.
    $timestamp = gmdate("YmdHis", time() + 3 * 3600);
    $password = base64_encode($SHORTCODE . $PASSKEY . $timestamp);
    [$status, $body] = request("POST", "$DARAJA_BASE/mpesa/stkpush/v1/processrequest", [
        "BusinessShortCode" => $SHORTCODE,
        "Password" => $password,
        "Timestamp" => $timestamp,
        "TransactionType" => "CustomerPayBillOnline",   // till numbers: "CustomerBuyGoodsOnline"
        "Amount" => $amount,
        "PartyA" => $phone,
        "PartyB" => $SHORTCODE,
        "PhoneNumber" => $phone,
        "CallBackURL" => $CALLBACK_URL,
        "AccountReference" => $orderRef,
        "TransactionDesc" => "Payment $orderRef",
    ], ["Authorization: Bearer $token"]);
    if ($status !== 200 || (string)($body["ResponseCode"] ?? "") !== "0") {
        throw new RuntimeException("STK push rejected ($status): " . json_encode($body));
    }
    return $body["CheckoutRequestID"];
}

// ---- Step 3 & 4: talk to mpesa_webhook ------------------------------------------------

function service(string $method, string $path, ?array $body = null): array {
    global $SERVICE_URL, $ADMIN_TOKEN;
    return request($method, $SERVICE_URL . $path, $body, ["X-Admin-Token: $ADMIN_TOKEN"]);
}

function register_order(string $checkoutId, string $orderRef, int $amount): void {
    [$status, $body] = service("POST", "/order", [
        "checkout_request_id" => $checkoutId,
        "order_ref" => $orderRef,
        "expected_amount" => $amount,
    ]);
    if ($status !== 200 && $status !== 201) {
        throw new RuntimeException("could not register order ($status): " . json_encode($body));
    }
}

/** Poll until the order leaves 'pending'. A real site would expose this through its own
 *  AJAX status endpoint that the checkout page calls every few seconds. */
function wait_for_result(string $checkoutId, int $timeout = 120): ?array {
    $deadline = time() + $timeout;
    while (time() < $deadline) {
        [$status, $order] = service("GET", "/order/$checkoutId");
        if ($status === 200 && $order["status"] !== "pending") {
            return $order;
        }
        sleep(2);
    }
    return null;
}

if (PHP_SAPI === "cli" && realpath($_SERVER["SCRIPT_FILENAME"]) === __FILE__) {
    if ($argc !== 4) {
        fwrite(STDERR, "usage: php mpesa_client.php <phone 2547XXXXXXXX> <amount KES> <order_ref>\n");
        exit(1);
    }
    [, $phone, $amountArg, $orderRef] = $argv;
    $amount = (int)$amountArg;

    $token = daraja_token();
    $checkoutId = stk_push($token, $phone, $amount, $orderRef);
    echo "STK push sent, CheckoutRequestID=$checkoutId. Ask the customer to enter their PIN...\n";

    // Register right away. If the callback beats us here, the service still reconciles it.
    register_order($checkoutId, $orderRef, $amount);

    $order = wait_for_result($checkoutId);
    if ($order === null) {
        echo "Still pending after 2 minutes; check /orders?status=pending later.\n";
        echo "FINAL STATUS: pending\n";
        exit(0);
    }
    echo json_encode($order, JSON_PRETTY_PRINT) . "\n";
    echo "FINAL STATUS: {$order["status"]}\n";
}
