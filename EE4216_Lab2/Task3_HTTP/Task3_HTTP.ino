#include <WiFi.h>                   // Wireless networking
#include <WebServer.h>              // Synchronous HTTP server
#include <Adafruit_NeoPixel.h>      // RGB LED bar control
#include <ArduinoJson.h>            // Parse and generate JSON
#include "lab_page.h"              // Supplied dashboard stored in flash

// CHANGE THESE THREE PINS to match your working Lab 1 circuit.
#define TRIG_PIN 5                  // Ultrasonic TRIG GPIO (example)
#define ECHO_PIN 18                 // Ultrasonic ECHO GPIO (via voltage divider)
#define LED_PIN 4                   // NeoPixel DATA GPIO (example)
#define LED_COUNT 8                 // Eight independently controlled pixels

const char* AP_SSID = "A0272092H"; // Replace with matric number
const char* AP_PASSWORD = "hinloknus";   // At least 8 characters
WebServer server(80);                // HTTP port 80
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

struct LedState {                    // Preserve unscaled logical LED settings
  uint8_t r, g, b, brightness;
};
LedState leds[LED_COUNT] = {};       // Initialize eight LEDs to off

void sendError(int status, const char* message) {
  JsonDocument doc;                  // Build error JSON
  doc["error"] = message;
  String payload;
  serializeJson(doc, payload);
  server.send(status, "application/json", payload);
}

void addLedJson(JsonObject obj, int id) {
  obj["id"] = id;                     // API index (0..7)
  obj["r"] = leds[id].r;              // Original requested red
  obj["g"] = leds[id].g;              // Original requested green
  obj["b"] = leds[id].b;              // Original requested blue
  obj["brightness"] = leds[id].brightness; // Original requested brightness
}

void applyStrip() {
  for (int i = 0; i < LED_COUNT; i++) {
    // Scale every channel by its own brightness / 255.
    uint8_t r = (uint16_t(leds[i].r) * leds[i].brightness) / 255;
    uint8_t g = (uint16_t(leds[i].g) * leds[i].brightness) / 255;
    uint8_t b = (uint16_t(leds[i].b) * leds[i].brightness) / 255;
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  strip.show();                      // Send all eight pixels to the hardware
}

void handleDistance() {
  // One ultrasonic reading per HTTP request; no continuous sampling.
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);            // 10 microsecond trigger pulse
  digitalWrite(TRIG_PIN, LOW);
  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000UL);
  // 30 ms timeout prevents indefinitely waiting for a missing echo.
  JsonDocument doc;
  bool valid = duration > 0;
  doc["valid"] = valid;
  if (valid) doc["distance_cm"] = duration * 0.0343f / 2.0f;
  else doc["distance_cm"] = nullptr; // JSON null, not zero
  String payload;
  serializeJson(doc, payload);
  server.sendHeader("Cache-Control", "no-store"); // Disable cached measurements
  server.send(200, "application/json", payload);
}

void handleGetLeds() {
  JsonDocument doc;
  JsonArray array = doc["leds"].to<JsonArray>();
  for (int i = 0; i < LED_COUNT; i++) {
    JsonObject obj = array.add<JsonObject>();
    addLedJson(obj, i);
  }
  String payload;
  serializeJson(doc, payload);
  server.send(200, "application/json", payload);
}

void handlePutLed(int id) {
  if (id < 0 || id >= LED_COUNT) { sendError(404, "Unknown LED ID"); return; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err || !doc.is<JsonObject>()) { sendError(400, "Malformed JSON object"); return; }
  JsonObject obj = doc.as<JsonObject>();
  const char* keys[] = {"r", "g", "b", "brightness"};
  int values[4];
  for (int i = 0; i < 4; i++) {
    JsonVariant v = obj[keys[i]];
    if (!v.is<int>()) { sendError(400, "Fields must be integers"); return; }
    int n = v.as<int>();
    if (n < 0 || n > 255) { sendError(400, "Fields must be 0..255"); return; }
    values[i] = n;                 // Validate everything before changing state
  }
  leds[id] = {uint8_t(values[0]), uint8_t(values[1]),
              uint8_t(values[2]), uint8_t(values[3])};
  applyStrip();                    // Commit changed pixel while preserving others
  JsonDocument response;
  JsonObject result = response.to<JsonObject>();
  addLedJson(result, id);
  String payload;
  serializeJson(response, payload);
  server.send(200, "application/json", payload);
}

void routePutLeds() {
  // The supplied dashboard calls PUT /api/leds/0 ... /api/leds/7.
  String path = server.uri();
  if (path.startsWith("/api/leds/")) {
    String idText = path.substring(10); // Length of "/api/leds/" is 10
    if (!idText.length()) { sendError(404, "Unknown LED ID"); return; }
    for (unsigned int i = 0; i < idText.length(); i++) {
      if (!isDigit(idText[i])) { sendError(404, "Unknown LED ID"); return; }
    }
    int id = idText.toInt();
    if (id >= LED_COUNT) { sendError(404, "Unknown LED ID"); return; }
    handlePutLed(id);
    return;
  }
  sendError(404, "Unknown endpoint");
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);
  strip.begin();
  strip.clear();
  strip.show();                     // Initialize all eight LEDs off

  WiFi.mode(WIFI_AP);                // AP works without external internet
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD, 6, 0, 2)) {
    Serial.println("AP creation failed"); return;
  }
  Serial.print("Dashboard URL: http://");
  Serial.print(WiFi.softAPIP());
  Serial.println("/");

  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", LAB_PAGE); // Serve supplied dashboard
  });
  server.on("/api/distance", HTTP_GET, handleDistance);
  server.on("/api/leds", HTTP_GET, handleGetLeds);
  server.on("/api/led", HTTP_GET, handleGetLeds); // PDF alias
  server.on("/api/led", HTTP_PUT, []() {
    // PDF-style optional alias: PUT /api/led?id=0 with JSON body
    if (!server.hasArg("id")) { sendError(400, "Missing id"); return; }
    String value = server.arg("id");
    if (!value.length()) { sendError(400, "Missing id"); return; }
    for (unsigned int i = 0; i < value.length(); i++) {
      if (!isDigit(value[i])) { sendError(404, "Unknown LED ID"); return; }
    }
    int id = value.toInt();
    if (id >= LED_COUNT) { sendError(404, "Unknown LED ID"); return; }
    handlePutLed(id);
  });
  server.onNotFound([]() {
    if (server.method() == HTTP_PUT) routePutLeds();
    else sendError(404, "Unknown endpoint");
  });
  server.begin();                   // Start HTTP server
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();           // Process queued HTTP requests continuously
}
