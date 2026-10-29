#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>
#include "config.h"
#include "thingspeak_ca.h"

WebServer server(80);
Preferences preferences;

float latestDistanceCm = 0.0f;
unsigned long lastSensorReceiveMs = 0;
bool hasValidReading = false;
IPAddress sensorIP;
bool sensorIPKnown = false;

unsigned long lastUploadMs = 0;
unsigned long lastPollMs = 0;
unsigned long lastWifiRetryMs = 0;
uint32_t lastCompletedEntryId = 0;
bool ntpStarted = false;
wl_status_t previousStaStatus = WL_IDLE_STATUS;

void sendJsonError(int code, const char* message) {
  JsonDocument doc;
  doc["error"] = message;
  String body;
  serializeJson(doc, body);
  server.send(code, "application/json", body);
}

// Task 2: the local HTTP server accepts data from Sensor Node A.
void handleSensorPost() {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error || !doc.is<JsonObject>()) {
    sendJsonError(400, "Malformed JSON object");
    return;
  }

  JsonVariant val = doc["distance"];
  if (!(val.is<int>() || val.is<long>() || val.is<float>() || val.is<double>())) {
    sendJsonError(400, "distance must be a number");
    return;
  }
  float d = val.as<float>();
  if (!isfinite(d) || d <= 0.0f || d > 400.0f) {
    sendJsonError(400, "distance must be within 0..400 cm, excluding zero");
    return;
  }

  // Only after validation: store value, uptime timestamp and actual source IP.
  latestDistanceCm = d;
  lastSensorReceiveMs = millis();
  hasValidReading = true;
  sensorIP = server.client().remoteIP(); // Sensor node's DHCP address on AP.
  sensorIPKnown = true;

  Serial.printf("[Task 2] sensor distance=%.1f cm, from %s, received ms=%lu\n",
                d, sensorIP.toString().c_str(), lastSensorReceiveMs);
  JsonDocument ack;
  ack["accepted"] = true;
  ack["distance"] = d;
  String body;
  serializeJson(ack, body);
  server.send(200, "application/json", body);
}

