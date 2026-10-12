import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.OffsetDateTime;
import java.time.ZoneOffset;
import java.time.format.DateTimeFormatter;
import java.util.Base64;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Take an M-Pesa payment from Java and track it through mpesa_webhook.
 * Needs Java 11+ (built-in HttpClient). Run directly, no build step:
 *
 *   export SERVICE_URL=http://localhost:8080
 *   export ADMIN_TOKEN=...
 *   export CALLBACK_URL=https://pay.example.com/mpesa/callback/<CALLBACK_SECRET>
 *   export DARAJA_CONSUMER_KEY=... DARAJA_CONSUMER_SECRET=...
 *   export DARAJA_SHORTCODE=174379 DARAJA_PASSKEY=...
 *   # optional: DARAJA_BASE_URL (defaults to the Safaricom sandbox)
 *
 *   java MpesaClient.java 254708374149 10 INV-1001
 *
 * To stay dependency-free this example extracts JSON fields with a regex. In a real
 * project use Jackson or Gson instead.
 */
public class MpesaClient {
    static String env(String name, String def) {
        String v = System.getenv(name);
        return (v == null || v.isEmpty()) ? def : v;
    }

    static final String SERVICE_URL = env("SERVICE_URL", "http://localhost:8080").replaceAll("/+$", "");
    static final String ADMIN_TOKEN = env("ADMIN_TOKEN", "");
    static final String CALLBACK_URL = env("CALLBACK_URL", "");
    static final String DARAJA_BASE = env("DARAJA_BASE_URL", "https://sandbox.safaricom.co.ke").replaceAll("/+$", "");
    static final String CONSUMER_KEY = env("DARAJA_CONSUMER_KEY", "");
    static final String CONSUMER_SECRET = env("DARAJA_CONSUMER_SECRET", "");
    static final String SHORTCODE = env("DARAJA_SHORTCODE", "");
    static final String PASSKEY = env("DARAJA_PASSKEY", "");

    static final HttpClient HTTP = HttpClient.newBuilder().connectTimeout(Duration.ofSeconds(30)).build();

    static class Resp {
        final int status;
        final String body;
        Resp(int status, String body) { this.status = status; this.body = body; }
    }

    /** Tiny HTTP helper. headers is a flat array: name1, value1, name2, value2, ... */
    static Resp request(String method, String url, String jsonBody, String... headers) throws Exception {
        HttpRequest.Builder b = HttpRequest.newBuilder(URI.create(url))
                .timeout(Duration.ofSeconds(30))
                .header("Content-Type", "application/json")
                .method(method, jsonBody == null
                        ? HttpRequest.BodyPublishers.noBody()
                        : HttpRequest.BodyPublishers.ofString(jsonBody));
        for (int i = 0; i + 1 < headers.length; i += 2) b.header(headers[i], headers[i + 1]);
        HttpResponse<String> r = HTTP.send(b.build(), HttpResponse.BodyHandlers.ofString());
        return new Resp(r.statusCode(), r.body());
    }

    /** First occurrence of "name":"value" in a JSON string, or null. */
    static String field(String json, String name) {
        Matcher m = Pattern.compile("\"" + Pattern.quote(name) + "\"\\s*:\\s*\"([^\"]*)\"").matcher(json);
        return m.find() ? m.group(1) : null;
    }

    static String esc(String s) { return s.replace("\\", "\\\\").replace("\"", "\\\""); }

    // ---- Step 1 & 2: talk to Safaricom (Daraja) --------------------------------------

    static String darajaToken() throws Exception {
        String basic = Base64.getEncoder().encodeToString(
                (CONSUMER_KEY + ":" + CONSUMER_SECRET).getBytes(StandardCharsets.UTF_8));
        Resp r = request("GET", DARAJA_BASE + "/oauth/v1/generate?grant_type=client_credentials",
                null, "Authorization", "Basic " + basic);
        if (r.status != 200) throw new RuntimeException("Daraja token request failed (" + r.status + "): " + r.body);
        return field(r.body, "access_token");
    }

