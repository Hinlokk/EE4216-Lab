#include <WiFi.h>                    // ESP32 Wi-Fi functionality
const char* SSID = "TP-Link_6FCB"; // Replace with hotspot / lab SSID
const char* PASSWORD = "72972786"; // Replace with Wi-Fi password
const unsigned long TIMEOUT_MS = 15000; // Stop connecting after 15 seconds

void setup() {
  Serial.begin(115200);             // Start Serial Monitor output
  delay(1000);                      // Allow serial connection to settle
  WiFi.mode(WIFI_STA);              // Join an existing Wi-Fi network
  WiFi.begin(SSID, PASSWORD);       // Begin connecting
  unsigned long started = millis(); // Remember start time
  while (WiFi.status() != WL_CONNECTED && millis() - started < TIMEOUT_MS) {
    delay(300);                     // Allow connection attempt to progress
    Serial.print(".");              // Indicate progress
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) { // Check whether association succeeded
    Serial.println("STA connected!");
    Serial.print("SSID: "); Serial.println(WiFi.SSID());
    Serial.print("RSSI (dBm): "); Serial.println(WiFi.RSSI());
    Serial.print("STA IP: "); Serial.println(WiFi.localIP());
    Serial.print("STA MAC: "); Serial.println(WiFi.macAddress());
    Serial.print((int)WiFi.status());
  } else {
    Serial.print("Connection failed or timed out. WiFi status code: ");
    Serial.println((int)WiFi.status());
    Serial.println("Check 2.4 GHz hotspot, SSID, password and signal.");
  }
}
void loop() {}                       // No repeating action required