// Optional local diagnostics endpoint; reachable through gateway AP even offline.
void handleStatusGet() {
  JsonDocument doc;
  doc["gateway_ap_ip"] = WiFi.softAPIP().toString();
  doc["upstream_sta_connected"] = (WiFi.status() == WL_CONNECTED);
  doc["gateway_sta_ip"] = WiFi.localIP().toString();
  doc["sensor_ip"] = sensorIPKnown ? sensorIP.toString() : "unknown";
  doc["has_valid_reading"] = hasValidReading;
  if (hasValidReading) {
    doc["distance_cm"] = latestDistanceCm;
    doc["reception_uptime_ms"] = lastSensorReceiveMs;
    doc["reading_age_ms"] = millis() - lastSensorReceiveMs;
  } else {
    doc["distance_cm"] = nullptr;
  }
  doc["last_completed_entry_id"] = lastCompletedEntryId;
  String body;
  serializeJson(doc, body);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

bool cloudReady() {
  if (WiFi.status() != WL_CONNECTED) return false;
  // TLS X.509 expiration checking requires real wall-clock time.
  if (time(nullptr) < 1700000000) {
    Serial.println("[TLS] Waiting for NTP clock synchronization");
    return false;
  }
  return true;
}

// Task 3: send latest fresh, valid distance to ThingSpeak every ~20 seconds.
void uploadSensorToCloud() {
  if (!cloudReady() || !hasValidReading) return;
  if (millis() - lastSensorReceiveMs > 30000UL) {
    Serial.println("[Task 3] Sensor reading stale (>30s); skip upload");
    return;
  }
  WiFiClientSecure secureClient;
  secureClient.setCACert(THINGSPEAK_CA_PEM); // Verify public server cert; never setInsecure().
  secureClient.setTimeout(3500);
  HTTPClient http;
  http.setTimeout(3500);
  if (!http.begin(secureClient, "https://api.thingspeak.com/update")) {
    Serial.println("[Task 3] HTTPS client initialization failed");
    return;
  }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String form = "api_key=" + String(SENSOR_WRITE_API_KEY)
              + "&field1=" + String(latestDistanceCm, 1);
  int status = http.POST(form);
  String response = (status > 0) ? http.getString() : "";
  http.end();

  // ThingSpeak indicates a stored update with a POSITIVE entry ID, not just HTTP 200.
  long entry = response.toInt();
  if (status == 200 && entry > 0) {
    Serial.printf("[Task 3] Cloud stored %.1f cm, entry ID=%ld\n",
                  latestDistanceCm, entry);
  } else {
    Serial.printf("[Task 3] Upload NOT stored. HTTP=%d, body='%s'\n",
                  status, response.c_str());
  }
}

bool parseDecimalField(const char* text, long minVal, long maxVal, int& parsed) {
  if (text == nullptr || *text == '\0') return false;
  for (const char* p = text; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
  }
  char* end = nullptr;
  long n = strtol(text, &end, 10);
  if (*end != '\0' || n < minVal || n > maxVal) return false;
  parsed = (int)n;
  return true;
}

// Task 4: forward cloud command over LOCAL HTTP to Sensor Node A.
bool forwardLedCommand(int index, int colorCode) {
  if (!sensorIPKnown) {
    Serial.println("[Task 4] Sensor IP unknown; wait for first POST /api/sensor");
    return false;
  }

  int r = (colorCode == 1) ? 255 : 0;
  int g = (colorCode == 2) ? 255 : 0;
  int b = (colorCode == 3) ? 255 : 0;

  JsonDocument cmd;
  cmd["index"] = index;
  cmd["r"] = r;
  cmd["g"] = g;
  cmd["b"] = b;
  String body;
  serializeJson(cmd, body);

  WiFiClient localClient;
  HTTPClient http;
  http.setTimeout(2500);
  String url = "http://" + sensorIP.toString() + "/api/led";
  if (!http.begin(localClient, url)) {
    Serial.println("[Task 4] Cannot initialize sensor HTTP client");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  int status = http.POST(body);
  String reply = (status > 0) ? http.getString() : "";
  http.end();

  if (status != 200) {
    Serial.printf("[Task 4] Sensor HTTP error=%d, response=%s\n",
                  status, reply.c_str());
    return false;
  }
  Serial.printf("[Task 4] Sensor HTTP Status: %d\n", status);
  Serial.print("[Task 4] Sensor Response: ");
  Serial.println(reply);
  
  JsonDocument ack;
  if (deserializeJson(ack, reply) || !ack.is<JsonObject>()) return false;
  bool acknowledged = ack["success"].is<bool>() && ack["success"].as<bool>()
                   && ack["index"].is<int>() && ack["index"].as<int>() == index
                   && ack["r"].is<int>() && ack["r"].as<int>() == r
                   && ack["g"].is<int>() && ack["g"].as<int>() == g
                   && ack["b"].is<int>() && ack["b"].as<int>() == b;
  if (!acknowledged) Serial.println("[Task 4] Invalid sensor acknowledgment");
  return acknowledged;
}

// Task 4: HTTPS client polls latest LED channel entry roughly every 5 seconds.
void pollCloudForLedCommand() {
  if (!cloudReady()) return;

  String url = "https://api.thingspeak.com/channels/"
      + String(LED_CONTROL_CHANNEL_ID)
      + "/feeds/last.json?api_key=" + String(LED_CONTROL_READ_API_KEY);

  WiFiClientSecure secureClient;
  secureClient.setCACert(THINGSPEAK_CA_PEM);
  secureClient.setTimeout(3500);
  HTTPClient http;
  http.setTimeout(3500);
  if (!http.begin(secureClient, url)) {
    Serial.println("[Task 4] Cloud HTTPS client initialization failed");
    return;
  }
  int status = http.GET();
  String response = (status > 0) ? http.getString() : "";
  http.end();

  if (status != 200) {
    Serial.printf("[Task 4] Cloud poll HTTP=%d (check Internet, TLS, channel/key)\n",
                  status);
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, response) || !doc.is<JsonObject>()) {
    Serial.println("[Task 4] Cloud supplied malformed JSON");
    return;
  }

  long entryId = doc["entry_id"].as<long>();
  const char* indexText = doc["field1"].as<const char*>();
  const char* colorText = doc["field2"].as<const char*>();
  int index = -1, colorCode = -1;
  if (entryId <= 0 ||
      !parseDecimalField(indexText, 0, 7, index) ||
      !parseDecimalField(colorText, 0, 3, colorCode)) {
    Serial.println("[Task 4] Missing/invalid entry ID, LED index or color code");
    return;
  }

  if ((uint32_t)entryId <= lastCompletedEntryId) return; // Do not replay completed command.

  Serial.printf("[Task 4] New cloud entry=%ld: LED %d, color code=%d\n",
                entryId, index, colorCode);
  if (forwardLedCommand(index, colorCode)) {
    // Mark completed ONLY after the sensor acknowledges successful actuation.
    lastCompletedEntryId = (uint32_t)entryId;
    preferences.putULong("doneId", lastCompletedEntryId); // Survives ESP32 reset.
    Serial.printf("[Task 4] LED actuation ACK; completed entry=%lu\n",
                  (unsigned long)lastCompletedEntryId);
  } else {
    Serial.println("[Task 4] Not acknowledged; will retry latest command next poll");
  }
}

void setup() {
  Serial.begin(115200);
  delay(250);

  preferences.begin("ee4216l3", false);
  lastCompletedEntryId = preferences.getULong("doneId", 0UL);

  WiFi.mode(WIFI_AP_STA);
  bool apStarted = WiFi.softAP(GATEWAY_AP_SSID, GATEWAY_AP_PASSWORD);
  if (!apStarted) Serial.println("[Task 1] ERROR starting gateway AP");
  Serial.print("[Task 1] Gateway AP IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.print("[Task 1] Gateway AP SSID: ");
  Serial.println(GATEWAY_AP_SSID);

  WiFi.setAutoReconnect(true);
  WiFi.begin(UPSTREAM_WIFI_SSID, UPSTREAM_WIFI_PASSWORD);
  Serial.println("[Task 1] Connecting STA upstream asynchronously...");

  server.on("/api/sensor", HTTP_POST, handleSensorPost);
  server.on("/api/status", HTTP_GET, handleStatusGet);
  server.onNotFound([]() { sendJsonError(404, "Unknown endpoint"); });
  server.begin();
  Serial.println("[Task 2] Gateway HTTP server listening on port 80");

  lastUploadMs = millis();
  lastPollMs = millis();
}

void loop() {
  server.handleClient(); // Local sensor access always checked first.
  unsigned long now = millis();
  wl_status_t staStatus = WiFi.status();

  if (staStatus != previousStaStatus) {
    previousStaStatus = staStatus;
    if (staStatus == WL_CONNECTED) {
      Serial.print("[Task 1] Gateway STA IP: ");
      Serial.println(WiFi.localIP());
      if (!ntpStarted) {
        configTime(0, 0, "pool.ntp.org", "time.google.com");
        ntpStarted = true; // Start NTP for TLS certificate date verification.
      }
    } else {
      Serial.println("[Task 1] Upstream STA disconnected; local AP stays active");
    }
  }

  // Never stay inside an infinite reconnect while loop.
  if (staStatus != WL_CONNECTED && now - lastWifiRetryMs >= 10000UL) {
    lastWifiRetryMs = now;
    WiFi.reconnect();
  }
  if (now - lastUploadMs >= CLOUD_UPLOAD_INTERVAL_MS) {
    lastUploadMs = now;
    uploadSensorToCloud();
  }
  if (now - lastPollMs >= CLOUD_POLL_INTERVAL_MS) {
    lastPollMs = now;
    pollCloudForLedCommand();
  }
}