    static String stkPush(String token, String phone, int amount, String orderRef) throws Exception {
        // Daraja wants East Africa Time (UTC+3, no daylight saving) as YYYYMMDDHHMMSS.
        String timestamp = OffsetDateTime.now(ZoneOffset.ofHours(3))
                .format(DateTimeFormatter.ofPattern("yyyyMMddHHmmss"));
        String password = Base64.getEncoder().encodeToString(
                (SHORTCODE + PASSKEY + timestamp).getBytes(StandardCharsets.UTF_8));
        String json = String.format(
                "{\"BusinessShortCode\":\"%s\",\"Password\":\"%s\",\"Timestamp\":\"%s\","
                + "\"TransactionType\":\"CustomerPayBillOnline\",\"Amount\":%d,\"PartyA\":\"%s\","
                + "\"PartyB\":\"%s\",\"PhoneNumber\":\"%s\",\"CallBackURL\":\"%s\","
                + "\"AccountReference\":\"%s\",\"TransactionDesc\":\"Payment %s\"}",
                esc(SHORTCODE), password, timestamp, amount, esc(phone), esc(SHORTCODE),
                esc(phone), esc(CALLBACK_URL), esc(orderRef), esc(orderRef));
        Resp r = request("POST", DARAJA_BASE + "/mpesa/stkpush/v1/processrequest", json,
                "Authorization", "Bearer " + token);
        if (r.status != 200 || !"0".equals(field(r.body, "ResponseCode")))
            throw new RuntimeException("STK push rejected (" + r.status + "): " + r.body);
        return field(r.body, "CheckoutRequestID");
    }

    // ---- Step 3 & 4: talk to mpesa_webhook -------------------------------------------

    static Resp service(String method, String path, String json) throws Exception {
        return request(method, SERVICE_URL + path, json, "X-Admin-Token", ADMIN_TOKEN);
    }

    static void registerOrder(String checkoutId, String orderRef, int amount) throws Exception {
        String json = String.format(
                "{\"checkout_request_id\":\"%s\",\"order_ref\":\"%s\",\"expected_amount\":%d}",
                esc(checkoutId), esc(orderRef), amount);
        Resp r = service("POST", "/order", json);
        if (r.status != 200 && r.status != 201)
            throw new RuntimeException("could not register order (" + r.status + "): " + r.body);
    }

    /** Poll until the order leaves "pending". Returns the order JSON, or null on timeout. */
    static String waitForResult(String checkoutId, int timeoutSeconds) throws Exception {
        long deadline = System.currentTimeMillis() + timeoutSeconds * 1000L;
        while (System.currentTimeMillis() < deadline) {
            Resp r = service("GET", "/order/" + checkoutId, null);
            // The order's own "status" comes before the nested payment object in the JSON.
            if (r.status == 200 && !"pending".equals(field(r.body, "status"))) return r.body;
            Thread.sleep(2000);
        }
        return null;
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 3) {
            System.err.println("usage: java MpesaClient.java <phone 2547XXXXXXXX> <amount KES> <order_ref>");
            System.exit(1);
        }
        String phone = args[0], orderRef = args[2];
        int amount = Integer.parseInt(args[1]);

        String token = darajaToken();
        String checkoutId = stkPush(token, phone, amount, orderRef);
        System.out.println("STK push sent, CheckoutRequestID=" + checkoutId
                + ". Ask the customer to enter their PIN...");

        // Register right away. If the callback beats us here, the service still reconciles it.
        registerOrder(checkoutId, orderRef, amount);

        String order = waitForResult(checkoutId, 120);
        if (order == null) {
            System.out.println("Still pending after 2 minutes; check /orders?status=pending later.");
            System.out.println("FINAL STATUS: pending");
            return;
        }
        System.out.println(order);
        System.out.println("FINAL STATUS: " + field(order, "status"));
    }
}
