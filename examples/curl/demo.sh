#!/usr/bin/env bash
# The whole integration contract, using nothing but curl.
# Any language that can make HTTP requests can do exactly these calls.
#
#   SERVICE_URL=http://localhost:8080 ADMIN_TOKEN=admintoken ./demo.sh
#
# (Step 1, sending the STK push, goes to Safaricom and is shown in the language examples.)
set -euo pipefail
SERVICE_URL=${SERVICE_URL:-http://localhost:8080}
ADMIN_TOKEN=${ADMIN_TOKEN:?set ADMIN_TOKEN}
CHECKOUT_ID=${1:-ws_CO_demo_$(date +%s)}   # normally returned by Safaricom's STK push

echo "== 2. register the expected payment (right after the STK push) =="
curl -s -X POST "$SERVICE_URL/order" -H "X-Admin-Token: $ADMIN_TOKEN" \
  -d "{\"checkout_request_id\":\"$CHECKOUT_ID\",\"order_ref\":\"INV-DEMO\",\"expected_amount\":100}"
echo; echo

echo "== 3. ask whether it has been paid (your app does this, or shows it to the customer) =="
curl -s "$SERVICE_URL/order/$CHECKOUT_ID" -H "X-Admin-Token: $ADMIN_TOKEN"
echo; echo

echo "== 4. orders that need a human (callback lost AND Safaricom never answered) =="
curl -s "$SERVICE_URL/orders?status=needs_review" -H "X-Admin-Token: $ADMIN_TOKEN"
echo; echo

echo "== 5. totals for a dashboard or daily report =="
curl -s "$SERVICE_URL/report" -H "X-Admin-Token: $ADMIN_TOKEN"
echo
