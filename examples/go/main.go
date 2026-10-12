// Take an M-Pesa payment from Go and track it through mpesa_webhook.
// Standard library only (Go 1.21+).
//
//	export SERVICE_URL=http://localhost:8080
//	export ADMIN_TOKEN=...
//	export CALLBACK_URL=https://pay.example.com/mpesa/callback/<CALLBACK_SECRET>
//	export DARAJA_CONSUMER_KEY=... DARAJA_CONSUMER_SECRET=...
//	export DARAJA_SHORTCODE=174379 DARAJA_PASSKEY=...
//	# optional: DARAJA_BASE_URL (defaults to the Safaricom sandbox)
//
//	go run . 254708374149 10 INV-1001
//	         phone (2547XXXXXXXX)  amount (whole KES)  your own order reference
package main

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"
)

func env(name, def string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return def
}

var (
	serviceURL     = strings.TrimRight(env("SERVICE_URL", "http://localhost:8080"), "/")
	adminToken     = env("ADMIN_TOKEN", "")
	callbackURL    = env("CALLBACK_URL", "")
	darajaBase     = strings.TrimRight(env("DARAJA_BASE_URL", "https://sandbox.safaricom.co.ke"), "/")
	consumerKey    = env("DARAJA_CONSUMER_KEY", "")
	consumerSecret = env("DARAJA_CONSUMER_SECRET", "")
	shortcode      = env("DARAJA_SHORTCODE", "")
	passkey        = env("DARAJA_PASSKEY", "")
	httpClient     = &http.Client{Timeout: 30 * time.Second}
)

type obj = map[string]any

// request is a tiny JSON-over-HTTP helper. It returns the status code and the decoded JSON
// (or the raw text if the body is not JSON).
func request(method, url string, body any, headers map[string]string) (int, any, error) {
	var rdr io.Reader
	if body != nil {
		b, err := json.Marshal(body)
		if err != nil {
			return 0, nil, err
		}
		rdr = bytes.NewReader(b)
	}
	req, err := http.NewRequest(method, url, rdr)
	if err != nil {
		return 0, nil, err
	}
	req.Header.Set("Content-Type", "application/json")
	for k, v := range headers {
		req.Header.Set(k, v)
	}
	resp, err := httpClient.Do(req)
	if err != nil {
		return 0, nil, err
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(resp.Body)
	var parsed any
	if err := json.Unmarshal(raw, &parsed); err != nil {
		parsed = string(raw)
	}
	return resp.StatusCode, parsed, nil
}

// str reads a string field from a decoded JSON object ("" if missing).
func str(v any, key string) string {
	if m, ok := v.(obj); ok {
		if s, ok := m[key].(string); ok {
			return s
		}
	}
	return ""
}

// ---- Step 1 & 2: talk to Safaricom (Daraja) ------------------------------------------

func darajaToken() (string, error) {
	basic := base64.StdEncoding.EncodeToString([]byte(consumerKey + ":" + consumerSecret))
	status, body, err := request("GET", darajaBase+"/oauth/v1/generate?grant_type=client_credentials",
		nil, map[string]string{"Authorization": "Basic " + basic})
	if err != nil {
		return "", err
	}
	if status != 200 {
		return "", fmt.Errorf("Daraja token request failed (%d): %v", status, body)
	}
	return str(body, "access_token"), nil
}

func stkPush(token, phone string, amount int, orderRef string) (string, error) {
	// Daraja wants East Africa Time (UTC+3, no daylight saving) as YYYYMMDDHHMMSS.
	timestamp := time.Now().UTC().Add(3 * time.Hour).Format("20060102150405")
	password := base64.StdEncoding.EncodeToString([]byte(shortcode + passkey + timestamp))
	status, body, err := request("POST", darajaBase+"/mpesa/stkpush/v1/processrequest", obj{
		"BusinessShortCode": shortcode,
		"Password":          password,
		"Timestamp":         timestamp,
		"TransactionType":   "CustomerPayBillOnline", // till numbers: "CustomerBuyGoodsOnline"
		"Amount":            amount,
		"PartyA":            phone,
		"PartyB":            shortcode,
		"PhoneNumber":       phone,
		"CallBackURL":       callbackURL,
		"AccountReference":  orderRef,
		"TransactionDesc":   "Payment " + orderRef,
	}, map[string]string{"Authorization": "Bearer " + token})
	if err != nil {
		return "", err
	}
	if status != 200 || str(body, "ResponseCode") != "0" {
		return "", fmt.Errorf("STK push rejected (%d): %v", status, body)
	}
	return str(body, "CheckoutRequestID"), nil
}

// ---- Step 3 & 4: talk to mpesa_webhook ------------------------------------------------

func service(method, path string, body any) (int, any, error) {
	return request(method, serviceURL+path, body, map[string]string{"X-Admin-Token": adminToken})
}

func registerOrder(checkoutID, orderRef string, amount int) error {
	status, body, err := service("POST", "/order", obj{
		"checkout_request_id": checkoutID,
		"order_ref":           orderRef,
		"expected_amount":     amount,
	})
	if err != nil {
		return err
	}
	if status != 200 && status != 201 {
		return fmt.Errorf("could not register order (%d): %v", status, body)
	}
	return nil
}

// waitForResult polls until the order leaves "pending". A real service would expose this
// through its own status endpoint, or push the result to the client.
func waitForResult(checkoutID string, timeout time.Duration) (any, error) {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		status, order, err := service("GET", "/order/"+checkoutID, nil)
		if err == nil && status == 200 && str(order, "status") != "pending" {
			return order, nil
		}
		time.Sleep(2 * time.Second)
	}
	return nil, nil
}

func fail(err error) {
	fmt.Fprintln(os.Stderr, err)
	os.Exit(1)
}

func main() {
	if len(os.Args) != 4 {
		fmt.Fprintln(os.Stderr, "usage: go run . <phone 2547XXXXXXXX> <amount KES> <order_ref>")
		os.Exit(1)
	}
	phone, orderRef := os.Args[1], os.Args[3]
	amount, err := strconv.Atoi(os.Args[2])
	if err != nil {
		fail(err)
	}

	token, err := darajaToken()
	if err != nil {
		fail(err)
	}
	checkoutID, err := stkPush(token, phone, amount, orderRef)
	if err != nil {
		fail(err)
	}
	fmt.Printf("STK push sent, CheckoutRequestID=%s. Ask the customer to enter their PIN...\n", checkoutID)

	// Register right away. If the callback beats us here, the service still reconciles it.
	if err := registerOrder(checkoutID, orderRef, amount); err != nil {
		fail(err)
	}

	order, err := waitForResult(checkoutID, 2*time.Minute)
	if err != nil {
		fail(err)
	}
	if order == nil {
		fmt.Println("Still pending after 2 minutes; check /orders?status=pending later.")
		fmt.Println("FINAL STATUS: pending")
		return
	}
	pretty, _ := json.MarshalIndent(order, "", "  ")
	fmt.Println(string(pretty))
	fmt.Println("FINAL STATUS:", str(order, "status"))
}
