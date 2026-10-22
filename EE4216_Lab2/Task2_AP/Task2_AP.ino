#include <WiFi.h>                      // ESP32 Wi-Fi functionality
const char* AP_SSID = "A0272092H"; // Replace with your matric number
const char* AP_PASSWORD = "hinloknus";   // Minimum 8 characters
void setup() {
  Serial.begin(115200);               // Start Serial Monitor
  delay(1000);
  WiFi.mode(WIFI_AP);                  // ESP32 creates its own network
  bool ok = WiFi.softAP(AP_SSID, AP_PASSWORD, 6, 0, 2);
  // Arguments: SSID, password, channel 6, visible, maximum 2 clients
  if (!ok) { Serial.println("AP setup failed"); return; }
  Serial.println("ESP32 AP ready");
  Serial.print("AP SSID: "); Serial.println(AP_SSID);
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
  Serial.print("AP MAC: "); Serial.println(WiFi.softAPmacAddress());
}
void loop() {
  Serial.print("Connected stations: ");
  Serial.println(WiFi.softAPgetStationNum()); // Count currently connected devices
  delay(2000);                          // Refresh every 2 seconds
}
