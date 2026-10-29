#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>
#include "secrets.h"

#define ENABLE_DIAG 0

#define TRIG_PIN  4
#define ECHO_PIN  5
#define LED_PIN         6    
#define NUM_LEDS        8
#define LED_BRIGHTNESS  40     

static const char*    GATEWAY_SENSOR_URL = "http://192.168.4.1/api/sensor";
static const char*    GATEWAY_DIAG_URL   = "http://192.168.4.1/api/diag";
static const uint32_t WINDOW_MS          = 5000;  
static const uint32_t PING_INTERVAL_MS   = 500;    
static const uint32_t RECONNECT_MS       = 10000;
static const float    DIST_MIN_CM        = 2.0f;
static const float    DIST_MAX_CM        = 400.0f;
static const int      MAX_PINGS          = WINDOW_MS / PING_INTERVAL_MS + 2;

WebServer server(80);
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

struct PingWindow {
  int      n = 0, valid = 0, timeouts = 0, outOfRange = 0, echoStuckHigh = 0;
  uint32_t echoUs[MAX_PINGS];  
  float    cm[MAX_PINGS];       
} win;

uint32_t lastReconnectMs = 0, lastPingMs = 0, windowStartMs = 0;
int      lastSensorHttp = 0;     
uint32_t wifiDrops = 0;
int      lastDiscReason = 0;

static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[WIFI ] Joined gateway AP '%s': my IP %s, gateway %s\n",
                    GATEWAY_SSID, WiFi.localIP().toString().c_str(),
                    WiFi.gatewayIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      wifiDrops++;
      lastDiscReason = info.wifi_sta_disconnected.reason;
      Serial.printf("[WIFI ] Disconnected from gateway (reason %d%s)\n", lastDiscReason,
                    lastDiscReason == 201 ? ": AP not found, check gateway is on and SSID matches" :
                    lastDiscReason == 202 || lastDiscReason == 15 ? ": wrong password" : "");
      break;
    default:
      break;
  }
}

static void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastReconnectMs >= RECONNECT_MS) {
    lastReconnectMs = millis();
    Serial.println("[WIFI ] Not connected, retrying gateway AP...");
    WiFi.disconnect();
    WiFi.begin(GATEWAY_SSID, GATEWAY_PASSWORD);
  }
}

static void takePing() {
  if (win.n >= MAX_PINGS) return;
  bool stuckHigh = digitalRead(ECHO_PIN) == HIGH;   
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long us = pulseIn(ECHO_PIN, HIGH, 30000UL); 

  win.echoUs[win.n++] = us;
  if (stuckHigh) win.echoStuckHigh++;
  if (us == 0) { win.timeouts++; return; }
  float d = us * 0.0343f / 2.0f;
  if (d < DIST_MIN_CM || d > DIST_MAX_CM) { win.outOfRange++; return; }
  win.cm[win.valid++] = d;
}

static void windowStats(float& mn, float& med, float& mx) {
  mn = med = mx = NAN;
  if (win.valid == 0) return;
  float s[MAX_PINGS];
  for (int i = 0; i < win.valid; i++) s[i] = win.cm[i];
  for (int i = 0; i < win.valid; i++)
    for (int j = i + 1; j < win.valid; j++)
      if (s[j] < s[i]) { float t = s[i]; s[i] = s[j]; s[j] = t; }
  mn = s[0];
  med = s[win.valid / 2];
  mx = s[win.valid - 1];
}

static String diagnosisHint(float mn, float mx) {
  if (win.n == 0) return "no pings taken";
  if (win.echoStuckHigh == win.n)
    return "ECHO pin already HIGH before every ping: wrong ECHO pin, TRIG/ECHO swapped, or bad divider";
  if (win.timeouts == win.n)
    return "no echo on any ping: check TRIG/ECHO pin numbers (or swapped), sensor VCC on 5V not 3V3, common GND";
  if (win.valid == 0)
    return "echoes received but all outside 2-400 cm: target too close, or noisy wiring";
  if (lastSensorHttp < 0)
    return "sensor OK but cannot reach gateway at 192.168.4.1: check Wi-Fi / gateway is running";
  if (lastSensorHttp >= 400)
    return "sensor OK but gateway rejected the reading (HTTP " + String(lastSensorHttp) + ")";
  if (win.timeouts + win.outOfRange > 0)
    return "intermittent echoes: aim at a flat, hard surface 10-100 cm away";
  if (mx - mn > 10.0f)
    return "readings unstable (spread " + String(mx - mn, 1) + " cm): keep sensor and target still";
  return "OK";
}

static int postJson(const char* url, const String& body, String& resp) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);   // gateway may be busy with an HTTPS call for a moment
  if (!http.begin(client, url)) { resp = "begin failed"; return -1000; }
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  resp = code > 0 ? http.getString() : http.errorToString(code);
  http.end();
  return code;
}

static void sendReading(float distanceCm) {
  JsonDocument doc;
  doc["node"] = NODE_ID;
  doc["distance"] = serialized(String(distanceCm, 1));  
  String body, resp;
  serializeJson(doc, body);
  lastSensorHttp = postJson(GATEWAY_SENSOR_URL, body, resp);
  Serial.printf("[HTTP ] POST /api/sensor %s -> %d %s\n", body.c_str(), lastSensorHttp, resp.c_str());
}

