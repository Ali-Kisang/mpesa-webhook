#!/usr/bin/env bash
# Runs every language example in examples/ against the mock Daraja server and a real
# mpesa_webhook process. Languages whose runtime is not installed are skipped.
#
#   make && bash test/test_examples.sh
#
# For each language it runs two payments:
#   phone ...1  the callback is delivered       -> expected final status: paid
#   phone ...2  the callback is lost            -> expected final status: paid_unverified
#               (the polling fallback finds the payment)
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${MPESA_BIN:-$ROOT/mpesa_webhook}
MOCK_PORT=${MOCK_PORT:-9199}
PORT=${PORT:-8180}
TMP=$(mktemp -d)

[ -x "$BIN" ] || { echo "Build first: make (binary not found at $BIN)"; exit 2; }

python3 "$ROOT/test/mock_daraja.py" "$MOCK_PORT" >"$TMP/mock.log" 2>&1 &
MOCK_PID=$!
CALLBACK_SECRET=cbsecret ADMIN_TOKEN=admintoken PORT=$PORT BIND_ADDR=127.0.0.1 \
DB_PATH="$TMP/demo.db" DARAJA_BASE_URL="http://127.0.0.1:$MOCK_PORT" \
DARAJA_CONSUMER_KEY=k DARAJA_CONSUMER_SECRET=s DARAJA_SHORTCODE=174379 DARAJA_PASSKEY=pk \
POLL_BASE_SECS=5 POLL_MAX=4 POLL_TICK=1 "$BIN" >"$TMP/server.log" 2>&1 &
SRV_PID=$!
trap 'kill $MOCK_PID $SRV_PID 2>/dev/null' EXIT
sleep 1.5

export SERVICE_URL="http://127.0.0.1:$PORT" ADMIN_TOKEN=admintoken
export CALLBACK_URL="http://127.0.0.1:$PORT/mpesa/callback/cbsecret"
export DARAJA_BASE_URL="http://127.0.0.1:$MOCK_PORT" DARAJA_CONSUMER_KEY=k DARAJA_CONSUMER_SECRET=s
export DARAJA_SHORTCODE=174379 DARAJA_PASSKEY=pk

failed=0; ran=0
run() {   # run <label> <expected status> <phone> <ref> <command...>
  local label=$1 expect=$2 phone=$3 ref=$4; shift 4
  local out; out=$("$@" "$phone" 25 "$ref" 2>&1)
  ran=$((ran+1))
  if echo "$out" | grep -q "^FINAL STATUS: $expect$"; then
    echo "  PASS  $label ($expect)"
  else
    echo "  FAIL  $label (wanted $expect)"; echo "$out" | tail -6 | sed 's/^/        /'; failed=$((failed+1))
  fi
}

have() { command -v "$1" >/dev/null 2>&1 || { echo "  skip  $2 ($1 not installed)"; return 1; }; }

echo "python";  have python3 python && {
  run "python, callback delivered" paid            254700000001 PY-1 python3 "$ROOT/examples/python/mpesa_client.py"
  run "python, callback lost"      paid_unverified 254700000002 PY-2 python3 "$ROOT/examples/python/mpesa_client.py"; }
echo "node";    have node node && {
  run "node, callback delivered"   paid            254700000001 JS-1 node "$ROOT/examples/node/mpesa_client.js"
  run "node, callback lost"        paid_unverified 254700000002 JS-2 node "$ROOT/examples/node/mpesa_client.js"; }
echo "php";     have php php && {
  run "php, callback delivered"    paid            254700000001 PHP-1 php "$ROOT/examples/php/mpesa_client.php"
  run "php, callback lost"         paid_unverified 254700000002 PHP-2 php "$ROOT/examples/php/mpesa_client.php"; }
echo "go";      have go go && {
  ( cd "$ROOT/examples/go" && go build -o "$TMP/goclient" . ) || { echo "  FAIL  go build"; failed=$((failed+1)); }
  [ -x "$TMP/goclient" ] && {
  run "go, callback delivered"     paid            254700000001 GO-1 "$TMP/goclient"
  run "go, callback lost"          paid_unverified 254700000002 GO-2 "$TMP/goclient"; }; }
echo "java";    have java java && {
  run "java, callback delivered"   paid            254700000001 JV-1 java "$ROOT/examples/java/MpesaClient.java"
  run "java, callback lost"        paid_unverified 254700000002 JV-2 java "$ROOT/examples/java/MpesaClient.java"; }

echo; echo "$ran example runs, $failed failed"
[ $failed -eq 0 ] || { echo "--- server log ---"; cat "$TMP/server.log"; exit 1; }