static void sendDiag(float mn, float med, float mx, const String& hint) {
  JsonDocument d;
  d["node"] = NODE_ID;
  d["ip"] = WiFi.localIP().toString();
  d["rssi"] = WiFi.RSSI();
  d["uptime_s"] = millis() / 1000;
  d["free_heap"] = ESP.getFreeHeap();
  d["wifi_drops"] = wifiDrops;
  d["last_disc_reason"] = lastDiscReason;
  d["trig_pin"] = TRIG_PIN;
  d["echo_pin"] = ECHO_PIN;
  d["window_ms"] = WINDOW_MS;
  d["pings"] = win.n;
  d["valid"] = win.valid;
  d["timeouts"] = win.timeouts;
  d["out_of_range"] = win.outOfRange;
  d["echo_stuck_high"] = win.echoStuckHigh;
  JsonArray a = d["echo_us"].to<JsonArray>();
  for (int i = 0; i < win.n; i++) a.add(win.echoUs[i]);
  if (win.valid > 0) {
    d["min_cm"] = serialized(String(mn, 1));
    d["median_cm"] = serialized(String(med, 1));
    d["max_cm"] = serialized(String(mx, 1));
  }
  d["last_sensor_http"] = lastSensorHttp;
  d["hint"] = hint;

  String body, resp;
  serializeJson(d, body);
  int code = postJson(GATEWAY_DIAG_URL, body, resp);
  if (code != 200) Serial.printf("[DIAG ] report not delivered: %d %s\n", code, resp.c_str());
}

static void endWindow() {
  float mn, med, mx;
  windowStats(mn, med, mx);

  String us;
  for (int i = 0; i < win.n; i++) { us += String(win.echoUs[i]); us += ' '; }
  Serial.printf("[SENSOR] %d pings: %d valid, %d timeout, %d out-of-range, %d echo-stuck-high | "
                "min/median/max %s/%s/%s cm\n",
                win.n, win.valid, win.timeouts, win.outOfRange, win.echoStuckHigh,
                win.valid ? String(mn, 1).c_str() : "-", win.valid ? String(med, 1).c_str() : "-",
                win.valid ? String(mx, 1).c_str() : "-");
  Serial.printf("[SENSOR] echo_us: %s\n", us.c_str());

  bool online = WiFi.status() == WL_CONNECTED;
  if (!online) {
    Serial.println("[WIFI ] Not connected to gateway, nothing sent this window");
  } else if (win.valid > 0) {
    sendReading(med);
  } else {
    lastSensorHttp = 0;
    Serial.println("[SENSOR] No valid echo in this window, reading not sent");
  }

  String hint = diagnosisHint(mn, mx);
  Serial.printf("[DIAG ] hint: %s\n", hint.c_str());
#if ENABLE_DIAG
  if (online) sendDiag(mn, med, mx, hint);
#endif

  win = PingWindow();  
}

static void sendJsonResp(int code, const JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

static void sendError(int code, const char* msg) {
  JsonDocument doc;
  doc["status"] = "error";
  doc["error"] = msg;
  sendJsonResp(code, doc);
  Serial.printf("[LED  ] %d %s\n", code, msg);
}

static bool getIntInRange(JsonVariantConst v, int lo, int hi, int& out) {
  if (!v.is<int>()) return false;   
  out = v.as<int>();
  return out >= lo && out <= hi;
}

static void handleLedPost() {
  if (!server.hasArg("plain") || server.arg("plain").length() == 0) {
    sendError(400, "empty body");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    sendError(400, "invalid JSON");
    return;
  }

  int idx, r, g, b;
  if (!getIntInRange(doc["index"], 0, NUM_LEDS - 1, idx)) {
    sendError(400, "'index' must be an integer 0-7");
    return;
  }
  if (!getIntInRange(doc["r"], 0, 255, r) ||
      !getIntInRange(doc["g"], 0, 255, g) ||
      !getIntInRange(doc["b"], 0, 255, b)) {
    sendError(400, "'r','g','b' must be integers 0-255");
    return;
  }

  strip.setPixelColor(idx, strip.Color(r, g, b));   
  strip.show();
  Serial.printf("[LED  ] LED %d set to (%d,%d,%d) by %s\n", idx, r, g, b,
                server.client().remoteIP().toString().c_str());

  JsonDocument ack;
  ack["status"] = "ok";
  ack["index"] = idx;
  ack["r"] = r;
  ack["g"] = g;
  ack["b"] = b;
  sendJsonResp(200, ack);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== EE4216 Lab 3 Task 4: Sensor node (full) ===");
  Serial.printf("[INFO ] TRIG=GPIO%d ECHO=GPIO%d LED=GPIO%d\n", TRIG_PIN, ECHO_PIN, LED_PIN);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  strip.begin();
  strip.setBrightness(LED_BRIGHTNESS);
  strip.clear();
  strip.show();

  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(GATEWAY_SSID, GATEWAY_PASSWORD);
  lastReconnectMs = windowStartMs = millis();

  server.on("/api/led", HTTP_POST, handleLedPost);
  server.on("/api/led", HTTP_ANY, []() { sendError(405, "use POST"); });
  server.onNotFound([]() { sendError(404, "not found"); });
  server.begin();
  Serial.println("[HTTP ] Sensor node server listening on port 80");
}

void loop() {
  server.handleClient();
  maintainWiFi();
  uint32_t now = millis();
  if (now - lastPingMs >= PING_INTERVAL_MS) {
    lastPingMs = now;
    takePing();
  }
  if (now - windowStartMs >= WINDOW_MS) {
    windowStartMs = now;
    endWindow();
    server.handleClient();
  }
  delay(2);
}
